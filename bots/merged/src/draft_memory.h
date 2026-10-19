// draft_memory.h - reads the starter-cpp draft's memory (format version 9, written by draft.cpp) into v2's Plan.
//
// The merged bot drafts with starter-cpp's draft.cpp (picks + map analysis) and plays with v2's gameplay files. v2's gameplay reads a Plan
// (plan.h: per-champion camp targets and altar routes); starter's draft stores something else: camp clusters with farm scores and routes, and
// altars with routes. This file translates between the two:
//   * starter record "camp cluster": anchor camp (type, x, y), farm_score, combat_score, route  ->  a TargetCandidate for v2's select_all_targets,
//     which hands every champion slot its two camps exactly as v2's own draft would (every champion can run it, so no extra memory is needed);
//   * starter route: start tile + compass direction codes (0 N, 1 NE, 2 E, 3 SE, 4 S, 5 SW, 6 W, 7 NW, 16-bit length, 65535 = no route)
//     ->  v2 PlanPath: start tile + v2 direction codes (0=(-1,-1) 1=(0,-1) 2=(1,-1) 3=(-1,0) 4=(1,0) 5=(-1,1) 6=(0,1) 7=(1,1)), found by looking
//     the SDK's own unit vector of each compass code up in v2's table (so the two y-axis conventions cannot be confused);
//   * a route the draft could not store (no memory left / estimated camp) is computed now with the engine's distance field, walls, holes, altars and
//     the enemy spawn solid, for this champion's own targets (`my_slot`) and the nearest altar only; a straight line is the last resort.
#pragma once
#include <cstddef>
#include <cstdint>
#include "plan.h"

// Fills `out` from starter draft memory. Returns false if the bytes are not a valid version-9 record (out is then empty).
// Call during play (needs the SDK's spawn region for the straight-line fallback). Scores are the draft's, never recomputed.
bool decode_starter_memory(const std::uint8_t* data, std::size_t size, Plan& out, int my_slot);
