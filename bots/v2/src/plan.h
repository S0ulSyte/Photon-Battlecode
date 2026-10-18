// plan.h - the draft's analysis result ("the plan") and how it is packed into / read back from draft memory.
//
// Reads/writes: nothing (pure data and functions; the draft fills a Plan, champions decode it). Edit here to change: the draft-memory byte layout
// (bump the version byte in plan.cpp when you do - currently 3), what a Plan holds.
//
// The DRAFT computes, for the current map: the shortest walking route from our spawn to every altar, and to the camps each champion
// should target (using target_select). It stores that in draft memory. Each CHAMPION reads it back on its first turn.
//
// Draft memory layout (version 3; whole bytes; coordinates are map tiles, x = column, y = row; north is +y):
//   byte 0  format version (3; was 2 before change 10 added the reward bytes)        byte 1  map width        byte 2  map height
//   byte 3  A = number of altar entries                         byte 4  T = number of target entries
//   then A altar entries, closest altar first:
//       x, y, start_x, start_y, S, then ceil(S/2) bytes holding the S steps, two per byte (first step in the LOW 4 bits)
//   then T target entries, ordered by champion slot, best target first:
//       slot_and_mode, kind, gems, gold, x, y, start_x, start_y, S, then the packed steps   (gems and gold: the camp group's reward, capped at 255)
//         slot_and_mode: bits 0-2 = champion slot (0 Issac, 1 Cassi, 2 Vendetta, 3 Ryth, 4 Marcella, 5 Varg),
//                        bit 7 = 1 if the camp was chosen for ATTACKING, 0 if for FARMING
//         kind: the camp_type number (0 wolf, 1 elk, 2 gem cluster, 3 gold cluster, 4 monolith)
//   A step is a direction code 0-7 = (dx,dy): 0=(-1,-1) 1=(0,-1) 2=(1,-1) 3=(-1,0) 4=(1,0) 5=(-1,1) 6=(0,1) 7=(1,1).
//   A route starts on a spawn tile (start_x, start_y) and ends on the target camp's tile, or (altars) on the tile next to the altar.
#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Distance value meaning "this tile cannot be reached".
constexpr std::uint16_t kUnreachable = 65535;
// Number of champion slots in the plan (Issac, Cassi, Vendetta, Ryth, Marcella, Varg).
constexpr int kPlanSlots = 6;

// A route: where it starts and the direction code of each step.
struct PlanPath {
    int start_x = 0, start_y = 0;
    std::vector<std::uint8_t> steps;
};

// An altar: its own tile and the route to the tile next to it.
struct PlanAltar {
    int x = 0, y = 0;
    PlanPath path;
};

// A camp a champion should target.
struct PlanTarget {
    int slot = 0;          // which champion
    bool attack = false;   // true = chosen for attacking, false = for farming
    int kind = 0;          // camp_type number
    int gems = 0, gold = 0;  // CHANGE 10: what capturing the camp's group pays (capped at 255 in the memory)
    int x = 0, y = 0;      // the camp's tile
    PlanPath path;         // route from our spawn to the camp's tile
};

struct Plan {
    int width = 0, height = 0;
    std::vector<PlanAltar> altars;     // closest first
    std::vector<PlanTarget> targets;   // by slot, best first
};

// A camp as the draft sees it.
struct PlanCamp {
    int kind = 0;
    int x = 0, y = 0;
    // CHANGE 10: what capturing the camp's group pays (0 when unknown). Last in the struct so older brace-initialisers {kind, x, y} still work.
    int gems = 0, gold = 0;
};

// Everything build_plan needs.
struct PlanInput {
    int width = 0, height = 0;
    // Row-major (index = y * width + x) number of steps from our spawn tiles / from the enemy's spawn tiles to each tile.
    // kUnreachable where there is no walking route. `enemy_dist` may be empty (then every combat score is 0).
    std::vector<std::uint16_t> our_dist;
    std::vector<std::uint16_t> enemy_dist;
    std::vector<PlanCamp> camps;
    std::vector<std::pair<int, int>> altars;
    // How close (Chebyshev) you must stand to an altar to use it.
    int reach = 1;
};

// The route from a root tile (distance 0) to (end_x, end_y), following strictly decreasing distances. `dist` is row-major.
// The end tile must have been reached (dist < kUnreachable).
PlanPath trace_path(const std::vector<std::uint16_t>& dist, int width, int height, int end_x, int end_y);

// Scores the camps, picks every champion's targets (target_select), and finds the routes.
Plan build_plan(const PlanInput& input);

// Packs a plan into at most `capacity` bytes. If it does not fit, the last target entries (highest slots) are left out.
std::vector<std::uint8_t> encode_plan(const Plan& plan, std::size_t capacity);

// Reads a plan back. Returns false if the bytes are not a valid version-3 plan.
bool decode_plan(const std::uint8_t* data, std::size_t size, Plan& out);

// The tiles of a route in walking order (the start tile first).
std::vector<std::pair<int, int>> path_tiles(const PlanPath& path);
