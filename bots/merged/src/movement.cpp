// movement.cpp - My position (cached), spawn tests, and walking along a route.
//
// Reads: my tile, the route tiles, the engine (blocked tiles). Writes: moves (one step), pos_ cache.
// Edit here to change: how routes are walked (follow_forward / follow_backward), what "near spawn" means (D17).
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

// What: my tile, from a cache. Why: asking the engine costs ~100 fuel and the bot asked ~10 times per turn. How: re-read only when the round or the
// move counter (moves.h) has changed; every movement call bumps the counter first.
// Result: my tile. Side effects: refreshes the cache (one engine call, ~100) when the round or the move counter changed; otherwise free.
cambc::vec2 Bot::my_pos() const {
    if (pos_round_ != round_ || pos_epoch_ != g_move_epoch) {
        pos_ = cambc::self().position();
        pos_round_ = round_;
        pos_epoch_ = g_move_epoch;
    }
    return pos_;
}

// What: am I standing on one of our spawn tiles?
// Result: true if I stand on one of our spawn tiles. Fuel: ~120.
bool Bot::at_spawn() const {
    FUEL_SCOPE(FP_AT_SPAWN);
    return cambc::map::spawn_region(team::ally).is_set(my_pos());
}

// What: how many steps from spawn my nearest tile of a route is (D17). Inputs: the route tiles. Result: the step count. No engine call.
int Bot::steps_from_spawn_on(const std::vector<std::pair<int, int>>& tiles) const {
    const vec2 here = my_pos();
    int best = 1000000;
    for (std::size_t i = 0; i < tiles.size(); ++i)
        best = std::min(best, static_cast<int>(i) + dist(here, to_vec(tiles[i])));
    return best;
}

// What: take one step along a route toward its end (the target camp / the altar), or toward the route if I am not on it.
// Why: a champion walks the planned route (shortest path computed in the draft) instead of path-finding every turn.
// Inputs: the route tiles and which cached "all tiles of the route" board to use. Result: none. Side effects: one step along the route (or toward it). Fuel: ~2,300.
void Bot::follow_forward(const std::vector<std::pair<int, int>>& tiles, int goals_slot) {
    FUEL_SCOPE(FP_FOLLOW);
    where_ = "follow_forward";
    auto& me = cambc::self();
    if (tiles.empty() || !me.can_move())
        return;
    const vec2 here = my_pos();
    int on = -1;
    {
    FUEL_SCOPE(FP_FOLLOW_FIND);
    for (std::size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i].first == here.x && tiles[i].second == here.y) {
            on = static_cast<int>(i);
            break;
        }
    }
    if (on < 0) {
        // Not on the path yet: walk to the nearest tile of it.
        if (!goals_[goals_slot]) {
            goals_[goals_slot].emplace();
            goals_[goals_slot]->clear();
            for (const auto& t : tiles)
                goals_[goals_slot]->set(to_vec(t));
        }
        move_towards_closest(*goals_[goals_slot]);
    } else if (on + 1 < static_cast<int>(tiles.size())) {
        const vec2 next = to_vec(tiles[static_cast<std::size_t>(on + 1)]);
        if (!tile_is_blocked(next, here)) {  // (refactor: was blocked_tiles().is_set(next))
            try {
                note_moved();  // (refactor) the cached position is stale from here on
                me.move_absolute(next);
            } catch (const cambc::error&) {
            }
        }
    }
}

// What: like follow_forward but walks the route toward its start (home). Same inputs and effects. Fuel: ~2,300.
void Bot::follow_backward(const std::vector<std::pair<int, int>>& tiles, int goals_slot) {
    FUEL_SCOPE(FP_FOLLOW);
    auto& me = cambc::self();
    if (tiles.empty() || !me.can_move())
        return;
    const vec2 here = my_pos();
    int on = -1;
    for (std::size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i].first == here.x && tiles[i].second == here.y) {
            on = static_cast<int>(i);
            break;
        }
    if (on < 0) {
        if (!goals_[goals_slot]) {
            goals_[goals_slot].emplace();
            goals_[goals_slot]->clear();
            for (const auto& t : tiles)
                goals_[goals_slot]->set(to_vec(t));
        }
        move_towards_closest(*goals_[goals_slot]);
    } else if (on > 0) {
        const vec2 next = to_vec(tiles[static_cast<std::size_t>(on - 1)]);
        if (!tile_is_blocked(next, here)) {  // (refactor: was blocked_tiles().is_set(next))
            try {
                note_moved();  // (refactor) the cached position is stale from here on
                me.move_absolute(next);
            } catch (const cambc::error&) {
            }
        }
    }
}

