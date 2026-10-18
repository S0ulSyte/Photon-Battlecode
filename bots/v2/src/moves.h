// moves.h - small map and movement helpers shared by the bot (adapted from the starter bot).
//
// Reads: the map and what is in sight (engine calls). Writes: one step of movement (move_towards_closest).
// Edit here to change: what counts as blocked (blocked_tiles / tile_is_blocked), how the bot path-finds to a set of goal tiles (move_towards_closest).
// Fuel: blocked_tiles ~1,000; move_towards_closest ~6,000; tile_is_blocked ~500.
#pragma once
#include <cambc.h>

// Chebyshev distance (diagonals count as 1).
int dist(cambc::vec2 a, cambc::vec2 b);
// The tiles within Chebyshev `radius` of `position`.
cambc::bitboard tiles_around(cambc::vec2 position, int radius);
// Tiles we cannot step onto: walls, holes, altars, the enemy spawn, and every champion, camp and teleport we can see.
cambc::bitboard blocked_tiles();
// REFACTOR: the same answer as blocked_tiles().is_set(tile) when `tile` is one tile (and `here` is my own tile, which blocked_tiles() leaves free),
// from four cheap per-tile engine queries instead of building the whole blocked board (about half the fuel).
bool tile_is_blocked(cambc::vec2 tile, cambc::vec2 here);
// One step along a shortest walking route to the nearest tile of `goals` (the route goes around walls and units).
// Returns true if we moved. Does nothing if we are already on a goal tile.
bool move_towards_closest(cambc::bitboard goals);

// REFACTOR (stage 1): a counter that goes up every time this champion tries to move (a step or a dash). Bot::my_pos() keeps my tile cached and
// re-reads it only when this counter or the round has changed, because asking the engine for my position costs ~100 fuel and the bot asked ~10
// times per turn. Every call that can move the champion calls note_moved() BEFORE it moves (also if the move later fails: a spare re-read is harmless).
inline int g_move_epoch = 0;
inline void note_moved() { ++g_move_epoch; }
