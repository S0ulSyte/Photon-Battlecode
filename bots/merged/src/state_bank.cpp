// state_bank.cpp - STATE 2, BANK: carry gems to an altar and tribute them. Also the test for when BANK applies.
//
// Reads: gems, altars in sight, the altar route. Writes: bank_phase_, bank_walking_, tribute / recall / steps.
// Edit here to change: when BANK starts (bank_wanted; cfg::kGemThreshold, V2_CHANGE_DEPOSIT_AT_SPAWN), what BANK does (state_bank).
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <variant>
#include "ability_util.h"
#include "bot.h"
#include "champion_util.h"
#include "config.h"
#include "fuel_profile.h"
#include "log.h"
#include "moves.h"

using cambc::team;
using cambc::vec2;

// What: remember which part of the BANK trip I am in (1 recalling, 2 walking the altar route, 3 approaching a seen altar, 4 tributing), and log the change.
// Inputs: the new phase number and a description. Side effects: bank_phase_ and a BANK log line when the phase changes. Fuel: ~2,000 when it logs.
void Bot::set_bank_phase(int phase, const char* what) {
    if (phase == bank_phase_)
        return;
    bank_phase_ = phase;
    LogLine log;
    log.s("BANK ").s(name_).s(" r").n(round_).s(" ").s(what).s(" gems ").n(cambc::self().gem_count()).s(" at ").xy(my_pos().x, my_pos().y);
    log.flush();
}

// What: the single test for "BANK applies". Why: the threshold test used to be written out in three places of play_turn, and change 3 must
// change all three the same way. How: more than kGemThreshold gems always counts; with V2_CHANGE_DEPOSIT_AT_SPAWN also any gem (> 0) while
// standing on one of our spawn tiles (at_spawn()).
// Result: true if BANK should handle this turn. Fuel: ~100 (one gem read).
bool Bot::bank_wanted() const {
    const std::uint32_t gems = cambc::self().gem_count();
    if (gems > static_cast<std::uint32_t>(cfg::kGemThreshold))
        return true;
#if V2_CHANGE_DEPOSIT_AT_SPAWN
    // DEFAULT (D20): once a deposit trip has started (bank_latched_), it also counts while we are off the spawn tiles with a small bag.
    if (gems > 0 && (at_spawn() || bank_latched_))
        return true;
#endif
    return false;
}

// STATE 2 (transition_table.md): BANK. Entry condition: bank_wanted().
// What: an altar in sight: walk to it and tribute. Otherwise recall once per trip (D2), then walk the altar route from draft memory, using attacks and skills that do not stop movement on the way.
// STATE 2: carrying more than the threshold of gems.
// Result: true if it handled the turn (false only if there is no altar route). Side effects: tribute, walking, recall. Fuel: ~5,000 on average.
bool Bot::state_bank() {
    ++acts_[3];
    where_ = "bank";
    auto& me = cambc::self();
    const int reach = cambc::constants::submit_gems_range;
    const auto visible = cambc::vision::visible_tiles();
    const vec2 here = my_pos();
    std::optional<vec2> altar;
    for (vec2 a : cambc::map::altars().positions())
        if (visible.is_set(a) && (!altar || dist(here, a) < dist(here, *altar)))
            altar = a;
    if (altar) {
        if (dist(here, *altar) <= reach) {
            set_bank_phase(4, "tribute");
            me.submit_gems(*altar);
        } else {
            set_bank_phase(3, "altar in sight, walking to it");
            move_towards_closest(tiles_around(*altar, reach));
            opportunistic_attacks();
        }
        return true;
    }
    // No altar in sight: recall (unless we are already at spawn), then follow the route to the closest altar from the draft.
#if V2_LITERAL_BANK_RECALL
    if (!at_spawn()) {
        set_bank_phase(1, "recall");
        me.recall();
        return true;
    }
#else
    // DEFAULT: recall only until we have reached spawn once on this trip; after that, walk the route.
    if (!bank_walking_) {
        if (!at_spawn()) {
            set_bank_phase(1, "recall");
            me.recall();
            return true;
        }
        bank_walking_ = true;
    }
#endif
    if (plan_.altars.empty())
        return false;
    set_bank_phase(2, "walking the altar route");
    follow_forward(altar_tiles_, 2);  // (refactor: cached in init; was path_tiles(plan_.altars[0].path) every turn)
    opportunistic_attacks();
    return true;
}

