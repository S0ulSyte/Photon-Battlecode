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

// MERGED. What: add the live camps near me to the fight as enemies that never move (they only hit what is within their range), so a camp is judged
// by the same simulation as an enemy champion. The nearest cfg::kCampCheckMaxCamps within cfg::kCampCheckRange. Inputs: the unit list and the next
// free unit id. Result: none. Side effects: appends units (ids 200 + index). Fuel: ~300 per camp.
void Bot::fc_add_camps(std::vector<FightUnit>& units, int& next_id) {
    std::vector<const cambc::camp*> near;
    const auto& camps = cambc::vision::camps();
    for (const auto& c : camps)
        if (c.health() > 0 && dist(my_pos(), c.position()) <= cfg::kCampCheckRange)
            near.push_back(&c);
    std::sort(near.begin(), near.end(), [&](const cambc::camp* a, const cambc::camp* b) { return dist(my_pos(), a->position()) < dist(my_pos(), b->position()); });
    if (near.size() > static_cast<std::size_t>(cfg::kCampCheckMaxCamps))
        near.resize(static_cast<std::size_t>(cfg::kCampCheckMaxCamps));
    for (const cambc::camp* c : near) {
        FightUnit u;
        u.id = 200 + next_id++;
        u.mine = false;
        const vec2 p = c->position();
        u.x = p.x;
        u.y = p.y;
        u.health = static_cast<int>(c->health());
        u.strength = static_cast<int>(c->strength());
        u.range = std::max(1, static_cast<int>(c->attack_range()));
        u.immobile = true;
        units.push_back(u);
    }
}

// MERGED. What: if I am below cfg::kRetreatMyHpPercent (25%) of my health and my opponent is above cfg::kRetreatOpponentHpPercent (15%) of its, get out of its range.
// The opponent is the nearest of: an enemy champion in sight within cfg::kEnemyVision, a live camp that can hit (strength > 0) within cfg::kCampCheckRange. "Range" is
// the opponent's attack range plus 1 for a champion (it can step and hit), the camp range for a camp. In range of any enemy: take one step to the nearest tile outside the
// range of all of them (if I cannot move, fight on). Already out of range: start a recall (it channels safely there; without it the follow-ally rule or the states
// would walk straight back in). Not on a spawn tile (the spawn hold heals there). Inputs: none. Result: true if this turn was spent on it.
// Side effects: a step or a recall, a RETREAT log line. Fuel: ~150 when I am healthy, ~1,500 otherwise.
bool Bot::retreat_low_hp() {
    auto& me = cambc::self();
    const auto record = snapshot_of(me);
    if (record.health * 100.0f >= record.max_health * cfg::kRetreatMyHpPercent || at_spawn())
        return false;
    const vec2 here = my_pos();
    struct Opp {
        vec2 pos;
        int reach;
        float pct;
        int d;
        bool camp;
    };
    std::vector<Opp> opps;
    for (const EnemyMem& e : enemies_) {
        if (!e.visible || !e.handle)
            continue;
        const int d = dist(here, e.pos);
        if (d > cfg::kEnemyVision)
            continue;
        float pct = 100.0f;
        try {
            pct = 100.0f * e.handle->health() / std::max(e.handle->max_health(), 1.0f);
        } catch (const std::exception&) {
        }
        opps.push_back({e.pos, e.range + 1, pct, d, false});
    }
    for (const auto& c : cambc::vision::camps())
        if (c.health() > 0 && c.strength() > 0 && dist(here, c.position()) <= cfg::kCampCheckRange)
            opps.push_back({c.position(), cfg::kCampRange, 100.0f * c.health() / std::max(c.max_health(), 1.0f), dist(here, c.position()), true});
    const bool retreating = round_ - retreat_last_ <= 3;  // I was already retreating in the last rounds
    if (opps.empty()) {
        // Nobody in sight any more. If I was retreating, I got away: recall now (otherwise the follow-ally rule or the states would walk me back in).
        if (!retreating)
            return false;
        try {
            me.recall();
        } catch (const cambc::error&) {
            return false;
        }
        return true;
    }
    const Opp* nearest = &opps[0];
    for (const Opp& o : opps)
        if (o.d < nearest->d)
            nearest = &o;
    if (nearest->pct <= static_cast<float>(cfg::kRetreatOpponentHpPercent))
        return false;  // nearly dead: finish it
    where_ = "retreat_low_hp";
    if (!retreating)
        retreat_start_ = round_;
    retreat_last_ = round_;
    bool in_range = false, far_enough = true;
    cambc::bitboard near_all, near_far;
    near_all.clear();
    near_far.clear();
    for (const Opp& o : opps) {
        in_range = in_range || o.d <= o.reach;
        far_enough = far_enough && o.d > o.reach + cfg::kRetreatExtraTiles;
        near_all |= tiles_around(o.pos, o.reach);
        near_far |= tiles_around(o.pos, o.reach + cfg::kRetreatExtraTiles);
    }
    const bool out_of_time = round_ - retreat_start_ >= cfg::kRetreatMaxRounds;
    const char* action = far_enough ? "recall (far enough)" : (out_of_time && !in_range ? "recall (retreated long enough)" : "step away");
    LogLine log;
    log.s("RETREAT ").s(name_).s(" r").n(round_).s(" hp ").n(static_cast<long long>(record.health)).s("/").n(static_cast<long long>(record.max_health))
        .s(" opponent ").s(nearest->camp ? "camp " : "champion ").n(static_cast<long long>(nearest->pct)).s("% dist ").n(nearest->d).s(" reach ").n(nearest->reach)
        .s(" -> ").s(action);
    log.flush();
    if (!far_enough && !(out_of_time && !in_range)) {
        const vec2 before = my_pos();
        move_towards_closest(~near_far);  // the nearest tile more than reach + 4 away from every opponent
        if (my_pos() != before)
            return true;
        if (in_range) {  // could not get that far: at least get out of range
            move_towards_closest(~near_all);
            return my_pos() != before;  // could not move at all: let the normal logic fight on
        }
        // out of range but cannot get farther: recall here
    }
    try {
        me.recall();
    } catch (const cambc::error&) {
        return false;
    }
    return true;
}

// MERGED. What: read which passive emblems I own and turn them into the percentages the camp FIGHT CHECK scales hits by: monster emblem = my damage to
// camps 110, jungle emblem = camp damage to me 70, durability emblem = damage to me from anything 90 (an emblem of any stat variant counts; two of one family
// do not stack, as in the rules). Re-read every 10 rounds (a purchase is rare). Inputs: none. Result: none. Side effects: emb_* members. Fuel: ~300 per read.
void Bot::refresh_emblems() {
    if (round_ - emb_round_ < 10)
        return;
    emb_round_ = round_;
    using IT = cambc::item_type;
    try {
        const auto counts = cambc::self().item_count();
        const bool monster = counts[IT::monster_emblem_of_strength] + counts[IT::monster_emblem_of_focus] + counts[IT::monster_emblem_of_health] > 0;
        const bool jungle = counts[IT::jungle_emblem_of_strength] + counts[IT::jungle_emblem_of_focus] + counts[IT::jungle_emblem_of_health] > 0;
        const bool durability = counts[IT::durability_emblem_of_strength] + counts[IT::durability_emblem_of_focus] + counts[IT::durability_emblem_of_health] > 0;
        emb_camp_out_ = monster ? 110 : 100;
        emb_camp_in_ = jungle ? 70 : 100;
        emb_in_ = durability ? 90 : 100;
    } catch (const cambc::error&) {
        // unreadable: keep the last values
    }
}

// MERGED. What: before attacking a camp or walking on past one, run FIGHT CHECK with the nearby live camps as enemies. If it predicts a LOSS, get out
// the way combat does (retreat_from_camps: step out of the camps' range) and then recall to heal. If I am still nearly at full health the camp is
// just too strong for me, so switch target instead of recalling forever. Not while capturing or walking home. Inputs: none. Result: true if this
// turn was spent leaving (the states below must not run); false to carry on. Side effects: a step, a recall, a target switch, a CAMPLOSE log line.
// Fuel: ~20,000 when a live camp is within range, ~100 otherwise.
bool Bot::camp_check() {
    if (capturing_ || returning_)
        return false;
    const vec2 here = my_pos();
    std::vector<vec2> threats;
    bool dangerous = false;  // some camp in range can actually hit (gem and gold clusters have strength 0: nothing to fear, no simulation needed)
    for (const auto& c : cambc::vision::camps())
        if (c.health() > 0 && dist(here, c.position()) <= cfg::kCampCheckRange) {
            threats.push_back(c.position());
            dangerous = dangerous || c.strength() > 0;
        }
    if (threats.empty() || !dangerous || cambc::fuel_remaining() < cfg::kCampCheckMinFuel)
        return false;
    where_ = "camp_check";
    const auto now = snapshot_of(cambc::self());
    const bool reuse = round_ - cc_round_ <= cfg::kCampCheckReuseRounds && cc_threats_ == threats.size() && now.health >= cc_hp_ * 0.9f;
    std::uint64_t sim_cost = 0;
    if (!reuse) {
        const std::uint64_t fuel_a = cambc::fuel_remaining();
        const FightResult r = fight_check(false, true);
        sim_cost = fuel_a - cambc::fuel_remaining();
        cc_round_ = round_;
        cc_win_ = r.win;  // TEAM win: the camps die strictly before my whole side does
        cc_hp_ = now.health;
        cc_threats_ = threats.size();
        cc_kill_ = r.turns_to_kill_enemies;
        cc_die_ = r.turns_until_me_dead;
        cc_team_die_ = r.turns_until_team_dead;
    }
    // About to die: if the simulation has me dead within cfg::kCampAboutToDieTurns turns, I leave (retreat, then recall) REGARDLESS of the team verdict.
    // (The simulation only records my death when I die before the camps are all dead, so cc_die_ is kFightNever in a fight I win outright.)
    // Otherwise: a team win means fight on; a team loss means get out (recall if hurt, switch target if not).
    const bool about_to_die = cc_die_ <= cfg::kCampAboutToDieTurns;
    if (cc_win_ && !about_to_die)
        return false;
    auto& me = cambc::self();
    ++acts_[1];
    const auto record = snapshot_of(me);
    const bool hurt = record.health * 100.0f < record.max_health * cfg::kCampHurtPercent;
    bool inside = false;
    for (vec2 p : threats)
        inside = inside || dist(here, p) <= cfg::kCampRange;
    float c_hp = 0, c_str = 0;
    int c_rng = 0;
    for (const auto& c : cambc::vision::camps())
        if (c.health() > 0 && dist(here, c.position()) <= cfg::kCampCheckRange) {
            c_hp = c.health();
            c_str = c.strength();
            c_rng = static_cast<int>(c.attack_range());
            break;
        }
    const bool heal = about_to_die || hurt;  // recall to heal; otherwise (a team loss at near-full health) the camp is too strong: switch target
    const char* action = inside ? "retreat" : (heal ? (at_spawn() ? "hold" : "recall") : "switch");
    LogLine log;
    log.s(about_to_die ? "CAMPBAIL " : "CAMPLOSE ").s(name_).s(" r").n(round_).s(" hp ").n(static_cast<long long>(record.health)).s("/").n(static_cast<long long>(record.max_health))
        .s(" camps ").n(static_cast<long long>(threats.size())).s(" c0 ").n(static_cast<long long>(c_hp)).s("/").n(static_cast<long long>(c_str)).s("/r").n(c_rng)
        .s(" mystr ").n(static_cast<long long>(record.strength)).s(" kill ").n(cc_kill_).s(" me_die ").n(cc_die_).s(" team_die ").n(cc_team_die_)
        .s(" emb ").n(emb_camp_out_).s("/").n(emb_camp_in_).s("/").n(emb_in_).s(" cost ").n(static_cast<long long>(sim_cost)).s(" -> ").s(action);
    log.flush();
    if (inside) {
        retreat_from_camps(threats);  // the same way out as in combat
        return true;
    }
    if (heal) {
        if (!at_spawn())
            me.recall();
        return true;
    }
    switch_target("camp fight check says the team loses at near-full health");
    return true;
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
FightResult Bot::fight_check(bool log_engagement, bool include_camps) {
    FUEL_SCOPE(FP_FIGHT_CHECK);
    where_ = "fight_check";
    std::vector<FightUnit> units;
    units.reserve(cfg::kFightMaxUnits);  // (refactor) the most the simulation uses; avoids growing (and copying) the list as units are added
    units.push_back(fc_unit_me());
    int next_id = 1;
    if (include_camps)
        fc_add_camps(units, next_id);  // MERGED: the camps come right after me, so the cap below can never cut them
    fc_add_allies(units, next_id);
#if V2_CHANGE_SHARED_ALLIES
    fc_add_shared_allies(units, next_id);
#endif
    fc_add_enemies(units);
    if (include_camps && units.size() > static_cast<std::size_t>(cfg::kCampCheckMaxUnits))
        units.resize(static_cast<std::size_t>(cfg::kCampCheckMaxUnits));  // MERGED: fewer units = a cheaper simulation (allies and enemies past the cap are left out)

    const std::uint64_t sim_a = cambc::fuel_remaining();
    FightResult r;
    {
        FUEL_SCOPE(FP_SIM);
        if (include_camps) {
            // MERGED: emblem passives. Mine are read exactly (item counts are readable for myself only); my allies are assumed to have the durability
            // emblem if I do (every champion shops treasure then durability first), nothing else.
            refresh_emblems();
            units[0].camp_out_pct = emb_camp_out_;
            units[0].camp_in_pct = emb_camp_in_;
            for (std::size_t k = 0; k < units.size(); ++k)
                if (units[k].mine)
                    units[k].in_pct = emb_in_;
            r = simulate_team_fight(units, 0, cfg::kCampFightMaxTurns);
        } else {
            r = simulate_fight(units, 0);
        }
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
    note_target(e.pos);  // MERGED: published to the allies in my shared byte
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
    note_target(camp.position());  // MERGED: published to the allies in my shared byte
    bool did = false;
    if (dist(my_pos(), camp.position()) <= cfg::kSkillReach)
        did = cast_skill_at(camp, camp.position(), true);
    did = basic_attack_with_reset(camp) || did;
    return did;
}

