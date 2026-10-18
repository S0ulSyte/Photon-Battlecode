// bot.cpp - the turn dispatcher: what a champion does every turn, in order. This is the state machine of transition_table.md.
//
//   Before the states (every turn):  P0 update the memory (enemies, camps, the logged fight), P1 dead? (only shop), P2 channeling or stunned? (do
//                                    nothing), P3 shop when at spawn, P4 low-HP recall.
//   1 STANDARD COMBAT  state_combat.cpp       an enemy is known and standardCombatAllowed: retreat from an awake camp, else FIGHT CHECK.
//   2 BANK             state_bank.cpp         carrying gems (see bank_wanted): go to an altar, recall first if none is in sight.
//   3 CAMP ATTACK      state_camp_attack.cpp  the target camp was chosen for ATTACK and is in sight (or we are capturing / returning from it).
//   4                  (here)                 if standardCombatAllowed is false, set it true and carry on.
//   5 DEFAULT          state_default.cpp      walk the route to the target camp, help allies, capture when the camp is cleared.
// The first state that handles the turn wins. Choices where the spec was silent are marked "DEFAULT:" and listed in transition_table.md (D1-D32).
//
// Reads: everything (through the other files). Writes: the flags that connect the states (standard_combat_allowed_, bank_*), the act counters.
// Edit here to change: the ORDER of the states, what happens on a dead / channeling / stunned turn, when the low-HP recall starts, how a state's
// engine error is reported (guarded).
#include <algorithm>
#include "ability_util.h"
#include "bot.h"
#include "champion_util.h"
#include "config.h"
#include "fuel_profile.h"
#include "log.h"
#include "moves.h"

// STEP P4 (transition_table.md): start a recall when health is below cfg::kLowHpPercent, no enemy is known and I am not at spawn.
// Inputs: none. Result: true if a recall was started (the turn is over). Side effects: the recall. Fuel: ~60.
bool Bot::low_hp_recall() {
    FUEL_SCOPE(FP_LOW_HP);
    auto& me = cambc::self();
    if (!enemies_.empty())  // (refactor: the free test first; same result)
        return false;
    const auto record = snapshot_of(me);  // (refactor: one record for health and max health)
    if (record.health * 100.0f >= record.max_health * cfg::kLowHpPercent || at_spawn())
        return false;
    me.recall();
    return true;
}

// What: the first lines of every turn: read the round, start the fuel bookkeeping, run init() on the very first turn.
// Inputs: none. Result: none. Side effects: round_, fuel_start_, fuel_floor_, act_before_, (profile builds) turn_start_totals_, init(). Fuel: ~20.
void Bot::begin_turn() {
    round_ = static_cast<int>(cambc::round_number());
    fuel_start_ = cambc::fuel_remaining();
    fuel_floor_ = fuel_start_;  // (refactor) start of the resume detection for this turn
#if V2_FUEL_PROFILE
    for (int k = 0; k < FP_COUNT; ++k)
        turn_start_totals_[k] = g_fuel_stats[k].total;  // (profile) so finish_turn can say which pieces made a heavy turn heavy
#endif
    if (!initialised_)
        init();
    for (int k = 0; k < 7; ++k)
        act_before_[k] = acts_[k];
}

// STEP P0 (transition_table.md): update the enemy memory, the camps seen, and the logged fight; the fuel of each is recorded in state_fuel_max_.
// Inputs: none. Result: none. Side effects: enemies_, camps_seen_, seen_now_, eng_, dead_ids_, state_fuel_max_[6..8]. Fuel: ~2,500.
void Bot::update_memory() {
    auto prof = [&](int slot, auto&& f) {
        const std::uint64_t a = cambc::fuel_remaining();
        f();
        const std::uint64_t b = cambc::fuel_remaining();
        if (a >= b) state_fuel_max_[slot] = std::max(state_fuel_max_[slot], a - b);
    };
    prof(6, [&] { update_enemies(); });
    prof(7, [&] { update_camps(); });
    prof(8, [&] { update_engagement(); });
    note_fuel();
}

// What: keep the deposit latch (DEFAULT D20) and the BANK flags in step with the gems carried. Why: bank_wanted() reads the latch, so it must be
// updated before bank_wanted() is used. Inputs: none. Result: none.
// Side effects: bank_latched_, bank_phase_, bank_walking_. Fuel: ~250.
void Bot::update_bank_flags() {
#if V2_CHANGE_DEPOSIT_AT_SPAWN
    // DEFAULT (D20): start the latch when we stand on spawn with any gem; drop it when the bag is empty (the tribute is done).
    if (cambc::self().gem_count() == 0)
        bank_latched_ = false;
    else if (at_spawn())
        bank_latched_ = true;
#endif
    if (!bank_wanted()) {  // (was: gems <= threshold; change 3 makes it the same test as the state 2 entry)
        bank_phase_ = 0;
        bank_walking_ = false;
    }
}

// STEP P1 (transition_table.md): a dead champion still gets a turn, but only to shop. DEFAULT D4: dying clears every transient flag (the spec never
// says): otherwise a stale `capturing_` or `returning_` would send the respawned champion straight back into state 3 with nothing to do.
// Inputs: none. Result: none. Side effects: resets the flags, shop(). Fuel: ~1,300.
void Bot::act_dead() {
    bank_walking_ = false;
    bank_latched_ = false;  // change 3: a dead champion has no bag
    bank_phase_ = 0;
    standard_combat_allowed_ = true;
    returning_ = false;
    capturing_ = false;
    camp_engaged_turns_ = 0;
    camp_pred_made_ = -1;
    shop();
}

// STEP P2 (transition_table.md): leave running channels (recall, tribute, teleport) alone, and skip turns where we can neither move nor attack
// (stunned). Inputs: none. Result: true if this turn must do nothing. Fuel: ~100 (one record instead of up to five separate state queries).
bool Bot::cannot_act() {
    const auto my_record = snapshot_of(cambc::self());
    return has_flag(my_record, cambc::champion_state::recalling) || has_flag(my_record, cambc::champion_state::submitting_gems) ||
           has_flag(my_record, cambc::champion_state::teleporting) ||
           (!has_flag(my_record, cambc::champion_state::can_move) && !has_flag(my_record, cambc::champion_state::can_attack));
}

// What: run one state with an engine-error guard. Why: an engine error inside a state is logged with the state's name and where it happened (where_),
// and the turn simply ends, instead of the exception escaping and hiding where it came from. Also records the state's fuel.
// Inputs: the state's label ("state1".."state5" - the 6th character picks the fuel slot) and the state as a callable returning "handled".
// Result: whether the state handled the turn (true after an engine error: the turn is over). Fuel: that of the state.
template <class F>
bool Bot::guarded(const char* label, F&& run) {
    const std::uint64_t before = cambc::fuel_remaining();
    const int idx = label[5] == '1' ? 0 : label[5] == '2' ? 1 : label[5] == '3' ? 2 : 3;
    FUEL_SCOPE(static_cast<FuelPiece>(FP_STATE1 + idx));  // (the four state pieces are consecutive in the enum)
    struct Measure {
        Bot* bot; std::uint64_t before; int idx;
        ~Measure() {
            const std::uint64_t after = cambc::fuel_remaining();
            if (after <= before)
                bot->state_fuel_max_[idx] = std::max(bot->state_fuel_max_[idx], before - after);
        }
    } measure{this, before, idx};
    try {
        const bool handled = run();
        note_fuel();  // (refactor) resume detection after each state
        return handled;
    } catch (const cambc::error& error) {
        LogLine log;
        log.s("ERR ").s(name_).s(" r").n(round_).s(" in ").s(label).s("/").s(where_).s(": ").s(error.message().c_str()).s(" (code ").n(static_cast<int>(error.code())).s(")");
        log.flush();
        return true;  // the turn is over
    }
}

// What: the five states, top to bottom; the first that handles the turn wins.
// Inputs: none. Result: none. Side effects: those of the states. Fuel: that of the states (the bulk of the turn).
void Bot::run_states() {
    state_fuel_max_[5] = std::max(state_fuel_max_[5], fuel_start_ > cambc::fuel_remaining() ? fuel_start_ - cambc::fuel_remaining() : 0);
    const PlanTarget* t = target();
    bool done = false;
    if (!enemies_.empty() && standard_combat_allowed_)
        done = guarded("state1", [&] { return state_standard_combat(); });
    if (!done && bank_wanted())  // (was: gems > threshold)
        done = guarded("state2", [&] { return state_bank(); });
#if !V2_LITERAL_FLAG_RESET
    if (done && bank_wanted())  // (was: gems > threshold) a BANK turn: the same test as the state 2 entry
        standard_combat_allowed_ = true;  // DEFAULT: a turn handled by BANK is not a state-3 turn
#endif
    if (!done && (returning_ || (t && t->attack && (capturing_ || target_group() != nullptr))))
        done = guarded("state3", [&] { return state_camp_attack(); });
    if (!done) {
        // STATE 4: if standardCombatAllowed is false, set it to true and carry on.
        standard_combat_allowed_ = true;
        guarded("state5", [&] {
            state_default();
            return true;
        });
    }
}

// One turn of one champion (called by main.cpp once per round). The order of the steps is the whole design; see the header of this file.
// Inputs: none. Result: none. Fuel: ~13,000 on average, peak ~65,000-80,000 (limit 100,000).
void Bot::play_turn() {
    begin_turn();
    update_memory();       // P0
    update_bank_flags();
    if (!cambc::self().is_alive()) {  // P1
        act_dead();
        finish_turn();
        return;
    }
    if (cannot_act()) {  // P2
        ++acts_[6];
        finish_turn();
        return;
    }
    if (at_spawn())  // P3
        shop();
    if (low_hp_recall()) {  // P4
        finish_turn();
        return;
    }
    run_states();  // states 1-5
    finish_turn();
}
