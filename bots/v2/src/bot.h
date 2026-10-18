// bot.h - the declaration of Bot, v2's per-champion bot, and the small structs it keeps.
//
// What it is for: one Bot object lives in each champion's program (main.cpp) and play_turn() is called once per round. The class is declared here and
// its methods are spread over several .cpp files, one topic per file (the table below), so a change to one topic touches one file.
// Reads/writes: see each file. The data members at the bottom are the "flags" of the spec's state machine (transition_table.md) plus the memory.
// Edit here to change: what the bot remembers (the members), what a piece of memory looks like (EnemyMem, CampSeen, Engagement, CampGroupInfo), or to add
// a method (declare it here, define it in the file of its topic).
//
//   bot.cpp              play_turn and the steps before the states (P0-P4), the five states' dispatcher
//   bot_setup.cpp        init (first turn), const_of
//   enemy_memory.cpp     views of champions in sight, enemy memory, logged engagements, nearest_enemy
//   camp_memory.cpp      camps seen, awake camps, camp group states
//   targets.cpp          my two targets, switching between them
//   movement.cpp         my position, spawn tests, walking a route
//   fight.cpp            FIGHT CHECK input, fight_enemy, flee, camp attacks
//   shop.cpp             buying shards
//   shared_memory.cpp    publishing my tile and health to the team
//   state_combat.cpp     state 1        state_bank.cpp    state 2        state_camp_attack.cpp  state 3        state_default.cpp   state 5
//   turn_end.cpp         fuel statistics, resume detection, the trace and FUEL log lines
#pragma once
#include <cambc.h>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include "fight_check.h"
#include "plan.h"
#include "fuel_profile.h"

// What we remember about an enemy champion: where it was last seen and what it was like then.
struct EnemyMem {
    std::uint32_t id = 0;
    cambc::champion_type type{};
    cambc::vec2 pos{0, 0};
    int health = 0, strength = 0, focus = 0, range = 1;
    int last_seen = 0;     // round number it was last seen in
    int stats_round = -1000;  // round strength/focus were last read (they only change when items are bought)
    bool visible = false;  // seen this turn
    // The champion object built while it was visible. Asking it later whether it is alive is cheap; building champion objects for
    // enemies we cannot see is very expensive.
    std::optional<cambc::champion> handle;
};

// What we have seen of one camp over time (the engine has no "awake" flag, so we infer it from health changes).
struct CampSeen {
    cambc::vec2 pos{0, 0};
    int health = 0;
    int last_change = -1000;  // round its health last changed
    int last_seen = -1000;    // round we last saw it
};

// One logged fight: the FIGHT CHECK prediction made at its start, and how it ended.
struct Engagement {
    bool active = false;
    int start_round = 0;
    bool predicted_win = false;
    int pred_kill = 0, pred_die = 0;
    int last_enemy_round = 0;
    std::vector<std::uint32_t> enemy_ids;
};

// CHANGE 9: one camp group as the detour rule needs it (read once at the start; reading all groups costs ~20k fuel).
struct CampGroupInfo {
    cambc::map::camp_group group;
    std::vector<cambc::vec2> tiles;  // the spawn tile of every member camp
    int capacity = 1;                // how many allies may farm it at once: elk 2, monolith 0, otherwise 1 (largest over its members)
};

// REFACTOR (stage 1): the champions in sight, as references to long-lived objects. A std::vector<cambc::champion> copied each champion object on
// every build (a champion object is large: abilities and consumables), which cost ~600 fuel per champion per round. Looping over this class gives
// `const cambc::champion&` exactly like the vector did, so the loops that use it did not change.
class ChampionView {
public:
    class iterator {
    public:
        explicit iterator(const cambc::champion* const* at) : at_(at) {}
        const cambc::champion& operator*() const { return **at_; }
        iterator& operator++() {
            ++at_;
            return *this;
        }
        bool operator!=(const iterator& other) const { return at_ != other.at_; }

    private:
        const cambc::champion* const* at_;
    };
    iterator begin() const { return iterator(items.data()); }
    iterator end() const { return iterator(items.data() + items.size()); }
    std::size_t size() const { return items.size(); }
    std::vector<const cambc::champion*> items;  // points into Bot::champion_cache_ (std::map nodes never move)
};

// What a piece of a state tells the state it belongs to (see state_camp_attack.cpp): the turn is over, the next states should run, or go on.
enum class CampStep { kContinue, kHandled, kPassOn };

class Bot {
public:
    // One turn of this champion (bot.cpp). Called by main.cpp once per round.
    void play_turn();

private:
    // ================================================ METHODS, by file ================================================
    // ---- bot.cpp: the steps before the states, and the dispatcher ----
    void begin_turn();
    void update_memory();
    void update_bank_flags();
    void act_dead();
    bool cannot_act();
    template <class F>
    bool guarded(const char* label, F&& run);
    void run_states();
    bool low_hp_recall();

    // ---- bot_setup.cpp ----
    void init();
    struct ChampionConst {  // the things about a champion that never change, read once per champion handle (each read is an engine call of ~100 fuel)
        cambc::champion_type type{};
        int range = 1;
        int slot = -1;          // draft slot (0 Issac ... 5 Varg), -1 if none
        bool marcella = false;  // stun-immune
    };
    const ChampionConst& const_of(const cambc::champion& c);

    // ---- enemy_memory.cpp ----
    void champions_in_sight(cambc::team side, ChampionView& out);
    const ChampionView& enemy_view();
    const ChampionView& ally_view();
    void update_enemies();
    const EnemyMem* nearest_enemy() const;
    void update_engagement();
    void end_engagement(const char* outcome);

    // ---- camp_memory.cpp ----
    void update_camps();
    std::vector<cambc::vec2> active_camps() const;
    bool group_state(const cambc::map::camp_group& g, cambc::map::camp_group_state& out);  // false when the group is not in vision
    int group_of(cambc::vec2 pos) const;  // index into groups_ of the group that owns a camp at `pos` (spawn tile within 2), or -1

    // ---- targets.cpp ----
    const PlanTarget* target() const;
    const std::vector<std::pair<int, int>>& target_tiles() const;
    const cambc::map::camp_group* target_group() const;  // nullptr when the target camp's tile is not in sight
    void switch_target(const char* why);

    // ---- movement.cpp ----
    cambc::vec2 my_pos() const;  // my tile, cached between moves (see moves.h note_moved)
    bool at_spawn() const;
    int steps_from_spawn_on(const std::vector<std::pair<int, int>>& tiles) const;
    void follow_forward(const std::vector<std::pair<int, int>>& tiles, int goals_slot);
    void follow_backward(const std::vector<std::pair<int, int>>& tiles, int goals_slot);

    // ---- fight.cpp ----
    FightResult fight_check(bool log_engagement);
    FightUnit fc_unit_me();
    void fc_add_allies(std::vector<FightUnit>& units, int& next_id);
    void fc_add_shared_allies(std::vector<FightUnit>& units, int& next_id);
    void fc_add_enemies(std::vector<FightUnit>& units);
    void fc_open_engagement(const FightResult& r, const std::vector<FightUnit>& units);
    void fight_enemy(const EnemyMem& target, bool win = false);  // `win` = FIGHT CHECK said WIN (only then may a dash be used)
    void flee();
    void retreat_from_camps(const std::vector<cambc::vec2>& active);
    void opportunistic_attacks();
    bool attack_camp(const cambc::camp& camp);

    // ---- shop.cpp, shared_memory.cpp ----
    void shop();
    void write_shared();

    // ---- state_combat.cpp (state 1), state_bank.cpp (state 2) ----
    bool state_standard_combat();
    bool state_bank();
    void set_bank_phase(int phase, const char* what);
    bool bank_wanted() const;  // whether BANK should handle this turn: gems above the threshold, or any gem on spawn / on a deposit trip

    // ---- state_camp_attack.cpp (state 3): each piece says whether the turn is over (kHandled), is passed on to the next states (kPassOn), or goes on ----
    bool state_camp_attack();
    bool do_return();
    CampStep camp_wait_for_respawn(const PlanTarget& t, const cambc::map::camp_group_state& st);
    bool camp_after_capture(const PlanTarget& t);
    bool camp_stand_on_reward(const PlanTarget& t, cambc::vec2 camp_tile);
    CampStep camp_fight_back();
    bool camp_hit_group(const PlanTarget& t, const cambc::map::camp_group& group);

    // ---- state_default.cpp (state 5) ----
    void state_default();
    bool free_slot_detour(const PlanTarget& planned);
    bool detour_continue();
    bool detour_start(const PlanTarget& planned, int planned_group);
    void help_allies_hit_camp();
    bool farm_group_step(const PlanTarget& t, cambc::vec2 camp_tile, const cambc::map::camp_group& group);

    // ---- turn_end.cpp ----
    void note_fuel();  // one fuel-counter reading for resume detection (see "resume detection" below)
    void finish_turn();
    void log_fuel_line(int step);  // one line of the FUEL log: step 0 = the main line, then chunks of the per-piece table (profile builds)

    // ================================================ DATA, by topic ================================================
    // ---- identity and plan (set in init) ----
    bool initialised_ = false;
    Plan plan_;
    bool plan_ok_ = false;
    int slot_ = -1;
    const char* name_ = "?";
    const char* where_ = "";  // last step reached inside a state, printed with any engine error
    int map_w_ = 0, map_h_ = 0;  // the map size, to pack a tile into 12 bits for shared memory
    std::optional<std::array<cambc::ability, 2>> my_abilities_;  // my two ability objects, copied once (me.abilities() copies them every call)

    // ---- my targets and routes ----
    std::vector<std::size_t> my_targets_;  // indices into plan_.targets for this champion, best first
    std::size_t target_idx_ = 0;           // which of my targets we are on
    std::optional<cambc::map::camp_group> target_groups_[2];  // the camp group of each of my targets (read once: camp_groups() costs ~20k fuel)
    std::vector<std::vector<std::pair<int, int>>> my_target_tiles_;  // path_tiles() of each entry of my_targets_, built once
    std::vector<std::pair<int, int>> altar_tiles_;                   // path_tiles() of the closest altar's route, built once
    std::vector<int> target_planned_group_;  // group_of() of each of my targets' camp tile, computed once (indexes like my_targets_)
    std::vector<std::pair<int, int>> return_tiles_;  // the route to walk back along (state 3)
    std::optional<cambc::bitboard> goals_[4];  // "all tiles of the path" boards: target 0, target 1, altar route, return route

    // ---- the spec's flags and state-3 bookkeeping ----
    bool standard_combat_allowed_ = true;  // spec flag standardCombatAllowed
    bool returning_ = false;               // heading back to spawn after a capture (state 3)
    bool capturing_ = false;               // standing on a cleared camp, waiting for the capture to finish
    int camp_engaged_turns_ = 0;           // turns we have been hitting the current target camp
    int camp_pred_death_ = -1;             // round we predicted the camp would die in
    int camp_pred_made_ = -1;
    int respawn_at_[2] = {0, 0};           // CHANGE 11: the round each of my two targets finishes respawning (0 = available)
    bool waiting_ = false;                 // CHANGE 11: waiting beside the camp that respawns first
    int wait_turns_ = 0;

    // ---- BANK ----
    bool bank_walking_ = false;  // the recall for this trip is done, we are walking the altar route
    int bank_phase_ = 0;         // 0 none, 1 recalling, 2 walking the altar route, 3 approaching a seen altar, 4 tributing
    // CHANGE 3 (DEFAULT D20): a deposit trip that began on a spawn tile with a small bag stays on until the bag is empty or we die. Without it
    // the first step off the spawn tiles makes the bag "too small" again, the default state walks back onto spawn, and the two flip forever.
    bool bank_latched_ = false;

    // ---- champions in sight and enemy memory ----
    ChampionView enemy_view_, ally_view_;  // the views, rebuilt at most once per round
    int enemy_view_round_ = -1, ally_view_round_ = -1;
    // Champion objects by engine handle. Building one costs ~800 fuel (the SDK sets up its abilities and consumables); each is now built once.
    std::map<std::uint32_t, cambc::champion> champion_cache_;
    std::map<std::uint32_t, ChampionConst> champion_const_;
    std::vector<EnemyMem> enemies_;
    std::vector<std::uint32_t> dead_ids_;  // enemy champions we know are dead
    Engagement eng_;
    int engagements_logged_ = 0;

    // ---- camps ----
    std::vector<cambc::camp> map_camps_;           // every camp of the map, kept from init (camp_groups() is read there anyway)
    std::map<int, std::size_t> map_camp_by_spawn_; // a camp's spawn tile (y * 256 + x) -> its place in map_camps_
    std::map<int, CampSeen> camps_seen_;           // every camp ever seen, by tile key
    // The camps seen THIS round, in the same order a walk over camps_seen_ would give (by tile key). Usually 0 to 3 entries.
    std::vector<const CampSeen*> seen_now_;
    // CHANGE 14: one cached answer per camp group (key = the group's id): the round it was asked in, whether it was readable, and the state.
    struct GroupStateCache {
        std::uint32_t id = 0;
        int round = -1;
        bool ok = false;
        cambc::map::camp_group_state st = cambc::map::camp_group_alive{};
    };
    std::vector<GroupStateCache> group_cache_;  // a short list searched in order (there are only a few groups per turn)
    int group_state_calls_ = 0, group_state_hits_ = 0;
    // CHANGE 9: every camp group (filled in init), the group of the detour in progress (-1 = none), how long it has lasted and from which round each
    // group is off-limits (a detour on it was abandoned).
    std::vector<CampGroupInfo> groups_;
    int detour_group_ = -1;
    int detour_turns_ = 0;
    cambc::vec2 detour_tile_{0, 0};  // the camp tile to stand on once the detour group is cleared
    std::vector<int> detour_ban_until_;

    // ---- position cache (movement.cpp) ----
    mutable cambc::vec2 pos_{0, 0};
    mutable int pos_round_ = -1, pos_epoch_ = -1;

    // ---- the round and the logs ----
    int round_ = 0;
    bool shared_error_logged_ = false;
    int ignored_enemy_turns_ = 0;  // state 3 turns with an enemy in sight that FIGHT CHECK said we would beat, yet we kept hitting the camp
    // How many turns went to each kind of action: 0 fight, 1 flee, 2 camp-retreat, 3 bank, 4 camp attack, 5 default, 6 no action (channel/stun).
    int acts_[7] = {0, 0, 0, 0, 0, 0, 0};
    int act_before_[7] = {0, 0, 0, 0, 0, 0, 0};  // copy of acts_ at the start of the turn (for the trace)
    bool was_dead_ = false;

    // ---- fuel bookkeeping (turn_end.cpp) ----
    std::uint64_t fuel_start_ = 0;       // the fuel counter at the start of this turn
    std::uint64_t max_fuel_used_ = 0;    // the most one turn used (first turn of play excluded)
    std::uint64_t total_fuel_used_ = 0;  // fuel used over all counted turns
    int fuel_turns_ = 0;                 // how many turns that is
    std::uint64_t first_turn_fuel_ = 0;  // the first turn of play has a doubled budget
    int overruns_ = 0;                   // turns whose fuel counter went UP over the turn (the old, incomplete overrun test)
    int turns_played_ = 0;
    // RESUME DETECTION. The old `overruns_` test misses a turn that ran out of fuel and was resumed next round, because the resumed part starts again
    // from a full 100,000. A reading of the fuel counter that is HIGHER than an earlier reading in the same turn proves the budget was refilled, i.e.
    // the turn was suspended. note_fuel() takes such readings at a few points of a turn.
    std::uint64_t fuel_floor_ = 0;       // the lowest fuel reading so far this turn
    int resumes_ = 0;                    // turns found suspended and resumed
    int last_resume_round_ = 0;          // the round of the last one
    int last_fuel_log_ = 0;              // the round the last FUEL line was queued
    int fuel_log_pending_ = 0;           // FUEL log lines still to write (one per turn, when enough fuel is left)
    std::uint64_t fuel_probe_ = 0;       // (profile builds) fuel cost of one fuel_remaining() call
    std::uint64_t turn_start_totals_[64] = {};  // (profile builds) piece totals at the start of this turn, to name what made a heavy turn heavy
    // Max fuel one call of: [0] state 1, [1] state 2, [2] state 3, [3] state 5, [5] the steps before the states, [6] update_enemies, [7] update_camps,
    // [8] update_engagement, [9] the simulation, [10] everything before the simulation in a turn, [11] (unused since the group-state cache lost its timer).
    std::uint64_t state_fuel_max_[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
};
