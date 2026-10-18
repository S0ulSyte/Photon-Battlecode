// fight.cpp - FIGHT CHECK input building and the fighting actions: fight_check, fight_enemy, flee, retreat_from_camps, opportunistic_attacks, attack_camp.
//
// Reads: me, allies in sight, shared memory, the enemy memory, the engine. Writes: eng_ (logged fights), actions (moves, casts, attacks).
// Edit here to change: what FIGHT CHECK sees (which units go into the simulation, with which stats), what "fight" does (dash, cast, move, attack order), where to flee.
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

// What: my own unit for FIGHT CHECK: my real tile, health, strength, focus and my real cooldowns (the others' are assumed ready).
// Inputs: none. Result: the FightUnit with id 0. Side effects: none. Fuel: ~800.
FightUnit Bot::fc_unit_me() {
    FUEL_SCOPE(FP_FC_ME);
    auto& me = cambc::self();
    FightUnit mine;
    const vec2 my_tile = my_pos();  // (refactor: one cached read; the old code asked the engine for the position twice)
    mine.id = 0;
    mine.mine = true;
    mine.x = my_tile.x;
    mine.y = my_tile.y;
    const auto my_snap = snapshot_of(me);  // (refactor: one record for health, strength and focus)
    mine.health = static_cast<int>(my_snap.health);
    mine.strength = static_cast<int>(my_snap.strength);
    const ChampionConst& my_const = const_of(me);  // (refactor: range and type read once)
    mine.range = my_const.range;
    mine.stun_immune = my_const.marcella;
    const float focus = my_snap.focus;  // (refactor: from the same record; the old code asked for it for each of the two abilities)
    const auto& my_ab = *my_abilities_;  // (set in init)
    for (std::size_t k = 0; k < 2; ++k) {
        int damage = -1;
        try {
            if (my_ab[k].is_damage() || my_ab[k].is_aoe_damage() || my_ab[k].is_cc())
                damage = static_cast<int>(my_ab[k].damage_amount());
        } catch (const std::exception&) {
        }
        mine.skills[k] = to_fight_skill(skill_info(my_ab[k], focus, damage), my_ab[k].cooldown());
    }
    return mine;
}

// What: add the allies I can see to the fight (their cooldowns are unknown: assumed ready). Inputs: the unit list and the next free unit id.
// Result: none. Side effects: appends units, advances next_id. Fuel: ~500 per ally.
void Bot::fc_add_allies(std::vector<FightUnit>& units, int& next_id) {
    FUEL_SCOPE(FP_FC_ALLIES);
    auto& me = cambc::self();
    for (const auto& a : ally_view()) {
        if (a == me || !a.is_alive())
            continue;
        FightUnit u;
        const ChampionConst& ally_const = const_of(a);  // (refactor: type and range read once per champion, not every call)
        const auto ally_snap = snapshot_of(a);          // (refactor: one record for position, health, strength and focus; was five engine calls)
        u.id = next_id++;
        u.mine = true;
        u.x = ally_snap.x;
        u.y = ally_snap.y;
        u.health = static_cast<int>(ally_snap.health);
        u.strength = static_cast<int>(ally_snap.strength);
        u.range = ally_const.range;
        u.stun_immune = ally_const.marcella;
        fill_design_skills(u, ally_const.type, ally_snap.focus);
        units.push_back(u);
    }
}

#if V2_CHANGE_SHARED_ALLIES
// CHANGE 13. What: add the allies OUT of my vision to the fight, from the team's shared memory (their last written tile and health level; design
// stats, D25; layout in config.h / shared_memory.cpp). Inputs: the unit list and the next free unit id. Result: none.
// Side effects: appends units, advances next_id. Fuel: ~1,000.
void Bot::fc_add_shared_allies(std::vector<FightUnit>& units, int& next_id) {
    if (map_w_ * map_h_ > cfg::kSharedMaxTiles)
        return;
    FUEL_SCOPE(FP_FC_SHARED);
    bool seen_slot[6] = {false, false, false, false, false, false};
    seen_slot[slot_ >= 0 ? slot_ : 0] = slot_ >= 0;
    for (const auto& a : ally_view())
        if (const int s = const_of(a).slot; s >= 0)  // (refactor: was slot_of(a.champion_name()), an engine call per ally)
            seen_slot[s] = true;
    std::array<std::byte, 8> raw{};
    try {
        cambc::shared_memory.read(0, std::span<std::byte>(raw));
    } catch (const std::exception&) {
        raw.fill(std::byte{0});
    }
    for (int s = 0; s < 4; ++s) {
        if (seen_slot[s])
            continue;  // already in the list with live numbers
        const unsigned value = static_cast<unsigned>(raw[static_cast<std::size_t>(2 * s)]) | (static_cast<unsigned>(raw[static_cast<std::size_t>(2 * s + 1)]) << 8);
        const int level = static_cast<int>(value >> cfg::kSharedTileBits), tile = static_cast<int>(value & cfg::kSharedTileMask);
        if (level == 0 || tile >= map_w_ * map_h_)
            continue;  // dead, or never written
        const DesignStats& d = design_stats_of_slot(s);
        FightUnit u;
        u.id = next_id++;
        u.mine = true;
        u.x = tile % map_w_;
        u.y = tile / map_w_;
        u.health = std::max(1, static_cast<int>(d.health * static_cast<float>(level) / static_cast<float>(cfg::kSharedHealthLevels)));
        u.strength = static_cast<int>(d.strength);
        u.range = d.range;
        u.stun_immune = type_of_slot(s) == cambc::champion_type::marcella;
        fill_design_skills(u, type_of_slot(s), d.focus);
        units.push_back(u);
    }
}
#endif

// What: add the enemies I can see or remember (last known tile and health) to the fight. Inputs: the unit list. Result: none.
// Side effects: appends units with ids 100 + index in enemies_. Fuel: ~200 per enemy.
void Bot::fc_add_enemies(std::vector<FightUnit>& units) {
    FUEL_SCOPE(FP_FC_ENEMIES);
    for (std::size_t i = 0; i < enemies_.size(); ++i) {
        const EnemyMem& e = enemies_[i];
        FightUnit u;
        u.id = 100 + static_cast<int>(i);
        u.mine = false;
        u.x = e.pos.x;
        u.y = e.pos.y;
        u.health = e.health;
        u.strength = e.strength;
        u.range = e.range;
        u.stun_immune = e.type == cambc::champion_type::marcella;
        fill_design_skills(u, e.type, static_cast<float>(e.focus));
        units.push_back(u);
    }
}

// What: open the logged engagement for the first FIGHT CHECK of a fight (the prediction, who is there) and write the ENGSTART line. update_engagement
// closes it later with the outcome. Inputs: the prediction and the unit list that produced it (units[0] is me). Result: none.
// Side effects: eng_, dead_ids_, ENGSTART log line. Fuel: ~3,000.
void Bot::fc_open_engagement(const FightResult& r, const std::vector<FightUnit>& units) {
    const FightUnit& mine = units[0];
    eng_ = Engagement{};
    dead_ids_.clear();
    eng_.active = true;
    eng_.start_round = round_;
    eng_.predicted_win = r.win;
    eng_.pred_kill = r.turns_to_kill_enemies;
    eng_.pred_die = r.turns_until_me_dead;
    eng_.last_enemy_round = round_;
    for (const EnemyMem& e : enemies_)
        eng_.enemy_ids.push_back(e.id);
    LogLine log;
    log.s("ENGSTART ").s(name_).s(" r").n(round_).s(" hp ").n(mine.health).s(" at ").xy(mine.x, mine.y).s(" allies ")
        .n(static_cast<long long>(units.size() - 1 - enemies_.size())).s(" enemies");
    for (const EnemyMem& e : enemies_)
        log.s(" ").s(cambc::to_string(e.type).c_str()).s(":").n(e.health).xy(e.pos.x, e.pos.y).s(e.visible ? "v" : "m");
    log.s(" pred ").s(r.win ? "WIN" : "LOSE");
    log.flush();
}

// What: FIGHT CHECK (step used by states 1 and 3). Build the fight (me, allies in sight, allies from shared memory, enemies in sight or remembered),
// simulate it, and open the logged engagement on the first call of a fight.
// Inputs: whether this call may open a logged engagement. Result: the simulation result (win / turns to kill / turns until I die).
// Side effects: eng_, dead_ids_, ENGSTART log line (first call of a fight only). Fuel: ~19,000 on average (the simulation is ~14,000 of it; up to ~60,000).
FightResult Bot::fight_check(bool log_engagement) {
    FUEL_SCOPE(FP_FIGHT_CHECK);
    where_ = "fight_check";
    std::vector<FightUnit> units;
    units.reserve(cfg::kFightMaxUnits);  // (refactor) the most the simulation uses; avoids growing (and copying) the list as units are added
    units.push_back(fc_unit_me());
    int next_id = 1;
    fc_add_allies(units, next_id);
#if V2_CHANGE_SHARED_ALLIES
    fc_add_shared_allies(units, next_id);
#endif
    fc_add_enemies(units);

    const std::uint64_t sim_a = cambc::fuel_remaining();
    FightResult r;
    {
        FUEL_SCOPE(FP_SIM);
        r = simulate_fight(units, 0);
    }
    state_fuel_max_[9] = std::max(state_fuel_max_[9], sim_a - cambc::fuel_remaining());
    state_fuel_max_[10] = std::max(state_fuel_max_[10], fuel_start_ - sim_a);
    if (log_engagement && !eng_.active && !enemies_.empty())
        fc_open_engagement(r, units);
    return r;
}

// What: fight one enemy: (in a won fight) dash next to it, cast the first ready skill, close the distance, basic-attack with the reset. The spec: "Move toward it, attack, and use every skill off cooldown."
// "Move toward it, attack, and use every skill off cooldown."
// Inputs: the enemy to fight; `win` = FIGHT CHECK said WIN (only then may a dash be used). Result: none.
// Side effects: may dash, cast, move and attack (this is the turn's action). Fuel: ~8,600 on average.
void Bot::fight_enemy(const EnemyMem& e, bool win) {
    FUEL_SCOPE(FP_FIGHT_ENEMY);
    where_ = "fight_enemy";
    auto& me = cambc::self();
    const cambc::champion* handle = nullptr;  // (refactor: a pointer into the cache; was an optional<champion> copy)
    if (e.visible)
        for (const auto& c : enemy_view())
            if (id_of(c) == e.id) {
                handle = &c;
                break;
            }
#if V2_CHANGE_DASH
    // CHANGE 8: in a won fight, with the enemy in sight and more than 1 tile away, dash next to it first. A dash is compatible with a move and
    // a basic attack in the same turn, so the rest of this function runs as usual (and finds the enemy already adjacent).
    if (win && handle && dist(my_pos(), e.pos) > 1 && me.can_cast())
        dash_next_to(e.pos, name_);  // (name_ only goes into the DASH log line)
#endif
    bool exclusive = false, casted = false;
    if (handle && dist(my_pos(), e.pos) <= cfg::kSkillReach)
        casted = cast_skill_at(*handle, e.pos, true, &exclusive);
    // Close the distance (a skill that excludes movement uses up the turn's move).
    if (!(casted && exclusive) && dist(my_pos(), e.pos) > me.attack_range() && me.can_move())
        move_towards_closest(tiles_around(e.pos, me.attack_range()));
    if (handle) {
        if (!casted && dist(my_pos(), e.pos) <= cfg::kSkillReach)
            casted = cast_skill_at(*handle, e.pos, false);
        basic_attack_with_reset(*handle);
    }
}

// What: run away: head for the nearest tile outside every known enemy's vision (radius cfg::kEnemyVision, D12), or next to the nearest ally, whichever is closer. DEFAULT D10: if there is nowhere to run, fight instead of standing still.
// Run away: head for the nearest tile outside every known enemy's vision, or next to the nearest ally, whichever is closer.
// Inputs: none. Result: none. Side effects: moves (or, if there is nowhere to run, fights the nearest enemy). Fuel: ~9,000.
void Bot::flee() {
    FUEL_SCOPE(FP_FLEE);
    where_ = "flee";
    auto& me = cambc::self();
    cambc::bitboard seen;
    seen.clear();
    for (const EnemyMem& e : enemies_)
        seen |= tiles_around(e.pos, cfg::kEnemyVision);
    cambc::bitboard goals = ~seen;
    for (const auto& a : ally_view())
        if (!(a == me))
            goals |= tiles_around(a.position(), 1);
    const bool moved = move_towards_closest(goals);
    if (!moved) {
        // DEFAULT: if there is nowhere to run to, fight rather than stand still and be hit.
        if (const EnemyMem* e = nearest_enemy())
            fight_enemy(*e);
    }
}

// What: walk to the nearest tile outside range cfg::kCampRange of every awake camp (camps hit anything within range 3).
// "If within range 3 of an active camp: BFS to the nearest tile outside range 3 of it and follow the path."
// Inputs: the awake camps. Result: none. Side effects: a step away from them. Fuel: ~6,500.
void Bot::retreat_from_camps(const std::vector<vec2>& active) {
    where_ = "retreat";
    cambc::bitboard near_camps;
    near_camps.clear();
    for (vec2 c : active)
        near_camps |= tiles_around(c, cfg::kCampRange);
    move_towards_closest(~near_camps);
}

// What: on the way somewhere, use a skill that does not stop movement and a basic attack on the nearest enemy within 4 tiles.
// "On the way, use any attack or skill on enemies in range that doesn't stop movement."
// Inputs: none. Result: none. Side effects: casts a non-movement-blocking skill and attacks the nearest enemy within 4, if any. Fuel: ~60 when none is near.
void Bot::opportunistic_attacks() {
    FUEL_SCOPE(FP_OPPORTUNISTIC);
    where_ = "opportunistic";
    auto& me = cambc::self();
    const cambc::champion* nearest = nullptr;
    const auto& enemies = enemy_view();
    for (const auto& c : enemies)
        if (c.is_alive() && dist(my_pos(), c.position()) <= cfg::kSkillReach && (!nearest || dist(my_pos(), c.position()) < dist(my_pos(), nearest->position())))
            nearest = &c;
    if (!nearest)
        return;
    cast_skill_at(*nearest, nearest->position(), false);
    basic_attack_with_reset(*nearest);
}

// What: hit a camp: cast a skill on it if within 4 tiles, then a basic attack (with the attack reset).
// Inputs: a camp. Result: true if we cast or attacked. Side effects: casts / attacks. Fuel: ~1,600.
bool Bot::attack_camp(const cambc::camp& camp) {
    FUEL_SCOPE(FP_ATTACK_CAMP);
    auto& me = cambc::self();
    // After a step the camp may have dropped out of sight; asking an unseen camp for its position throws.
    if (!camp.is_visible())
        return false;
    bool did = false;
    if (dist(my_pos(), camp.position()) <= cfg::kSkillReach)
        did = cast_skill_at(camp, camp.position(), true);
    did = basic_attack_with_reset(camp) || did;
    return did;
}

