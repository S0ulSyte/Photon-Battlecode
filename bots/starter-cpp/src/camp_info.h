#pragma once

#include <cambc.h>
#include <cstdint>
#include <vector>
#include "route_info.h"

namespace mack_draft {

// One monster in a camp group and its original map tile.
struct CampMember {
    cambc::camp_type type{};
    cambc::vec2 position{0, 0};
};

// Strategy data for one camp group; connected members share one set of distances and scores.
struct CampInfo {
    std::vector<CampMember> members;
    // Shortest distance to any member from each team's spawn; -1 means unreachable.
    int our_distance = -1;
    int enemy_distance = -1;
    // 0-255; higher scores indicate a nearby camp from our/enemy spawn, respectively.
    std::uint8_t farm_score = 0;
    std::uint8_t combat_score = 0;
    // Shortest route from an ally spawn to the nearest reachable member.
    AllyRoute ally_route;
};

}  // namespace mack_draft
