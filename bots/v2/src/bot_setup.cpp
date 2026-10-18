// bot_setup.cpp - Bot::init (one-time set-up on the first turn) and the per-champion constants cache.
//
// Reads: draft memory (the plan), the map's camp groups, my own champion object. Writes: plan_, my_targets_, target_groups_, groups_, map_camps_, route caches, my_abilities_, name_, slot_.
// Edit here to change: what is precomputed at the start (routes, groups), how the plan is read, the champion names printed in logs (kSlotNames).
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

namespace {
const char* const kSlotNames[7] = {"issac", "cassi", "vendetta", "ryth", "marcella", "varg", "other"};  // names printed in logs, by draft slot
}  // namespace

// What: one-time set-up on the champion's first turn: who I am, my plan from draft memory, my targets' camp groups and routes.
// Inputs: none (reads the SDK and draft memory). Result: none. Side effects: fills plan_, my_targets_, target_groups_, groups_, map_camps_, route caches,
//  my_abilities_; writes the INIT log line. Fuel: about 90,000 once (first turn only; the first turn has a doubled budget). camp_groups() alone is ~20,000.
void Bot::init() {
    initialised_ = true;
    auto& me = cambc::self();
    slot_ = slot_of(me.champion_name());
    my_abilities_ = me.abilities();  // (refactor) copied once; FIGHT CHECK and the cast helpers used to copy them on every call
    g_my_abilities = &*my_abilities_;
    map_w_ = cambc::map::width();  // (change 13) the map size, for the shared-memory tile index
    map_h_ = cambc::map::height();
    name_ = kSlotNames[slot_ < 0 ? 6 : slot_];
    const auto& mem = cambc::draft_memory;
    plan_ok_ = decode_plan(reinterpret_cast<const std::uint8_t*>(mem.data()), mem.size(), plan_);
    if (plan_ok_)
        for (std::size_t i = 0; i < plan_.targets.size(); ++i)
            if (plan_.targets[i].slot == slot_)
                my_targets_.push_back(i);
    // Find the camp group of each of my targets once (reading all groups costs ~20k fuel, so never every turn).
    if (!my_targets_.empty()) {
        for (const auto& g : cambc::map::camp_groups()) {
#if V2_CHANGE_FREE_SLOT
            CampGroupInfo info{g, {}, 1};  // change 9: remember the group's tiles and capacity while we are looping over it anyway
            int capacity = 0;  // largest capacity over the members: elk 2, monolith 0, every other camp 1
#endif
            for (const auto& c : g.camps()) {
                // REFACTOR: remember every camp object for update_camps (no extra SDK calls: g.camps() was being walked anyway).
                map_camp_by_spawn_[c.spawn_position().y * cfg::kTileKeyStride + c.spawn_position().x] = map_camps_.size();
                map_camps_.push_back(c);
#if V2_CHANGE_FREE_SLOT
                info.tiles.push_back(c.spawn_position());
                const int cap = c.kind() == cambc::camp_type::monolith ? 0 : c.kind() == cambc::camp_type::elk ? 2 : 1;
                capacity = std::max(capacity, cap);
#endif
                for (std::size_t k = 0; k < my_targets_.size() && k < 2; ++k) {
                    const PlanTarget& t = plan_.targets[my_targets_[k]];
                    if (c.spawn_position().x == t.x && c.spawn_position().y == t.y)
                        target_groups_[k] = g;
                }
            }
#if V2_CHANGE_FREE_SLOT
            info.capacity = capacity;
            groups_.push_back(std::move(info));
            detour_ban_until_.push_back(0);
#endif
        }
    }
    // REFACTOR: the camp group of each of my targets (free_slot_detour needed it on every call).
#if V2_CHANGE_FREE_SLOT
    for (std::size_t i : my_targets_)
        target_planned_group_.push_back(group_of(vec2{plan_.targets[i].x, plan_.targets[i].y}));
#endif
    // REFACTOR: build every route's tile list once (target_tiles() and state_bank used to rebuild them on every call).
    for (std::size_t i : my_targets_)
        my_target_tiles_.push_back(path_tiles(plan_.targets[i].path));
    if (!plan_.altars.empty())
        altar_tiles_ = path_tiles(plan_.altars[0].path);
    LogLine log;
    log.s("INIT ").s(name_).s(" plan_ok ").n(plan_ok_).s(" altars ").n(static_cast<long long>(plan_.altars.size())).s(" targets");
    for (std::size_t i : my_targets_) {
        const PlanTarget& t = plan_.targets[i];
        log.s(" ").s(t.attack ? "attack:" : "farm:").n(t.kind).xy(t.x, t.y).s("/").n(static_cast<long long>(t.path.steps.size()));
    }
    log.flush();
}

// What: the unchanging facts about a champion (type, attack range, draft slot, stun immunity), read from the engine once per champion handle.
// Why: FIGHT CHECK asked for them again for every unit on every call (~100 fuel per engine call; they never change).
// Inputs: any champion object. Result: its type, attack range, draft slot and stun immunity (constants, read from the engine once per champion).
// Side effects: fills champion_const_. Fuel: ~200 the first time per champion, ~40 after.
const Bot::ChampionConst& Bot::const_of(const cambc::champion& c) {
    const std::uint32_t id = id_of(c);
    auto it = champion_const_.find(id);
    if (it == champion_const_.end()) {
        ChampionConst k;
        k.type = c.champion_name();
        k.range = c.attack_range();
        k.slot = slot_of(k.type);
        k.marcella = k.type == cambc::champion_type::marcella;
        it = champion_const_.emplace(id, k).first;
    }
    return it->second;
}

