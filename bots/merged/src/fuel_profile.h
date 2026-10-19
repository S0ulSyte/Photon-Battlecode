// fuel_profile.h - a tiny profiler for the per-turn fuel budget (100,000 fuel per turn; see REPORT section 2).
//
// What: FUEL_SCOPE(FP_x) at the top of a function (or block) measures the fuel that scope used, with cambc::fuel_remaining() before and after,
//       and adds it to that piece's peak (largest single call), total and call count. Scopes nest, so a piece's number INCLUDES its children.
// Why:  the bot peaked near 97,000 fuel in one turn; to cut it you must know which pieces cost what. The numbers go out in the `FUEL pieces` log
//       lines (Bot::log_fuel in finish_turn) every 50 rounds.
// How:  RAII object; the destructor reads the fuel again. Each scope itself costs about two fuel_remaining() calls (see the `probe` line of the
//       log), so the profile slightly overstates small pieces. With V2_FUEL_PROFILE = 0 the macro is empty and nothing is measured.
// Reads/writes: g_fuel_stats[] (one entry per piece). Edit here to: add a piece (new enum value + a name in kFuelPieceNames).
#pragma once
#include <cambc.h>
#include <cstdint>
#include "config.h"

// The pieces that can be measured. Keep kFuelPieceNames (below) in the same order.
enum FuelPiece {
    FP_ENEMY_VIEW,      // vision::champions(enemy) (once per round)
    FP_ALLY_VIEW,       // vision::champions(ally) (once per round)
    FP_UPD_ENEMIES,     // Bot::update_enemies
    FP_UPD_CAMPS,       // Bot::update_camps
    FP_UPD_ENG,         // Bot::update_engagement
    FP_SHOP,            // Bot::shop
    FP_LOW_HP,          // Bot::low_hp_recall
    FP_WRITE_SHARED,    // Bot::write_shared
    FP_STATE1,          // state 1 standard combat (inclusive)
    FP_STATE2,          // state 2 bank
    FP_STATE3,          // state 3 camp attack
    FP_STATE5,          // state 5 default
    FP_FIGHT_CHECK,     // Bot::fight_check (build + simulate + log)
    FP_FC_ME,           //   building my own unit
    FP_FC_ALLIES,       //   building the visible allies
    FP_FC_SHARED,       //   building the shared-memory allies
    FP_FC_ENEMIES,      //   building the enemies
    FP_SIM,             //   simulate_fight itself
    FP_FIGHT_ENEMY,     // Bot::fight_enemy
    FP_FLEE,            // Bot::flee
    FP_MOVE,            // move_towards_closest
    FP_FOLLOW,          // follow_forward / follow_backward
    FP_OPPORTUNISTIC,   // Bot::opportunistic_attacks
    FP_ATTACK_CAMP,     // Bot::attack_camp
    FP_CAST,            // cast_skill_at
    FP_BASIC_ATTACK,    // basic_attack_with_reset
    FP_DASH,            // dash_next_to
    FP_GROUP_STATE,     // Bot::group_state (cache check + the real engine call)
    FP_TARGET_GROUP,    // Bot::target_group
    FP_DETOUR,          // Bot::free_slot_detour
    FP_ACTIVE_CAMPS,    // Bot::active_camps
    FP_AT_SPAWN,        // Bot::at_spawn
    FP_NEAREST,         // Bot::nearest_enemy
    FP_TARGET_TILES,    // Bot::target_tiles (path_tiles)
    FP_BLOCKED,         // blocked_tiles() (walls, holes, altars, spawn, units, camps, teleports)
    FP_MOVE_FIELD,      //   the distance field part of move_towards_closest
    FP_MOVE_REACH,      //   reachable_tiles + choosing the step
    FP_FOLLOW_FIND,     //   follow_forward: finding my index on the path
    FP_FINISH_LOG,      // writing the FUEL log lines (happens AFTER the turn's fuel was measured)
    FP_COUNT
};

// Names printed in the log, same order as the enum.
inline constexpr const char* kFuelPieceNames[FP_COUNT] = {
    "enemy_view", "ally_view", "upd_enemies", "upd_camps", "upd_eng", "shop", "low_hp", "write_shared", "state1", "state2", "state3",
    "state5", "fight_check", "fc_me", "fc_allies", "fc_shared", "fc_enemies", "sim", "fight_enemy", "flee", "move", "follow", "opportunistic",
    "attack_camp", "cast", "basic_attack", "dash", "group_state", "target_group", "detour", "active_camps", "at_spawn", "nearest",
    "target_tiles", "blocked", "move_field", "move_reach", "follow_find", "finish_log"};

// What one piece has used so far.
struct FuelStat {
    std::uint64_t peak = 0;   // the most one call used
    std::uint64_t total = 0;  // all calls together
    std::uint32_t calls = 0;  // how many calls
};

// One global table (every champion is its own program instance, so "global" means "per champion").
inline FuelStat g_fuel_stats[FP_COUNT];

#if V2_FUEL_PROFILE
// Measures the scope it lives in. Constructed at the top of the scope, adds to the piece when the scope ends (also on an exception).
class FuelScope {
public:
    explicit FuelScope(FuelPiece piece) : piece_(piece), start_(cambc::fuel_remaining()) {}
    ~FuelScope() {
        const std::uint64_t end = cambc::fuel_remaining();
        if (start_ >= end) {  // (the fuel goes UP only when a turn was suspended and resumed; ignore that sample)
            FuelStat& s = g_fuel_stats[piece_];
            const std::uint64_t used = start_ - end;
            s.peak = used > s.peak ? used : s.peak;
            s.total += used;
            ++s.calls;
        }
    }
    FuelScope(const FuelScope&) = delete;
    FuelScope& operator=(const FuelScope&) = delete;

private:
    FuelPiece piece_;
    std::uint64_t start_;
};
// Usage: FUEL_SCOPE(FP_SHOP);  (two helper macros so __LINE__ makes the variable name unique)
#define V2_FUEL_CAT2(a, b) a##b
#define V2_FUEL_CAT(a, b) V2_FUEL_CAT2(a, b)
#define FUEL_SCOPE(piece) FuelScope V2_FUEL_CAT(fuel_scope_, __LINE__)(piece)
#else
#define FUEL_SCOPE(piece) ((void)0)
#endif
