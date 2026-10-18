#pragma once

#include <cambc.h>
#include <cstdint>
#include <vector>

namespace draft {

// One shortest route from an ally spawn tile to a camp or altar approach tile.
struct AllyRoute {
    // Starting spawn tile for the sequence of direction codes.
    cambc::vec2 start{0, 0};
    // Forward direction codes: 0 north, 1 northeast, 2 east, 3 southeast, 4 south, 5 southwest, 6 west, 7 northwest.
    std::vector<std::uint8_t> directions;
    // False means no route exists or reconstruction did not complete successfully.
    bool available = false;
};

}  // namespace draft
