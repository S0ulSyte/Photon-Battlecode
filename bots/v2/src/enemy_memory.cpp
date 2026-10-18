// enemy_memory.cpp - What the bot knows about champions in sight and enemies it remembers: the views, the enemy memory (P0), the logged-engagement bookkeeping, and picking the enemy to fight.
//
// Reads: the engine (champions in sight, their health and position). Writes: enemy_view_, ally_view_, champion_cache_, enemies_, dead_ids_, eng_.
// Edit here to change: how long enemies are remembered (cfg::kMemoryTurns), what is stored per enemy (EnemyMem in bot.h), how the fight target is chosen (nearest_enemy), when a logged fight ends.
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

// What: fill `out` with the champions of one side that are in sight, in the same order as cambc::vision::champions(side) (tile scan order).
// Why: vision::champions() builds a new champion object for each one on every call (~800 fuel each).
// How: REFACTOR (stage 1): the same tile scan, then the handle of the champion on each tile (one cheap engine call, the first half of what
//      champion_at does), and the object comes from champion_cache_ (built the first time that handle is seen); `out` only points at it.
// Inputs: a side (ally or enemy) and the list to fill. Result: `out` points at the cached champion objects in sight, in tile scan order.
// Side effects: may build champion objects into champion_cache_. Fuel: ~600 per call plus ~100 per champion in sight.
void Bot::champions_in_sight(cambc::team side, ChampionView& out) {
    out.items.clear();
    const auto tiles = cambc::vision::champion_tiles(side).positions();
    for (const vec2 tile : tiles) {
        std::uint32_t handle{};
        cambc::detail::check(cambc::detail::sdk_champion_at(tile.x, tile.y, &handle));  // same call and error check as champion_at()
        auto it = champion_cache_.find(handle);
        if (it == champion_cache_.end())
            it = champion_cache_.emplace(handle, cambc::detail::handle_access::make_champion(static_cast<cambc::handle_champion_t>(handle))).first;
        out.items.push_back(&it->second);
    }
}

// What: the enemy champions in sight this round (read from the engine at most once per round). Result: a ChampionView. Fuel: ~700 the first call per round.
const ChampionView& Bot::enemy_view() {
    if (enemy_view_round_ != round_) {
        FUEL_SCOPE(FP_ENEMY_VIEW);
        champions_in_sight(team::enemy, enemy_view_);  // (refactor: was enemy_view_ = cambc::vision::champions(team::enemy))
        enemy_view_round_ = round_;
    }
    return enemy_view_;
}

// What: the allied champions in sight this round, me included (at most one engine read per round). Result: a ChampionView. Fuel: ~1,000 the first call per round.
const ChampionView& Bot::ally_view() {
    if (ally_view_round_ != round_) {
        FUEL_SCOPE(FP_ALLY_VIEW);
        champions_in_sight(team::ally, ally_view_);  // (refactor: was ally_view_ = cambc::vision::champions(team::ally))
        ally_view_round_ = round_;
    }
    return ally_view_;
}

// What: refresh the enemy memory (enemies_) from what is in sight now; forget the ones unseen for kMemoryTurns or known dead. Step P0 of transition_table.md.
// Inputs: none. Result: none. Side effects: writes enemies_ and dead_ids_. Fuel: ~900 per turn (~700 of it is enemy_view).
void Bot::update_enemies() {
    FUEL_SCOPE(FP_UPD_ENEMIES);
    for (auto& e : enemies_)
        e.visible = false;
    for (const auto& c : enemy_view()) {
        const auto record = snapshot_of(c);  // (refactor: one record for health, position, strength and focus)
        const int hp = static_cast<int>(record.health);
        if (hp <= 0)
            continue;
        const std::uint32_t id = id_of(c);
        auto it = std::find_if(enemies_.begin(), enemies_.end(), [&](const EnemyMem& e) { return e.id == id; });
        if (it == enemies_.end()) {
            enemies_.push_back({});
            it = enemies_.end() - 1;
            it->id = id;
            // The champion type and its attack range never change: read them once. (Reading them costs a lot of fuel.)
            const ChampionConst& k = const_of(c);  // (refactor: type and range from the shared cache; same values)
            it->type = k.type;
            it->range = k.range;
            it->handle = c;
        }
        it->pos = vec2{record.x, record.y};
        it->health = hp;
        // Strength and focus only change when the enemy buys items, so refresh them every 10 rounds.
        if (round_ - it->stats_round >= cfg::kEnemyStatsRefreshRounds) {
            it->strength = static_cast<int>(record.strength);
            it->focus = static_cast<int>(record.focus);
            it->stats_round = round_;
        }
        it->last_seen = round_;
        it->visible = true;
        dead_ids_.erase(std::remove(dead_ids_.begin(), dead_ids_.end(), id), dead_ids_.end());
    }
    // Forget enemies that have been out of sight too long, or that we know are dead.
    for (std::size_t i = 0; i < enemies_.size();) {
        EnemyMem& e = enemies_[i];
        bool drop = false;
        if (!e.visible) {
            if (round_ - e.last_seen > cfg::kMemoryTurns) {
                drop = true;
            } else if (e.handle) {
                try {
                    if (!e.handle->is_alive()) {
                        drop = true;
                        dead_ids_.push_back(e.id);
                    }
                } catch (const std::exception&) {
                }
            }
        }
        if (drop)
            enemies_.erase(enemies_.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }
}

// What: pick the enemy to fight. Result: a pointer into enemies_ (nullptr if none). Which one: the nearest (see the switches V2_CHANGE_TARGET_SCORE and
//  V2_CHANGE_RANGE_EQUAL in config.h). Side effects: none. Fuel: ~230.
const EnemyMem* Bot::nearest_enemy() const {
    FUEL_SCOPE(FP_NEAREST);
    const EnemyMem* best = nullptr;
    const vec2 here = my_pos();
#if V2_CHANGE_TARGET_SCORE
    // CHANGE 7. What: the enemy to fight is the one of lowest score = W_DIST * distance + W_HEALTH * (health / 100). Why: "nearest" walks past
    // a weak enemy to hit a strong one; the score trades distance against how long the target takes to kill. How: scan the remembered enemies
    // (health = last seen); a tie goes to a visible enemy, then to the lower id. (The name nearest_enemy is kept for the callers.)
    double best_score = 0;
    for (const EnemyMem& e : enemies_) {
        const double score = V2_W_TARGET_DIST * dist(here, e.pos) + V2_W_TARGET_HEALTH * (e.health / 100.0);
        const bool better = !best || score < best_score ||
                            (score == best_score && ((e.visible && !best->visible) || (e.visible == best->visible && e.id < best->id)));
        if (better) {
            best = &e;
            best_score = score;
        }
    }
    return best;
#endif
#if V2_CHANGE_RANGE_EQUAL
    // CHANGE 15. What: a ranged champion sees every enemy inside its attack range as equally far. Why: for a ranged unit all of them can be hit
    // without moving, so "nearest" only adds a meaningless preference; the tie rule then picks the one that dies soonest. How: effective
    // distance = max(real distance, attack range) for range > 1 (melee unchanged); ties: visible first, then lower health, then lower id.
    {
        const int reach = std::max(cambc::self().attack_range(), 1);
        auto eff = [&](const EnemyMem& e) { return std::max(dist(here, e.pos), reach > 1 ? reach : 0); };
        for (const EnemyMem& e : enemies_) {
            if (!best) {
                best = &e;
                continue;
            }
            const int d = eff(e), db = eff(*best);
            const bool better = d < db || (d == db && ((e.visible && !best->visible) ||
                                                       (e.visible == best->visible && (e.health < best->health || (e.health == best->health && e.id < best->id)))));
            if (better)
                best = &e;
        }
        return best;
    }
#endif
    for (const EnemyMem& e : enemies_)
        if (!best || dist(here, e.pos) < dist(here, best->pos) || (dist(here, e.pos) == dist(here, best->pos) && e.visible && !best->visible))
            best = &e;
    return best;
}

// What: close the logged fight with its outcome (WON, LOST, DISENGAGED, TIMEOUT) and write the ENG log line. Inputs: the outcome word. Fuel: ~2,500 (log line).
void Bot::end_engagement(const char* outcome) {
    if (!eng_.active)
        return;
    LogLine log;
    log.s("ENG ").s(name_).s(" r").n(eng_.start_round).s("-").n(round_).s(" pred ").s(eng_.predicted_win ? "WIN" : "LOSE").s(" t_kill ")
        .n(eng_.pred_kill >= kFightNever ? -1 : eng_.pred_kill).s(" t_die ").n(eng_.pred_die >= kFightNever ? -1 : eng_.pred_die).s(" outcome ").s(outcome);
    log.flush();
    eng_.active = false;
    ++engagements_logged_;
}

// What: step the logged fight forward one turn and end it when someone died, the enemies went quiet, or it ran too long. Step P0 of transition_table.md.
// Inputs: none. Result: none. Side effects: eng_, dead_ids_, ENG log line. Fuel: ~120.
void Bot::update_engagement() {
    FUEL_SCOPE(FP_UPD_ENG);
    if (!eng_.active)
        return;
    auto& me = cambc::self();
    if (!me.is_alive()) {
        end_engagement("LOST");
        return;
    }
    if (!enemies_.empty())
        eng_.last_enemy_round = round_;
    for (const EnemyMem& e : enemies_)
        if (e.visible && std::find(eng_.enemy_ids.begin(), eng_.enemy_ids.end(), e.id) == eng_.enemy_ids.end())
            eng_.enemy_ids.push_back(e.id);
    bool all_dead = !eng_.enemy_ids.empty();
    for (std::uint32_t id : eng_.enemy_ids)
        if (std::find(dead_ids_.begin(), dead_ids_.end(), id) == dead_ids_.end())
            all_dead = false;
    if (all_dead)
        end_engagement("WON");
    else if (round_ - eng_.last_enemy_round > cfg::kEngagementQuietTurns)
        end_engagement("DISENGAGED");
    else if (round_ - eng_.start_round > cfg::kEngagementMaxTurns)
        end_engagement("TIMEOUT");
}

