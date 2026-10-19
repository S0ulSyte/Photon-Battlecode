// moves.cpp - see moves.h. Adapted from the starter bot's helpers.
//
// Reads: the map masks and what is in sight. Writes: one step (me.move_towards / move_absolute), bumping g_move_epoch first.
// Edit here to change: the path-finding of move_towards_closest or the definition of a blocked tile.
#include "moves.h"
#include "fuel_profile.h"

using cambc::vec2;

// What: Chebyshev distance (diagonals count as 1). Inputs: two tiles. Result: the distance. No engine call.
int dist(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

// What: the board of tiles within Chebyshev `radius` of a tile. Inputs: the tile and radius. Result: the board. Fuel: ~300 (two engine calls).
cambc::bitboard tiles_around(vec2 position, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    tiles.draw_chebyshev(position, radius);
    return tiles;
}

// What: the part of "blocked" that never changes: walls, holes, altars and the enemy spawn. Why: it was OR-ed together on every call. How: built on the
// first call (the map masks are cached by the SDK, so this needs no engine call) and kept.
static const cambc::bitboard& fixed_obstacles() {
    static const cambc::bitboard fixed =
        cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(cambc::team::enemy);
    return fixed;
}

// What: blocked tiles when the caller already has the camp tiles and my own tile. Why: move_towards_closest needs the camp tiles twice and my tile
// several times; each is an engine call. How: the fixed obstacles, plus every visible champion, the given camps and every teleport; my own tile is free.
static cambc::bitboard blocked_tiles_with(const cambc::bitboard& camps, cambc::vec2 here) {
    FUEL_SCOPE(FP_BLOCKED);
    auto blocked = fixed_obstacles() | cambc::vision::champion_tiles() | camps | cambc::vision::teleport_tiles();
    blocked.set(here, false);
    return blocked;
}

// Inputs: a tile and my own tile. Result: true if the tile is blocked (see moves.h). Fuel: ~500.
bool tile_is_blocked(cambc::vec2 tile, cambc::vec2 here) {
    FUEL_SCOPE(FP_BLOCKED);
    if (tile == here)
        return false;  // blocked_tiles() frees my own tile
    return fixed_obstacles().is_set(tile) || cambc::vision::has_champion(tile) || cambc::vision::has_camp(tile) || cambc::vision::has_teleport(tile);
}

// What: every tile we cannot step onto (walls, holes, altars, the enemy spawn, every visible champion, camp and teleport; my own tile is free).
// Inputs: none. Result: the board. Fuel: ~1,000.
cambc::bitboard blocked_tiles() {
    return blocked_tiles_with(cambc::vision::camp_tiles(), cambc::self().position());
}

// What: take one step along a shortest walking route to the nearest tile of `goals` (routes go around walls and units).
// Inputs: the goal board. Result: true if we moved (false if already on a goal, no goal reachable, or the step failed).
// Side effects: one move (after note_moved()). Fuel: ~6,000 on average (the distance field and the bitboard operations).
bool move_towards_closest(cambc::bitboard goals) {
    FUEL_SCOPE(FP_MOVE);
    auto& me = cambc::self();
    if (!me.can_move())
        return false;
    const cambc::vec2 here = me.position();  // (refactor: one engine call; the old code asked for it several times)
    if (goals.is_set(here))
        return false;

    // Only free goals or occupied camp tiles can seed the field.
    const cambc::bitboard camps = cambc::vision::camp_tiles();  // (refactor: asked once; the old code asked for it twice)
    const auto blocked = blocked_tiles_with(camps, here);
    goals &= ~blocked | camps;
    if (goals.is_empty())
        return false;
    try {
        if (goals.count() == 1 && !blocked.is_set(goals.positions()[0])) {
            note_moved();  // (refactor) the cached position is stale from here on
            return me.move_towards(goals.positions()[0]);
        }

    // Expand from the goals until our current tile is reached.
    const auto path_walls = blocked & ~goals;
    cambc::distance_field field;
    cambc::bitboard visited;
    {
    FUEL_SCOPE(FP_MOVE_FIELD);
    field.compute_to(path_walls, goals, tiles_around(here, 0), visited);
    }

    // Choose a reachable step with a smaller distance to the goal.
    auto step = here;
    {
    FUEL_SCOPE(FP_MOVE_REACH);
    static const int move_speed = me.base_stats().base_move_speed;  // (refactor: a champion's speed never changes; was read on every call)
    // (refactor) The distance of the best tile so far is kept in a variable instead of asking the field for it again at every comparison.
    bool have_best = false;
    std::uint16_t best_distance = 0;
    for (auto tile : blocked.reachable_tiles(step, move_speed).positions()) {
        const std::uint16_t d = field.get(tile);
        if (!have_best) {
            best_distance = field.get(step);
            have_best = true;
        }
        if (d < best_distance) {
            step = tile;
            best_distance = d;
        }
    }
    }
        if (step != here)
            note_moved();  // (refactor) the cached position is stale from here on
        return step != here && me.move_absolute(step);
    } catch (const cambc::error&) {
        // A destination turned out to be occupied (for example by a unit we could not see): stay put this turn.
        return false;
    }
}
