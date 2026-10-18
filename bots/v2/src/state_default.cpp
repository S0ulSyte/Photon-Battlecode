// state_default.cpp - STATE 5, DEFAULT (transition_table.md): walk the route to the target camp, help allies, take a free camp on the way, capture
// the camp when it is cleared.
//
// Reads: the target camp and its group state, allies in sight, camps seen this round (seen_now_), the camp groups (groups_).
// Writes: capturing_, respawn_at_, the detour_* fields, target switches, and the actions (attacks, steps).
// Edit here to change: what a farming champion does on its way. The pieces, in the order state_default calls them:
//   help_allies_hit_camp   attack a camp an ally is already hitting
//   free_slot_detour       (V2_CHANGE_FREE_SLOT, D22) take a free camp within reach instead of walking past it:
//       detour_continue        keep going with a detour that is already running
//       detour_start           look for a new one and start it
//   farm_group_step        the target camp's group is cleared or was just captured (switch after a farm capture, D7)
#include <algorithm>
#include <variant>
#include "ability_util.h"
#include "bot.h"
#include "config.h"
#include "log.h"
#include "moves.h"

using cambc::team;
using cambc::vec2;

#if V2_CHANGE_FREE_SLOT
// CHANGE 9 (a). What: keep going with a detour that is already running: hit the group while it is alive, stand on the cleared camp's tile while its
// reward is available, drop the detour when the group respawns, is out of sight, has no living member in sight, or has not been scratched.
// Inputs: none (uses detour_group_). Result: true if this turn was spent on the detour; false if there was none or it just ended (then the caller
// looks for a new one in the same turn). Side effects: attacks, a step, a DETOUR "end" log line, detour_ban_until_, capturing_. Fuel: ~3,000.
bool Bot::detour_continue() {
    if (detour_group_ < 0)
        return false;
    const int reach = std::max(cambc::self().attack_range(), 1);
    const CampGroupInfo& gi = groups_[static_cast<std::size_t>(detour_group_)];
    auto end_detour = [&](const char* why, int ban_rounds) {
        LogLine log;
        log.s("DETOUR ").s(name_).s(" r").n(round_).s(" end ").s(why).s(" after ").n(detour_turns_);
        log.flush();
        detour_ban_until_[static_cast<std::size_t>(detour_group_)] = round_ + ban_rounds;
        detour_group_ = -1;
        capturing_ = false;
    };
    cambc::map::camp_group_state st = cambc::map::camp_group_alive{};
    const bool have_state = group_state(gi.group, st);  // false when no member is in vision (change 14: cached per round)
    ++detour_turns_;
    if (!have_state) {
        end_detour("out of sight", 0);
    } else if (std::holds_alternative<cambc::map::camp_group_respawning>(st)) {
        end_detour("group respawning (done or taken by someone else)", 0);
    } else if (detour_turns_ > cfg::kDetourMaxTurns) {
        end_detour("too long (DEFAULT limit 150 turns)", cfg::kDetourBanRounds);  // DEFAULT D22: give up after 150 turns, avoid it for 150 rounds
    } else if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
        // Cleared: stand on the tile of the camp we were hitting until the capture completes (the same rule as for the planned camp).
        const vec2 tile = detour_tile_;
        if (my_pos() != tile)
            move_towards_closest(tiles_around(tile, 0));
        return true;
    } else {
        // Alive: hit its nearest living member in sight; walk toward it if none is in reach.
        const auto members = gi.group.camps();
        const cambc::camp* hit = nullptr;
        for (const auto& c : members)
            if (c.is_visible() && c.health() > 0 && (!hit || dist(my_pos(), c.position()) < dist(my_pos(), hit->position())))
                hit = &c;
        if (!hit) {
            end_detour("no living member in sight", 0);
        } else if (hit->health() >= hit->max_health() && detour_turns_ >= cfg::kDetourUnscratchedTurns && dist(my_pos(), hit->position()) <= reach) {
            end_detour("camp not scratched after 6 turns", cfg::kDetourBanRounds);  // DEFAULT D22
        } else {
            if (dist(my_pos(), hit->position()) > reach)
                move_towards_closest(tiles_around(hit->position(), reach));
            attack_camp(*hit);
            return true;
        }
    }
    return false;
}

// CHANGE 9 (b). What: look for a free camp within attack reach and start a detour on it. The first camp (nearest) seen this round whose group is
// alive or has its reward available, is not my own target's group, is not banned, and has fewer allies standing on its tiles than its capacity.
// Not while we stand capturing our own camp, nor while our own camp is itself in reach (we are already working on it).
// Inputs: the planned target and its group's index. Result: true if a detour started (and the camp was hit this very turn).
// Side effects: detour_* fields, a DETOUR "start" log line, an attack. Fuel: ~100 when no camp is in sight, ~2,500 when one is.
bool Bot::detour_start(const PlanTarget& planned, int planned_group) {
    auto& me = cambc::self();
    const vec2 planned_tile{planned.x, planned.y};
    // REFACTOR: with no camp in sight this round there is nothing to find, so skip the rest.
    if (seen_now_.empty())
        return false;
    const int reach = std::max(me.attack_range(), 1);
    if (capturing_ || (cambc::vision::has_camp(planned_tile) && dist(my_pos(), planned_tile) <= reach))
        return false;
    const CampSeen* found = nullptr;
    int found_group = -1;
    const vec2 here = my_pos();
    for (const CampSeen* csp : seen_now_) {  // the camps seen this round, in tile-key order
        const CampSeen& cs = *csp;
        if (cs.health <= 0 || dist(here, cs.pos) > reach)
            continue;
        const int gidx = group_of(cs.pos);
        if (gidx < 0 || gidx == planned_group || round_ < detour_ban_until_[static_cast<std::size_t>(gidx)])
            continue;
        const CampGroupInfo& gi = groups_[static_cast<std::size_t>(gidx)];
        // Free slot: fewer allies stand on the group's tiles than its capacity (a monolith group has capacity 0, so never free).
        int on_tiles = 0;
        for (const auto& a : ally_view())
            if (!(a == me))
                for (vec2 t : gi.tiles)
                    if (a.position() == t)
                        ++on_tiles;
        if (on_tiles >= gi.capacity)
            continue;
        // The group must be alive or have its reward available (not respawning).
        cambc::map::camp_group_state cand_state = cambc::map::camp_group_alive{};
        if (!group_state(gi.group, cand_state) || std::holds_alternative<cambc::map::camp_group_respawning>(cand_state))
            continue;  // unreadable or respawning (change 14: cached per round)
        if (!found || dist(my_pos(), cs.pos) < dist(my_pos(), found->pos)) {
            found = &cs;
            found_group = gidx;
        }
    }
    if (!found)
        return false;
    detour_group_ = found_group;
    detour_turns_ = 0;
    // The tile to stand on later: the group's member tile nearest to the camp we found.
    detour_tile_ = groups_[static_cast<std::size_t>(found_group)].tiles.front();
    for (vec2 t : groups_[static_cast<std::size_t>(found_group)].tiles)
        if (dist(t, found->pos) < dist(detour_tile_, found->pos))
            detour_tile_ = t;
    LogLine log;
    log.s("DETOUR ").s(name_).s(" r").n(round_).s(" start camp ").xy(found->pos.x, found->pos.y).s(" planned ").xy(planned.x, planned.y);
    log.flush();
    if (cambc::vision::has_camp(found->pos))
        attack_camp(cambc::vision::camp_at(found->pos));  // start hitting it this very turn
    return true;
}

// CHANGE 9. What: on the way to a FARM target, take a free camp that is within reach instead of walking past it (or standing beside it). Why: a
// champion was stuck beside another champion's camp for 206 rounds while its own target was 3 tiles away (evidence/change_09_free_slot_instance.txt).
// How: continue a running detour (detour_continue), otherwise look for a new one (detour_start). Inputs: the planned target.
// Result: true if this turn was spent on the detour. Side effects: those of the two pieces. Fuel: ~1,400 on average.
bool Bot::free_slot_detour(const PlanTarget& planned) {
    FUEL_SCOPE(FP_DETOUR);
    where_ = "free_slot";
    // REFACTOR: the group of my planned camp never changes, so it is looked up once in init (was group_of() on every call).
    const int planned_group = target_planned_group_.empty() ? group_of(vec2{planned.x, planned.y})
                                                            : target_planned_group_[std::min(target_idx_, target_planned_group_.size() - 1)];
    if (detour_continue())
        return true;
    return detour_start(planned, planned_group);
}
#endif

// What: "If a camp is visible and an ally is fighting it, attack it too until it dies." Only camps within our attack range matter, so look at just
// those (one board intersection instead of asking about every visible camp). Only when exactly one other champion of ours is in sight (a hand edit
// of the user's: `allies.size() > 1 && allies.size() < 3`, the count includes me).
// Inputs: none. Result: none. Side effects: at most one attack on a damaged camp. Fuel: ~300 when not applicable, ~3,000 when it is.
void Bot::help_allies_hit_camp() {
    auto& me = cambc::self();
    // REFACTOR: only the NUMBER of allies in sight is needed to decide whether to look at them, so count the occupied tiles (two engine calls)
    // instead of building the whole ally view (~1,000 fuel) on every turn of state 5. If the view was already built this round, use its size.
    const std::size_t visible_allies = ally_view_round_ == round_ ? ally_view_.size() : cambc::vision::champion_tiles(team::ally).count();
    if (visible_allies > 1 && visible_allies < 3) {
        const auto& allies = ally_view();
        for (vec2 pos : (cambc::vision::camp_tiles() & me.attack_tiles()).positions()) {
            const auto c = cambc::vision::camp_at(pos);
            if (c.health() <= 0 || c.health() >= c.max_health())
                continue;
            bool ally_near = false;
            for (const auto& a : allies)
                if (!(a == me) && dist(a.position(), pos) <= cfg::kAllyNearCampRange)
                    ally_near = true;
            if (ally_near && attack_camp(c))
                break;
        }
    }
}

// What: the target camp's group is in sight: if its monsters are dead, stand on the camp tile to capture it ("Once the enemy at the target camp is
// dead, capture it"); if we just completed a FARM capture (respawning while capturing_), log it and switch to the other target (D7).
// Inputs: the target, its tile and its group. Result: true if the turn is over, false to go on (hit the camp, walk the route).
// Side effects: capturing_, respawn_at_ (change 11), a CAMP log line, a target switch, a step. Fuel: ~700.
bool Bot::farm_group_step(const PlanTarget& t, vec2 camp_tile, const cambc::map::camp_group& group) {
    cambc::map::camp_group_state st = cambc::map::camp_group_alive{};
    group_state(group, st);  // (change 14: cached per round; default "alive" on failure, as before)
    if (std::holds_alternative<cambc::map::camp_group_reward_available>(st)) {
        if (my_pos() == camp_tile)
            capturing_ = true;
        else
            follow_forward(target_tiles(), static_cast<int>(target_idx_));
        return true;
    }
    if (std::holds_alternative<cambc::map::camp_group_respawning>(st) && capturing_) {
        // We completed a capture of a FARM camp.
        capturing_ = false;
#if V2_CHANGE_WAIT_STATE
        if (const auto* rs = std::get_if<cambc::map::camp_group_respawning>(&st))  // change 11: remember when the camp we just took is back
            respawn_at_[std::min<std::size_t>(target_idx_, 1)] = round_ + (rs->timer > 0 ? static_cast<int>(rs->timer) : cfg::respawn_rounds(t.kind));
#endif
        LogLine log;
        log.s("CAMP ").s(name_).s(" r").n(round_).s(" captured ").xy(t.x, t.y).s(" by me (farm)");
        log.flush();
#if !V2_NO_SWITCH_AFTER_FARM_CAPTURE
        switch_target("farm camp captured");
        return true;
#endif
    }
    return false;
}

// STATE 5 (transition_table.md): the default. Walk the route to the target camp, help allies, take a free camp on the way, capture when cleared.
// Entry condition: none (it runs when no earlier state handled the turn). Inputs: none. Result: none (always handles the turn).
// Side effects: those of the pieces above, plus hitting the target camp before and after the step. Fuel: ~5,900 on average.
void Bot::state_default() {
    ++acts_[5];
    where_ = "default";
    const PlanTarget* t = target();
    if (!t) {
        // No plan for this champion (or no target): DEFAULT: fight whatever camp is in reach, like the starter bot.
        for (const auto& c : cambc::vision::camps())
            if (attack_camp(c))
                return;
        return;
    }
    help_allies_hit_camp();
#if V2_CHANGE_FREE_SLOT
    // CHANGE 9: on the way to a FARM target, a free camp in reach is taken first (see free_slot_detour).
    if (!t->attack && free_slot_detour(*t))
        return;
#endif
    const vec2 camp_tile{t->x, t->y};
    const cambc::map::camp_group* group = target_group();
    if (group && farm_group_step(*t, camp_tile, *group))
        return;

    // Hit the target camp's monster if it is in reach, walk the path, and try again after the step.
    auto hit_target = [&]() {
        if (cambc::vision::has_camp(camp_tile))
            return attack_camp(cambc::vision::camp_at(camp_tile));
        return false;
    };
    hit_target();
    follow_forward(target_tiles(), static_cast<int>(target_idx_));
    hit_target();
}
