// random.h - a seeded random number generator (from the starter bot). v2 does not call it today; kept because the starter and v1 use the pattern.
// Reads: the spawn region and my starting tile. Writes: nothing. Edit here to change: the seed. (WASI randomness is always zero in matches.)
#pragma once
#include <cambc.h>
#include <cstdint>
#include <random>

// WASI randomness is always zero in matches, so seed from our spawn and this champion's start.
inline std::uint32_t random_seed() {
    const auto spawn = cambc::map::spawn_region(cambc::team::ally).positions()[0];
    std::uint32_t seed = spawn.x * 73856093u ^ spawn.y * 19349663u;
    if (!cambc::is_draft()) {
        const auto start = cambc::self().position();
        seed ^= start.x * 83492791u ^ start.y * 2654435761u;
    }
    return seed;
}

// Seed once; keep the generator across turns.
inline unsigned random_number(unsigned limit) {
    static std::mt19937 random{random_seed()};
    return std::uniform_int_distribution<unsigned>{0, limit - 1}(random);
}
