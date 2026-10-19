#pragma once

#include <cambc.h>
#include <cstdint>
#include <vector>
#include "route_info.h"

namespace draft {

// The type and map location of a camp included in a camp record.
struct CampMember {
    // Monster type that identifies this camp.
    cambc::camp_type type{};
    // Tile occupied by this camp on the map.
    cambc::vec2 position{0, 0};
};

// Draft-time strategy data for a camp cluster. This draft keeps one anchor camp
// for each cluster of adjacent camps.
struct CampInfo {
    // Camp members represented by this record. The current draft stores only the
    // cluster's anchor; adjacent camps grouped with it are omitted.
    std::vector<CampMember> members;
    // Shortest walking distance from an allied spawn to the anchor, when computed.
    // -1 means the distance is unknown or the anchor is unreachable.
    int our_distance = -1;
    // Relative farming priority from 0 to 255; higher values favor nearer camps.
    std::uint8_t farm_score = 0;
    // Reserved for combat-priority scoring. This draft does not calculate it.
    std::uint8_t combat_score = 0;
    // MERGED: the cluster's strength (difficulty): the sum over every camp in the cluster of monolith 5, elk 2, wolf 1, gem or gold cluster 1.
    // Target selection gives a camp this many champion slots. Capped at 255 in memory.
    std::uint8_t strength = 1;
    // Reconstructed path from an allied spawn to the anchor. When available,
    // this is a shortest path; see AllyRoute for its start and direction encoding.
    AllyRoute ally_route;
    // True when this camp was selected using straight-line distance because the
    // exact BFS was skipped or unfinished. In that case, no shortest-path
    // guarantee is made and ally_route may be unavailable. False indicates the
    // camp was found by BFS, whose available route is shortest.
    bool estimatedRoute = false;
};

}  // namespace draft
