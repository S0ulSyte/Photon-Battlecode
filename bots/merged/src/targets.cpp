// targets.cpp - My two camp targets: which is current, its route and camp group, and switching between them.
//
// Reads: the plan (my_targets_), cached routes and groups. Writes: target_idx_ and the per-camp counters on a switch.
// Edit here to change: how a target switch resets state (switch_target). (Which camps are targeted is decided by the draft: plan.cpp, target_select.cpp.)
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

// What: my current target camp (one of my two from the plan, chosen by target_idx_).
// Result: my current target from the plan (nullptr if I have none). No engine call.
const PlanTarget* Bot::target() const {
    if (my_targets_.empty())
        return nullptr;
    return &plan_.targets[my_targets_[std::min(target_idx_, my_targets_.size() - 1)]];
}

// REFACTOR (stage 1): returns the route cached in init() instead of rebuilding the vector (path_tiles) on every call. Same tiles, same order.
// Result: the route to the current target, start to camp tile (cached in init). Fuel: ~40.
const std::vector<std::pair<int, int>>& Bot::target_tiles() const {
    FUEL_SCOPE(FP_TARGET_TILES);
    static const std::vector<std::pair<int, int>> kNone;  // returned when there is no target
    if (my_targets_.empty())
        return kNone;
    return my_target_tiles_[std::min(target_idx_, my_targets_.size() - 1)];
}

// What: the camp group of my current target, but only while I can see the target camp's own tile (a group's state cannot be read out of sight).
// Result: the camp group of the current target if the target camp's own tile is in sight, else nullptr. Fuel: ~450.
const cambc::map::camp_group* Bot::target_group() const {
    FUEL_SCOPE(FP_TARGET_GROUP);
    const PlanTarget* t = target();
    if (!t)
        return nullptr;
    const std::size_t idx = std::min<std::size_t>(target_idx_, 1);
    // "In vision" = the target camp's own tile is in sight.
    if (!target_groups_[idx] || !cambc::vision::visible_tiles().is_set(vec2{t->x, t->y}))
        return nullptr;
    return &*target_groups_[idx];
}

// What: go for my other target camp, and forget what I knew about the old one (engaged turns, prediction, capturing).
// Inputs: the reason (goes into the SWITCH log line). Side effects: flips target_idx_ between my two targets and clears the per-camp counters. Fuel: ~2,500 (log).
void Bot::switch_target(const char* why) {
    if (my_targets_.size() < 2)
        return;
    target_idx_ = 1 - std::min<std::size_t>(target_idx_, 1);
    camp_engaged_turns_ = 0;
    camp_pred_made_ = -1;
    camp_pred_death_ = -1;
    capturing_ = false;
    LogLine log;
    log.s("SWITCH ").s(name_).s(" r").n(round_).s(" -> target ").n(static_cast<long long>(target_idx_)).s(" because ").s(why);
    log.flush();
}

