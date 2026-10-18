// state_combat.cpp - STATE 1, STANDARD COMBAT (transition_table.md): an enemy is known and standardCombatAllowed.
//
// Reads: enemies_, awake camps, FIGHT CHECK. Writes: acts_ counters; actions through fight_enemy / flee / retreat_from_camps.
// Edit here to change: what happens when an enemy is known (retreat from camps first, then fight if FIGHT CHECK says WIN, else flee).
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

// STATE 1 (transition_table.md): STANDARD COMBAT.
// Entry condition (checked by play_turn): an enemy is in vision or memory and standard_combat_allowed_.
// What: if I am within kCampRange of an awake camp, walk out of its range; otherwise FIGHT CHECK: win -> fight_enemy (dash, cast, attack), lose -> flee.
// STATE 1: an enemy is in vision or memory and standardCombatAllowed.
// Result: true (always handles the turn). Side effects: retreats from awake camps, or runs FIGHT CHECK and then fights or flees.
// Fuel: ~27,000 on average per call (FIGHT CHECK dominates).
bool Bot::state_standard_combat() {
    auto& me = cambc::self();
    const auto active = active_camps();
    for (vec2 c : active)
        if (dist(my_pos(), c) <= cfg::kCampRange) {
            retreat_from_camps(active);
            ++acts_[2];
            return true;
        }
    const FightResult r = fight_check(true);
    const EnemyMem* e = nearest_enemy();
    if (r.win && e) {
        ++acts_[0];
        fight_enemy(*e, true);  // (change 8: the second argument says the fight is predicted won)
    } else {
        ++acts_[1];
        flee();
    }
    return true;
}

