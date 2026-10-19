// champion_util.h - small free functions about champions that several files of the bot need.
//
// What it is for: turning SDK objects into plain numbers (draft slot, id, the whole live record of a champion in one engine call), and the design
//                 stats of a draft slot's champion.
// Reads: the SDK (engine calls, noted per function). Writes: nothing (the design-stats cache is the only state).
// Used by: bot_setup.cpp, enemy_memory.cpp, fight.cpp, shared_memory.cpp, turn files.
// Edit here to change: how a champion is identified (slot_of / id_of), which fields the one-call record carries (snapshot_of), or the stand-in
//                      stats used for allies read from shared memory (design_stats_of_slot).
#pragma once
#include <cambc.h>
#include <cstdint>
#include <utility>
#include "config.h"

// Draft slot of a champion type: 0 Issac, 1 Cassi, 2 Vendetta, 3 Ryth, 4 Marcella, 5 Varg; -1 for any other type. No engine call.
int slot_of(cambc::champion_type t);

// The engine handle of a champion as a plain number (its identity for the whole game). No engine call.
std::uint32_t id_of(const cambc::champion& c);

// A route tile as a vec2. No engine call.
inline cambc::vec2 to_vec(const std::pair<int, int>& p) { return cambc::vec2{p.first, p.second}; }

// The whole live record of a champion (tile, health, max health, strength, focus, state flags) from ONE engine call (~100 fuel). Throws cambc::error
// like the SDK getters do (for example when the champion is not in sight).
cambc::detail::champion_info snapshot_of(const cambc::champion& c);

// One flag of the state word of a record taken with snapshot_of (can_move, can_attack, can_cast, recalling, ...). No engine call.
bool has_flag(const cambc::detail::champion_info& record, cambc::champion_state flag);

#if V2_CHANGE_SHARED_ALLIES
// The champion type that sits in draft slot 0..5. No engine call.
cambc::champion_type type_of_slot(int slot);

// Design (base) stats of the champion in a draft slot, read from the engine once and cached. Used for allies known only from shared memory.
struct DesignStats {
    bool known = false;
    float health = 0, strength = 0, focus = 0;
    int range = 1;
};
const DesignStats& design_stats_of_slot(int slot);
#endif
