#pragma once

#include <cambc.h>
#include "route_info.h"

namespace mack_draft {

// One altar and the shortest distance from each team's spawn to a tile in tribute range.
struct AltarInfo {
    // The altar's tile; it is blocked, so routes end on an adjacent tile in submission range instead.
    cambc::vec2 position{0, 0};
    // Number of walking steps to the closest reachable submission tile; -1 means unreachable.
    int our_distance = -1;
    int enemy_distance = -1;
    // Ally route ends on the closest reachable tile from which gems can be submitted.
    AllyRoute ally_route;
};

}  // namespace mack_draft
