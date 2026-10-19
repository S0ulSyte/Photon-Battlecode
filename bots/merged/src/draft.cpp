#include <cambc.h>
#include "altar_info.h"
#include "camp_info.h"
#include "draft.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

namespace {

using draft::CampInfo;
using draft::CampMember;
using draft::AltarInfo;
using draft::AllyRoute;

// Analysis results collected during drafting and serialized for the champions.
struct DraftInfo {
    // Camp clusters ordered from nearest to farthest when exact search is used.
    std::vector<CampInfo> camps;
    // Altars, with the closest allied submission tile and route when analysis permits.
    std::vector<AltarInfo> altars;
    // Number of camps omitted because they were adjacent to a retained cluster anchor.
    std::size_t adjacent_camps_ignored = 0;
    // Number of camp clusters discovered on the map.
    std::size_t camp_clusters = 0;
    // Whether the BFS reached all camps or exhausted all reachable tiles.
    bool search_complete = false;
    // Whether camp ranking fell back to straight-line distance instead of exact path distance.
    bool approximate = false;
};

// Maximum number of camps retained when an exact path analysis is not available.
constexpr std::size_t approximate_camps_to_save = 10;
// Keep exact searches for compact or narrow maps that fit the tested draft budget.
constexpr std::size_t max_exact_search_tiles = 2500;
// A narrow map can still be searched exactly even when its total tile count is large.
constexpr int max_exact_search_thin_dimension = 30;
// Preserve fuel for the rest of the draft and game logic while analysis runs.
constexpr std::uint64_t normal_fuel_reserve = 55000;
// The SDK uses the largest uint16 value to represent a tile that was not reached.
constexpr std::uint16_t unknown_distance = cambc::distance_field::unreachable;
// Direction indices are part of the route byte format; keep this order stable.
constexpr std::array<cambc::vec2, 8> route_directions{
    cambc::vec2::north,      cambc::vec2::north_east, cambc::vec2::east,      cambc::vec2::south_east,
    cambc::vec2::south,      cambc::vec2::south_west, cambc::vec2::west,      cambc::vec2::north_west};

// MERGED: the strength (difficulty) of one camp: monolith 5, elk 2, wolf 1; gem and gold clusters (they do not fight back) 1 like a wolf.
int camp_strength(cambc::camp_type type) {
    switch (type) {
    case cambc::camp_type::monolith: return 5;
    case cambc::camp_type::elk: return 2;
    default: return 1;  // wolf, gem cluster, gold cluster
    }
}

// MERGED: how much a camp type is worth; its farm score is multiplied by this (monolith 0.1, elk 1, wolf 0.9, gem cluster 0.7, gold cluster 0.6).
double camp_value(cambc::camp_type type) {
    switch (type) {
    case cambc::camp_type::monolith: return 0.1;
    case cambc::camp_type::elk: return 1.0;
    case cambc::camp_type::wolf: return 0.9;
    case cambc::camp_type::gem_cluster: return 0.7;
    case cambc::camp_type::gold_cluster: return 0.6;
    default: return 1.0;
    }
}

// MERGED: a closeness score (0-255) times the camp's value, rounded and kept in a byte.
std::uint8_t farm_score_of(double closeness, const CampInfo& camp) {
    const double value = camp.members.empty() ? 1.0 : camp_value(camp.members.front().type);
    const double score = closeness * value;
    return static_cast<std::uint8_t>(std::min(255.0, std::max(0.0, score + 0.5)));
}

// Store a 16-bit value as two little-endian bytes for draft memory.
void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

// Encode -1 as the distance-field unreachable marker.
void append_distance(std::vector<std::uint8_t>& bytes, int distance) {
    append_u16(bytes, distance < 0 ? unknown_distance : static_cast<std::uint16_t>(distance));
}

// Reconstruct a route using an SDK distance field (used by alternate analysis paths).
bool make_ally_route(const cambc::distance_field& distances, cambc::vec2 target, AllyRoute& route) {
    route = {};
    if (distances.get(target) == cambc::distance_field::unreachable)
        return false;

    // next_move_from points toward the distance-field roots, so collect a reversed path from target to spawn.
    std::vector<std::uint8_t> reverse_directions;
    cambc::vec2 current = target;
    while (distances.get(current) != 0) {
        // Leave room for the reserved 65535 route-length marker.
        if (reverse_directions.size() >= unknown_distance - 1)
            return false;

        const auto step_to_root = distances.next_move_from(current);
        if (!step_to_root)
            return false;

        std::size_t direction = 0;
        while (direction < route_directions.size() && route_directions[direction] != *step_to_root)
            ++direction;
        if (direction == route_directions.size())
            return false;

        reverse_directions.push_back(static_cast<std::uint8_t>(direction));
        current = current + *step_to_root;
    }

    // Reverse the collected path and invert every step to store it from spawn toward the target.
    route.start = current;
    route.directions.reserve(reverse_directions.size());
    for (auto it = reverse_directions.rbegin(); it != reverse_directions.rend(); ++it)
        route.directions.push_back(static_cast<std::uint8_t>((*it + 4) % route_directions.size()));
    route.available = true;
    return true;
}

// Number of serialized bytes required for a route, including its start and length header.
std::size_t route_record_size(const AllyRoute& route) {
    if (!route.available || route.directions.size() >= unknown_distance)
        // Unavailable route: x, y, and the reserved 16-bit route-length marker.
        return 4;
    // The route header is four bytes; two 4-bit direction codes share each following byte.
    return 4 + (route.directions.size() + 1) / 2;
}

// Append a route header and packed direction codes; unavailable routes use the reserved marker.
void append_route(std::vector<std::uint8_t>& bytes, const AllyRoute& route) {
    const bool available = route.available && route.directions.size() < unknown_distance;
    // 255,255 plus length 65535 marks a route that could not be stored.
    bytes.push_back(available ? static_cast<std::uint8_t>(route.start.x) : 255);
    bytes.push_back(available ? static_cast<std::uint8_t>(route.start.y) : 255);
    append_u16(bytes, available ? static_cast<std::uint16_t>(route.directions.size()) : unknown_distance);

    if (!available)
        return;
    // Directions fit in four bits (0-7); store the first in the low half and the next in the high half.
    for (std::size_t i = 0; i < route.directions.size(); i += 2) {
        std::uint8_t packed = route.directions[i];
        if (i + 1 < route.directions.size())
            packed = static_cast<std::uint8_t>(packed | (route.directions[i + 1] << 4));
        bytes.push_back(packed);
    }
}

// Treat camps whose x and y coordinates differ by at most one as a single cluster.
bool camps_are_adjacent(cambc::vec2 a, cambc::vec2 b) {
    return a.x >= b.x - 1 && a.x <= b.x + 1 && a.y >= b.y - 1 && a.y <= b.y + 1;
}

struct CampAnalysis {
    // Initialization and search lifecycle flags.
    bool initialized = false;
    bool complete = false;
    // Map dimensions used to convert between 2D positions and linear tile indices.
    int width = 0;
    int height = 0;
    // Index of the next discovered tile to process in the BFS queue.
    std::size_t queue_head = 0;
    // Count of nearby camps omitted during clustering.
    std::size_t adjacent_camps_ignored = 0;
    // Combined obstacle mask and its byte-per-tile form for fast BFS checks.
    cambc::bitboard blocked;
    std::vector<std::uint8_t> blocked_tiles;
    // Camp cluster index at each tile, or -1 when that tile is not an anchor.
    std::vector<int> camp_at_tile;
    // One representative camp for each adjacent-camp cluster.
    std::vector<CampMember> anchors;
    // MERGED: the cluster strength of each anchor (same index as `anchors`): the sum of camp_strength over every camp in the cluster.
    std::vector<std::uint8_t> anchor_strength;
    // Allied spawn tiles used as the simultaneous BFS starting points.
    std::vector<cambc::vec2> spawn_roots;
    // Tracks whether each anchor has already been added to the results.
    std::vector<bool> found;
    // Shortest distance from any allied spawn to each tile, or unknown_distance.
    std::vector<std::uint16_t> distances;
    // For each reached tile, direction to its BFS predecessor toward a spawn.
    std::vector<std::uint8_t> parent_directions;
    // FIFO work queue implementing the breadth-first search.
    std::vector<std::size_t> queue;
    // Results accumulated for serialization.
    DraftInfo info;
};

// Persistent draft-time working state; analysis continues across draft turns.
CampAnalysis camp_analysis;

// Flatten an in-bounds map coordinate into the arrays used by the search.
std::size_t tile_index(cambc::vec2 position, int width) {
    return static_cast<std::size_t>(position.y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(position.x);
}

void choose_straight_line_camps(CampAnalysis& state);

// Gather map objectives, collapse adjacent camps, and prepare the exact BFS when affordable.
void initialize_camp_analysis() {
    CampAnalysis& state = camp_analysis;
    state.width = cambc::map::width();
    state.height = cambc::map::height();
    const std::size_t tile_count = static_cast<std::size_t>(state.width) * state.height;

    // Collect every camp, then keep the first discovered camp as each cluster's anchor.
    std::vector<CampMember> discovered;
    for (const auto& group : cambc::map::camp_groups())
        for (const auto& camp : group.camps())
            discovered.push_back({camp.kind(), camp.spawn_position()});

    state.camp_at_tile.assign(tile_count, -1);
    std::vector<bool> clustered(discovered.size(), false);
    for (std::size_t first = 0; first < discovered.size(); ++first) {
        if (clustered[first])
            continue;

        std::vector<std::size_t> cluster{first};
        clustered[first] = true;
        for (std::size_t current = 0; current < cluster.size(); ++current) {
            const cambc::vec2 position = discovered[cluster[current]].position;
            for (std::size_t candidate = 0; candidate < discovered.size(); ++candidate) {
                if (!clustered[candidate] &&
                    camps_are_adjacent(position, discovered[candidate].position)) {
                    clustered[candidate] = true;
                    cluster.push_back(candidate);
                }
            }
        }

        // MERGED: the strength of the whole cluster (every camp in it counts, not only the anchor we keep).
        int cluster_strength = 0;
        for (const std::size_t member : cluster)
            cluster_strength += camp_strength(discovered[member].type);

        const std::size_t index = state.anchors.size();
        state.anchors.push_back(discovered[first]);
        state.anchor_strength.push_back(static_cast<std::uint8_t>(std::min(cluster_strength, 255)));
        state.camp_at_tile[tile_index(discovered[first].position, state.width)] = static_cast<int>(index);
        state.adjacent_camps_ignored += cluster.size() - 1;
    }

    state.info.camp_clusters = state.anchors.size();
    state.info.adjacent_camps_ignored = state.adjacent_camps_ignored;
    for (const cambc::vec2 position : cambc::map::altars().positions())
        state.info.altars.push_back({position});

    state.spawn_roots = cambc::map::spawn_region(cambc::team::ally).positions();
    const bool exact_search_by_size =
        tile_count <= max_exact_search_tiles || std::min(state.width, state.height) <= max_exact_search_thin_dimension;
    if (!exact_search_by_size) {
        choose_straight_line_camps(state);
        state.initialized = true;
        return;
    }

    // Walls, holes, and altar tiles cannot be traversed during the path search.
    state.blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    const auto blocked_bits = state.blocked.export_linear_buffer();
    state.blocked_tiles.resize(tile_count);
    for (std::size_t index = 0; index < tile_count; ++index)
        state.blocked_tiles[index] = static_cast<std::uint8_t>((blocked_bits[index / 8] >> (index % 8)) & 1);

    state.found.assign(state.info.camp_clusters, false);
    state.distances.assign(tile_count, unknown_distance);
    state.parent_directions.assign(tile_count, 255);
    state.queue.reserve(tile_count);

    // Seed one multi-source BFS with all valid allied spawn tiles.
    for (const cambc::vec2 root : state.spawn_roots) {
        if (!cambc::map::is_in_bounds(root))
            continue;
        const std::size_t index = tile_index(root, state.width);
        if (state.blocked_tiles[index])
            continue;
        if (state.distances[index] != unknown_distance)
            continue;
        state.distances[index] = 0;
        state.queue.push_back(index);
    }

    state.initialized = true;
}

// Rank a bounded set of camps by squared straight-line distance when exact search is too costly.
void choose_straight_line_camps(CampAnalysis& state) {
    std::vector<std::pair<std::int32_t, std::size_t>> ranked;
    ranked.reserve(state.anchors.size());
    for (std::size_t index = 0; index < state.anchors.size(); ++index) {
        const cambc::vec2 camp = state.anchors[index].position;
        std::int32_t closest_squared_distance = std::numeric_limits<std::int32_t>::max();
        for (const cambc::vec2 root : state.spawn_roots) {
            const std::int32_t dx = camp.x - root.x;
            const std::int32_t dy = camp.y - root.y;
            closest_squared_distance = std::min(closest_squared_distance, dx * dx + dy * dy);
        }
        ranked.emplace_back(closest_squared_distance, index);
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    state.info.camps.clear();
    const std::size_t target_camps = std::min(approximate_camps_to_save, ranked.size());
    for (std::size_t i = 0; i < target_camps; ++i) {
        CampInfo camp;
        camp.members.push_back(state.anchors[ranked[i].second]);
        camp.strength = state.anchor_strength[ranked[i].second];
        camp.farm_score = farm_score_of(255 - 255 * static_cast<double>(i) / std::max<std::size_t>(target_camps, 1), camp);
        camp.estimatedRoute = true;
        state.info.camps.push_back(std::move(camp));
    }
    state.info.approximate = true;
    state.info.search_complete = true;
    state.complete = true;
}

// Follow BFS predecessor links back to a spawn, then reverse and encode the route forward.
bool make_ally_route(const CampAnalysis& state, cambc::vec2 target, AllyRoute& route) {
    route = {};
    std::size_t current_index = tile_index(target, state.width);
    if (state.distances[current_index] == unknown_distance)
        return false;

    std::vector<std::uint8_t> reverse_directions;
    while (state.distances[current_index] != 0) {
        const std::uint8_t direction = state.parent_directions[current_index];
        if (direction >= route_directions.size() || reverse_directions.size() >= unknown_distance - 1)
            return false;
        reverse_directions.push_back(direction);
        const cambc::vec2 current{static_cast<int>(current_index % state.width),
                                  static_cast<int>(current_index / state.width)};
        const cambc::vec2 parent = current + route_directions[direction];
        current_index = tile_index(parent, state.width);
    }

    route.start = {static_cast<int>(current_index % state.width), static_cast<int>(current_index / state.width)};
    route.directions.reserve(reverse_directions.size());
    for (auto it = reverse_directions.rbegin(); it != reverse_directions.rend(); ++it)
        route.directions.push_back(static_cast<std::uint8_t>((*it + 4) % route_directions.size()));
    route.available = true;
    return true;
}

// Continue the multi-source BFS until it finds every camp or approaches the fuel reserve.
void advance_camp_analysis(std::uint64_t fuel_reserve) {
    CampAnalysis& state = camp_analysis;
    if (state.complete)
        return;
    if (!state.initialized) {
        if (cambc::fuel_remaining() <= fuel_reserve + 15000)
            return;
        initialize_camp_analysis();
    }

    while (state.queue_head < state.queue.size()) {
        if (cambc::fuel_remaining() <= fuel_reserve + 8000)
            break;

        const std::size_t current_index = state.queue[state.queue_head++];
        const cambc::vec2 current{static_cast<int>(current_index % state.width),
                                  static_cast<int>(current_index / state.width)};
        // BFS visits tiles in nondecreasing path distance, so camps are recorded nearest first.
        const int camp_index = state.camp_at_tile[current_index];
        if (camp_index >= 0 && !state.found[static_cast<std::size_t>(camp_index)]) {
            state.found[static_cast<std::size_t>(camp_index)] = true;
            CampInfo camp;
            camp.members.push_back(state.anchors[static_cast<std::size_t>(camp_index)]);
            camp.strength = state.anchor_strength[static_cast<std::size_t>(camp_index)];
            camp.our_distance = state.distances[current_index];
            if (state.info.camps.size() < approximate_camps_to_save ||
                cambc::fuel_remaining() > fuel_reserve + 20000)
                make_ally_route(state, current, camp.ally_route);
            state.info.camps.push_back(std::move(camp));
            if (state.info.camps.size() == state.anchors.size()) {
                state.complete = true;
                break;
            }
        }

        // Visit each traversable neighbor once and remember how to step back toward its parent.
        const std::uint16_t next_distance = static_cast<std::uint16_t>(state.distances[current_index] + 1);
        for (std::size_t direction = 0; direction < route_directions.size(); ++direction) {
            const cambc::vec2 next = current + route_directions[direction];
            if (next.x < 0 || next.y < 0 || next.x >= state.width || next.y >= state.height)
                continue;
            const std::size_t next_index = tile_index(next, state.width);
            if (state.blocked_tiles[next_index])
                continue;
            if (state.distances[next_index] != unknown_distance)
                continue;
            state.distances[next_index] = next_distance;
            state.parent_directions[next_index] = static_cast<std::uint8_t>((direction + 4) % route_directions.size());
            state.queue.push_back(next_index);
        }
    }

    if (state.queue_head == state.queue.size())
        state.complete = true;

    state.info.search_complete = state.complete;
    // Scale farming priority against the nearest camp; the nearest receives the maximum score.
    const int nearest_distance = state.info.camps.empty() ? 1 : std::max(state.info.camps.front().our_distance, 1);
    for (CampInfo& camp : state.info.camps) {
        camp.farm_score = farm_score_of(255.0 * nearest_distance / std::max(camp.our_distance, nearest_distance), camp);
    }
}

// Find the closest reachable tile in submission range of each altar and save its route.
void refresh_altar_distances() {
    CampAnalysis& state = camp_analysis;
    const int altar_range = cambc::constants::submit_gems_range;
    for (AltarInfo& altar : state.info.altars) {
        for (int y = std::max(altar.position.y - altar_range, 0);
             y <= std::min(altar.position.y + altar_range, state.height - 1); ++y) {
            for (int x = std::max(altar.position.x - altar_range, 0);
                 x <= std::min(altar.position.x + altar_range, state.width - 1); ++x) {
                const cambc::vec2 stand{x, y};
                const std::size_t index = tile_index(stand, state.width);
                if (state.blocked_tiles[index])
                    continue;
                if (state.distances[index] != unknown_distance &&
                    (altar.our_distance < 0 || state.distances[index] < altar.our_distance)) {
                    altar.our_distance = state.distances[index];
                    make_ally_route(state, stand, altar.ally_route);
                }
            }
        }
    }
}

// Serialize versioned draft data, prioritizing camp records over altar records within the byte limit.
std::vector<std::uint8_t> encode_draft_info(const DraftInfo& info) {
    // Header: format version, map width/height, camp/altar counts, and approximate-ranking flag.
    // Version 7 adds the estimated-route flag to each camp record. MERGED: version 8 adds a signed score byte (always -1) to each altar record; version 9 adds the cluster strength byte to each camp record (after combat_score).
    std::vector<std::uint8_t> memory{
        9, static_cast<std::uint8_t>(cambc::map::width()), static_cast<std::uint8_t>(cambc::map::height()), 0, 0,
        static_cast<std::uint8_t>(info.approximate)};
    std::size_t camps_written = 0;
    std::size_t altars_written = 0;

    // A smallest camp record has one member, distance and scores, estimate flag, and route header.
    constexpr std::size_t minimum_camp_record_size = 2 + 3 + 4 + 1 + 1 + 4;  // MERGED: +1 for the strength byte
    for (std::size_t i = 0; i < info.camps.size() && camps_written < 255; ++i) {
        const CampInfo& camp = info.camps[i];
        if (camp.members.size() > 65535)
            continue;

        // Drop the route payload first if needed to preserve the camp's location and scores.
        AllyRoute route = camp.ally_route;
        const std::size_t route_size = route_record_size(route);
        const std::size_t camp_record_size = 2 + 3 * camp.members.size() + 4 + 1 + 1 + route_size;
        if (memory.size() + camp_record_size > cambc::constants::draft_memory_bytes &&
            route.available)
            route = {};

        const std::size_t record_size = 2 + 3 * camp.members.size() + 4 + 1 + 1 + route_record_size(route);
        if (memory.size() + record_size > cambc::constants::draft_memory_bytes)
            break;

        // Camp record: member count and members, allied distance, scores, estimate flag, route.
        append_u16(memory, static_cast<std::uint16_t>(camp.members.size()));
        for (const CampMember& member : camp.members) {
            memory.push_back(static_cast<std::uint8_t>(member.type));
            memory.push_back(static_cast<std::uint8_t>(member.position.x));
            memory.push_back(static_cast<std::uint8_t>(member.position.y));
        }
        append_distance(memory, camp.our_distance);
        memory.push_back(camp.farm_score);
        memory.push_back(camp.combat_score);
        memory.push_back(camp.strength);  // MERGED: version 9
        memory.push_back(static_cast<std::uint8_t>(camp.estimatedRoute));
        append_route(memory, route);
        ++camps_written;
    }

    // Altar record: position, score (-1: not a target), ally/enemy distances, and allied route.
    for (const AltarInfo& altar : info.altars) {
        if (altars_written == 255)
            break;
        const std::size_t record_size = 7 + route_record_size(altar.ally_route);
        if (memory.size() + record_size > cambc::constants::draft_memory_bytes)
            break;

        memory.push_back(static_cast<std::uint8_t>(altar.position.x));
        memory.push_back(static_cast<std::uint8_t>(altar.position.y));
        memory.push_back(static_cast<std::uint8_t>(altar.score));  // -1 (0xFF)
        append_distance(memory, altar.our_distance);
        append_distance(memory, altar.enemy_distance);
        append_route(memory, altar.ally_route);
        ++altars_written;
    }

    // Fill in counts after records have been limited by available draft memory.
    memory[3] = static_cast<std::uint8_t>(camps_written);
    memory[4] = static_cast<std::uint8_t>(altars_written);
    return memory;
}

// Print analysis progress, saved-record counts, memory use, and fuel use to the replay log.
void print_diagnostics(const DraftInfo& info, const std::vector<std::uint8_t>& memory,
                       std::uint64_t fuel_before, std::uint64_t fuel_after) {
    const std::uint64_t fuel_used = fuel_after <= fuel_before ? fuel_before - fuel_after : 0;
    const bool fuel_measurement_valid = fuel_after <= fuel_before;
    const std::size_t saved_camps = memory.size() > 3 ? memory[3] : 0;
    const std::size_t saved_altars = memory.size() > 4 ? memory[4] : 0;

    std::printf("draft: map %dx%d, ranking %s, nearest camps %zu/%zu saved, search %s, altars %zu/%zu saved, "
                "adjacent camps ignored %zu, memory %zu/%u bytes, ",
                cambc::map::width(), cambc::map::height(), info.approximate ? "straight-line" : "path-distance",
                saved_camps,
                (info.approximate ? std::min(approximate_camps_to_save, info.camp_clusters)
                                  : info.camp_clusters),
                info.search_complete ? "complete" : "continuing",
                saved_altars, info.altars.size(), info.adjacent_camps_ignored, memory.size(),
                static_cast<unsigned>(cambc::constants::draft_memory_bytes));
    if (fuel_measurement_valid) {
        std::printf("analysis fuel %llu, fuel remaining %llu\n", static_cast<unsigned long long>(fuel_used),
                    static_cast<unsigned long long>(fuel_after));
    } else {
        std::printf("analysis fuel unknown (fuel counter reset), fuel remaining %llu\n",
                    static_cast<unsigned long long>(fuel_after));
    }

    std::fflush(stdout);
}

// MERGED: map symmetry from the two spawn regions, and combat scores from it.
//
// The transform that maps our spawn tiles exactly onto the enemy's is the map's symmetry (flip x, flip y, 180-degree rotation, and on square
// maps the two diagonal flips). A camp near the ENEMY spawn mirrors to a camp near OURS, which has a high farm score, so a camp's combat score is
// the farm score of its mirror-image camp minus 1 (0 if the mirror camp is not among the camps saved so far).
enum Symmetry { kSymUnknown = -2, kSymNone = -1, kFlipX = 0, kFlipY, kRotate, kTranspose, kAntiTranspose, kSymCount };
int g_symmetry = kSymUnknown;  // found once, then reused every draft turn
const char* const symmetry_names[kSymCount] = {"flip-x", "flip-y", "rotate-180", "transpose", "anti-transpose"};

cambc::vec2 mirror_tile(int symmetry, cambc::vec2 p, int width, int height) {
    switch (symmetry) {
    case kFlipX: return {width - 1 - p.x, p.y};
    case kFlipY: return {p.x, height - 1 - p.y};
    case kRotate: return {width - 1 - p.x, height - 1 - p.y};
    case kTranspose: return {p.y, p.x};
    default: return {height - 1 - p.y, width - 1 - p.x};  // anti-transpose
    }
}

bool in_map(cambc::vec2 p, int width, int height) { return p.x >= 0 && p.y >= 0 && p.x < width && p.y < height; }

// Which transform maps our spawn tiles onto the enemy's. If several do (a centred spawn), the one that maps the most camp anchors onto camp anchors wins.
int detect_symmetry(const CampAnalysis& state) {
    const int width = state.width, height = state.height;
    const auto ours = cambc::map::spawn_region(cambc::team::ally);
    const auto theirs = cambc::map::spawn_region(cambc::team::enemy);
    const auto our_tiles = ours.positions();
    if (our_tiles.empty() || our_tiles.size() != theirs.count())
        return kSymNone;
    std::vector<std::uint8_t> camp_here(static_cast<std::size_t>(width) * height, 0);
    for (const CampMember& anchor : state.anchors)
        camp_here[tile_index(anchor.position, width)] = 1;
    int best = kSymNone, best_matches = -1;
    for (int symmetry = 0; symmetry < kSymCount; ++symmetry) {
        if (symmetry >= kTranspose && width != height)
            continue;
        bool spawns_match = true;
        for (const cambc::vec2 tile : our_tiles) {
            const cambc::vec2 m = mirror_tile(symmetry, tile, width, height);
            if (!in_map(m, width, height) || !theirs.is_set(m)) {
                spawns_match = false;
                break;
            }
        }
        if (!spawns_match)
            continue;
        int matches = 0;
        for (const CampMember& anchor : state.anchors) {
            const cambc::vec2 m = mirror_tile(symmetry, anchor.position, width, height);
            matches += in_map(m, width, height) && camp_here[tile_index(m, width)];
        }
        if (matches > best_matches) {
            best_matches = matches;
            best = symmetry;
        }
    }
    return best;
}

// Set every saved camp's combat score from the farm score of its mirror camp (see above). Returns how many camps got a non-zero score.
std::size_t assign_combat_scores(CampAnalysis& state) {
    if (g_symmetry == kSymUnknown)
        g_symmetry = detect_symmetry(state);
    if (g_symmetry == kSymNone)
        return 0;
    const int width = state.width, height = state.height;
    std::vector<int> camp_at(static_cast<std::size_t>(width) * height, -1);
    for (std::size_t i = 0; i < state.info.camps.size(); ++i)
        if (!state.info.camps[i].members.empty())
            camp_at[tile_index(state.info.camps[i].members.front().position, width)] = static_cast<int>(i);
    std::size_t scored = 0;
    for (CampInfo& camp : state.info.camps) {
        camp.combat_score = 0;
        if (camp.members.empty())
            continue;
        const cambc::vec2 m = mirror_tile(g_symmetry, camp.members.front().position, width, height);
        if (!in_map(m, width, height))
            continue;
        const int other = camp_at[tile_index(m, width)];
        if (other < 0)
            continue;
        const int farm = state.info.camps[static_cast<std::size_t>(other)].farm_score;
        camp.combat_score = static_cast<std::uint8_t>(std::max(0, farm - 1));
        scored += camp.combat_score > 0;
    }
    return scored;
}

// Prepared one turn before it can be submitted as draft memory.
std::vector<std::uint8_t> pending_memory;

}  // namespace

void play_draft() {
    // Memory prepared on the previous draft turn must be submitted before this turn's pick.
    if (!pending_memory.empty()) {
        try {
            cambc::draft_memory = pending_memory;
        } catch (const cambc::error& error) {
            std::printf("draft: draft memory submission failed (%d): %s\n",
                        static_cast<int>(error.code()), error.message().c_str());
        }
        pending_memory.clear();
    }

    // Follow the configured pick order; the game stops requesting picks when the team is full.
    // Fixed champion selection order used for this team's draft.
    constexpr std::array picks{cambc::champion_draft::cassi_and_issac, cambc::champion_draft::stellar,
                               cambc::champion_draft::tavi, cambc::champion_draft::ryth, cambc::champion_draft::ren};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    const std::uint32_t team_size = cambc::draft::team_size();
    const bool has_pick = chosen.size() < picks.size();
    const bool final_pick = has_pick && chosen.size() + 1 >= team_size;

    if (has_pick)
        cambc::draft::pick(picks[chosen.size()]);

    // Analysis after the final pick cannot be submitted, so finish preparing memory on the preceding pick.
    if (!has_pick || final_pick)
        return;

    // Spend available draft fuel on analysis, then stage the result for the next draft turn.
    const std::uint64_t fuel_before = cambc::fuel_remaining();
    advance_camp_analysis(normal_fuel_reserve);
    const bool next_pick_is_final = chosen.size() + 2 >= team_size;
    if (next_pick_is_final && camp_analysis.initialized && !camp_analysis.complete)
        choose_straight_line_camps(camp_analysis);
    if (camp_analysis.initialized) {
        if (camp_analysis.complete && cambc::fuel_remaining() > normal_fuel_reserve + 30000)
            refresh_altar_distances();
        // MERGED: combat scores from the map's symmetry, only when the budget has room (about 20k fuel on a 42-camp map).
        if (cambc::fuel_remaining() > normal_fuel_reserve + 20000) {
            const std::size_t scored = assign_combat_scores(camp_analysis);
            std::printf("draft: symmetry %s, combat scores %zu/%zu camps\n",
                        g_symmetry >= 0 ? symmetry_names[g_symmetry] : "none", scored, camp_analysis.info.camps.size());
        } else {
            std::printf("draft: no fuel room for symmetry/combat scores this turn\n");
        }
        pending_memory = encode_draft_info(camp_analysis.info);
    }
    const std::uint64_t fuel_after = cambc::fuel_remaining();
    if (camp_analysis.initialized)
        print_diagnostics(camp_analysis.info, pending_memory, fuel_before, fuel_after);
}
