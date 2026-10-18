// state_camp_attack.cpp - STATE 3, CAMP ATTACK (transition_table.md): fight an attack-target camp, wait for its respawn, capture it, and walk home
// after a capture.
//
// Reads: the target camp group and its state, enemies in sight, FIGHT CHECK, the plan's target.
// Writes: capturing_, returning_, respawn_at_, waiting_, wait_turns_, camp_engaged_turns_, camp_pred_made_, camp_pred_death_, target switches,
//         standard_combat_allowed_, and the actions (attacks, steps, recall).
// Edit here to change: what to do at an attack camp. The pieces, in the order state_camp_attack calls them:
//   camp_wait_for_respawn    waiting beside a respawning camp when the other target is back later (V2_CHANGE_WAIT_STATE, D23)
//   camp_after_capture       the camp is respawning: it was captured (by me: walk home; by others: switch)
//   camp_stand_on_reward     the monsters are dead: stand on the camp tile until the capture completes
//   camp_fight_back          an enemy is in sight: FIGHT CHECK, and fight it if we win (V2_CHANGE_FIGHT_IN_CAMP)
//   camp_hit_group           hit the nearest living monster; the full-health rule (D6)
//   do_return                the walk home after a capture
#include <algorithm>
#include <variant>
#include "ability_util.h"
#include "bot.h"
#include "config.h"
#include "log.h"
#include "moves.h"

using cambc::vec2;

// What: the walk home after a capture (part of state 3). Why: after capturing an attack camp the champion goes home to heal and bank.
// Inputs: none. Result: true while we are still walking home, false once we are at spawn.
// Side effects: recall (when far from spawn and no enemy is near) or one step back along return_tiles_; clears returning_ at spawn. Fuel: ~3,000.
bool Bot::do_return() {
    auto& me = cambc::self();
    if (at_spawn()) {
        returning_ = false;
        capturing_ = false;
        return false;
    }
    bool enemy_near = false;
    for (const EnemyMem& e : enemies_)
        if (dist(my_pos(), e.pos) <= cfg::kReturnEnemyRange)
            enemy_near = true;
    if (steps_from_spawn_on(return_tiles_) > cfg::kReturnNearSpawnSteps && !enemy_near) {
        me.recall();
        return true;
    }
    goals_[3].reset();
    follow_backward(return_tiles_, 3);
    return true;
}

#if V2_CHANGE_WAIT_STATE
// CHANGE 11. What: remember when this target finishes respawning, and wait here if the other target is still respawning and finishes later.
// Why: with both targets respawning the old rule switched every time we arrived (hole 5). How: respawn_at_[idx] = now + the engine's remaining
// timer (the camp kind's respawn time if the timer is unusable); alive / reward available = 0 = available.
// Inputs: the target, the target group's state. Result: kHandled (we wait here this turn) or kContinue.
// Side effects: respawn_at_, waiting_, wait_turns_, standard_combat_allowed_ (D23: waiting is not camp fighting), a WAIT log line. Fuel: ~50 (2,000 on the log).
CampStep Bot::camp_wait_for_respawn(const PlanTarget& t, const cambc::map::camp_group_state& st) {
    const std::size_t idx = std::min<std::size_t>(target_idx_, 1);
    if (const auto* rs = std::get_if<cambc::map::camp_group_respawning>(&st)) {
        int left = static_cast<int>(rs->timer);
        if (left <= 0)
            left = cfg::respawn_rounds(t.kind);  // wolf 40, gold cluster 50, elk / gem cluster 60 (evidence/engine_facts.txt)
        respawn_at_[idx] = round_ + left;
        const std::size_t other = 1 - idx;
        // Wait only when we are NOT the one who just captured it (that case walks home first), the other target is still respawning, and
        // this one is back first.
        if (!capturing_ && my_targets_.size() >= 2 && respawn_at_[other] > round_ && respawn_at_[other] > respawn_at_[idx]) {
            if (!waiting_) {
                waiting_ = true;
                wait_turns_ = 0;
                LogLine log;
                log.s("WAIT ").s(name_).s(" r").n(round_).s(" at target ").n(static_cast<long long>(idx)).s(" back r").n(respawn_at_[idx]).s(" other back r").n(respawn_at_[other]);
                log.flush();
            }
            ++wait_turns_;
            standard_combat_allowed_ = true;  // DEFAULT D23: waiting is not camp fighting, so enemies are handled by state 1 next turn
            return CampStep::kHandled;        // stay beside the camp this turn
        }
    } else {
        respawn_at_[idx] = 0;  // alive or reward available: available now
    }
    waiting_ = false;
    return CampStep::kContinue;
}
#endif

// What: the target group is respawning, so the camp was captured (by me or by someone else). If I did it, start the walk home; either way switch
// to the other target. Inputs: the target. Result: what state_camp_attack returns (true if we went on walking home, false = let the next states run).
// Side effects: capturing_, returning_, return_tiles_, a CAMP log line, a target switch. Fuel: ~5,000 (two log lines).
bool Bot::camp_after_capture(const PlanTarget& t) {
    if (capturing_) {
        returning_ = true;
        return_tiles_ = target_tiles();
        goals_[3].reset();
    }
    LogLine log;
    log.s("CAMP ").s(name_).s(" r").n(round_).s(" captured ").xy(t.x, t.y).s(capturing_ ? " by me" : " by others");
    log.flush();
    capturing_ = false;
    switch_target("camp already captured");
    return returning_ ? do_return() : false;
}

// What: the monsters are dead (reward available): stand on the camp's tile until the capture completes. Inputs: the target and its tile.
// Result: true (always handles the turn). Side effects: a CAMP "cleared" log line (once), capturing_, a step along the route. Fuel: ~2,500.
bool Bot::camp_stand_on_reward(const PlanTarget& t, vec2 camp_tile) {
    if (camp_pred_made_ >= 0) {
        LogLine log;
        log.s("CAMP ").s(name_).s(" r").n(round_).s(" cleared ").xy(t.x, t.y).s(" predicted ").n(camp_pred_death_).s(" actual ").n(round_);
        log.flush();
        camp_pred_made_ = -1;
    }
    if (my_pos() == camp_tile)
        capturing_ = true;
    else
        follow_forward(target_tiles(), static_cast<int>(target_idx_));
    return true;
}

// What: the monsters are alive and an enemy champion is in sight: run FIGHT CHECK. If we lose, switch target; if we win, fight the enemy instead of
// the camp. Why: state 3 used to keep hitting the camp while being attacked (hole 9). Inputs: none.
// Result: kPassOn (we lose: target switched, let the next states run), kHandled (we fought back), or kContinue (no enemy in sight, or fighting back is
// off: go on and hit the camp). Side effects: FIGHT CHECK's, a target switch, ignored_enemy_turns_, acts_[0]. Fuel: ~20,000 with an enemy, ~10 without.
CampStep Bot::camp_fight_back() {
    where_ = "state3 enemy_check";
    bool enemy_visible = false;
    for (const EnemyMem& e : enemies_)
        enemy_visible = enemy_visible || e.visible;
    if (!enemy_visible)
        return CampStep::kContinue;
    const FightResult r = fight_check(true);
    if (!r.win) {
        switch_target("fight check says lose");
        return CampStep::kPassOn;
    }
#if V2_CHANGE_FIGHT_IN_CAMP
    // CHANGE 12. What: fight the visible enemy we are predicted to beat. Why: see above. How: pick the nearest VISIBLE enemy and run fight_enemy
    // on it (win = true, so a dash may be used); that is this turn's action.
    const EnemyMem* target_enemy = nullptr;
    for (const EnemyMem& e : enemies_)
        if (e.visible && (!target_enemy || dist(my_pos(), e.pos) < dist(my_pos(), target_enemy->pos)))
            target_enemy = &e;
    if (target_enemy) {
        ++acts_[0];  // counted as a fight turn
        fight_enemy(*target_enemy, true);
        return CampStep::kHandled;
    }
#endif
    ++ignored_enemy_turns_;
    return CampStep::kContinue;
}

// What: hit the nearest living monster of the group, or walk toward it. Also predicts the turn it dies, and applies the full-health rule (D6): a
// camp still at full health after kFullHealthGraceTurns turns of attacking is not worth it, so switch.
// Inputs: the target, its tile is not needed, and its group. Result: true (hit or walked), false after a target switch.
// Side effects: camp_engaged_turns_, camp_pred_*, a CAMP "engaging" log line (once per camp), attacks, a step. Fuel: ~4,000.
bool Bot::camp_hit_group(const PlanTarget& t, const cambc::map::camp_group& group) {
    auto& me = cambc::self();
    where_ = "state3 pick_hit";
    // The nearest living monster of the group.
    const cambc::camp* hit = nullptr;
    const auto members = group.camps();
    for (const auto& c : members)
        if (c.is_visible() && c.health() > 0 && (!hit || dist(my_pos(), c.position()) < dist(my_pos(), hit->position())))
            hit = &c;

    const int attack_reach = std::max(me.attack_range(), 1);
    const bool in_range = hit && dist(my_pos(), hit->position()) <= attack_reach;
    if (in_range) {
        ++camp_engaged_turns_;
        if (camp_pred_made_ < 0) {
            // "Predict the turn it dies from the damage it's taking": remaining monster health / our damage per turn.
            float remaining = 0;
            for (const auto& c : members)
                if (c.is_visible() && c.health() > 0)
                    remaining += c.health();
            const float dps = std::max(1.0f, me.strength());
            camp_pred_made_ = round_;
            camp_pred_death_ = round_ + static_cast<int>(remaining / dps) + 1;
            LogLine log;
            log.s("CAMP ").s(name_).s(" r").n(round_).s(" engaging ").xy(t.x, t.y).s(" predicted death r").n(camp_pred_death_);
            log.flush();
        }
    }

    where_ = "state3 full_health";
    // "If it's at full health ... switch target to the other camp."
    bool full = hit && hit->health() >= hit->max_health();
#if V2_LITERAL_FULL_HEALTH
    if (full) {
        switch_target("camp at full health (literal spec)");
        return false;
    }
#else
    if (full && camp_engaged_turns_ >= cfg::kFullHealthGraceTurns) {
        switch_target("camp still at full health after several turns of attacking");
        return false;
    }
#endif

    where_ = "state3 attack";
    // Hold position and hit it; if it is out of reach, walk the path until it is.
    if (hit && in_range) {
        attack_camp(*hit);
    } else {
        follow_forward(target_tiles(), static_cast<int>(target_idx_));
        if (hit)
            attack_camp(*hit);
    }
    return true;
}

// STATE 3 (transition_table.md): the target camp was chosen for attack and is in sight (or we are capturing / walking home from it).
// Entry condition (checked by play_turn): returning_, or (the target is an ATTACK camp and (capturing_ or its group is in sight)).
// Inputs: none. Result: true if it handled the turn, false after a target switch or when there is nothing to do (the next states run).
// Side effects: sets standard_combat_allowed_ false (spec: state 3 turns ignore state 1), plus those of the pieces below. Fuel: ~6,500 on average.
bool Bot::state_camp_attack() {
    ++acts_[4];
    standard_combat_allowed_ = false;
    if (returning_)
        return do_return();
    const PlanTarget* t = target();
    if (!t)
        return false;
    where_ = "state3 target_group";
    const vec2 camp_tile{t->x, t->y};
    const cambc::map::camp_group* group = target_group();
    if (!group) {
        // Out of sight. DEFAULT: we only keep "capturing" while we really stand on the camp's tile; otherwise the flag is stale.
        if (capturing_ && my_pos() == camp_tile)
            return true;
        capturing_ = false;
        return false;
    }
    where_ = "state3 group_state";
    cambc::map::camp_group_state st = cambc::map::camp_group_alive{};
    group_state(*group, st);  // (change 14: cached per round; on failure `st` keeps the default "alive", as the old try/catch did)

#if V2_CHANGE_WAIT_STATE
    if (camp_wait_for_respawn(*t, st) == CampStep::kHandled)
        return true;
#endif
    if (std::holds_alternative<cambc::map::camp_group_respawning>(st))
        return camp_after_capture(*t);
    if (std::holds_alternative<cambc::map::camp_group_reward_available>(st))
        return camp_stand_on_reward(*t, camp_tile);

    // The monsters are alive.
    const CampStep fought = camp_fight_back();
    if (fought == CampStep::kPassOn)
        return false;
    if (fought == CampStep::kHandled)
        return true;
    return camp_hit_group(*t, *group);
}
