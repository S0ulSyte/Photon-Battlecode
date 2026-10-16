#include <cambc.h>
#include "altar_info.h"
#include "camp_info.h"
#include "draft.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

using mack_draft::CampInfo;
using mack_draft::CampMember;
using mack_draft::AltarInfo;
using mack_draft::AllyRoute;

// Data prepared during drafting and later copied into draft memory for the champions to read.
struct DraftInfo {
    std::vector<CampInfo> camps;
    std::vector<AltarInfo> altars;
};

constexpr int max_score = 255;
// The SDK uses the largest uint16 value to represent a tile that was not reached.
constexpr std::uint16_t unknown_distance = cambc::distance_field::unreachable;
// Direction indices are part of the route byte format; keep this order stable.
constexpr std::array<cambc::vec2, 8> route_directions{
    cambc::vec2::north,      cambc::vec2::north_east, cambc::vec2::east,      cambc::vec2::south_east,
    cambc::vec2::south,      cambc::vec2::south_west, cambc::vec2::west,      cambc::vec2::north_west};

// Scale distance against the nearest reachable camp: nearest scores 255, twice as far scores about half.
std::uint8_t score_for(int distance, int nearest_distance) {
    if (distance < 0)
        return 0;
    return static_cast<std::uint8_t>(
        max_score * nearest_distance / std::max(distance, nearest_distance));
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

std::size_t route_record_size(const AllyRoute& route) {
    if (!route.available || route.directions.size() >= unknown_distance)
        // Unavailable route: x, y, and the reserved 16-bit route-length marker.
        return 4;
    // The route header is four bytes; two 4-bit direction codes share each following byte.
    return 4 + (route.directions.size() + 1) / 2;
}

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

// Collect camp groups and altars, then calculate the shortest relevant distances and camp scores.
DraftInfo collect_draft_info() {
    DraftInfo info;
    // The map API already returns connected monsters grouped as one camp.
    for (const auto& group : cambc::map::camp_groups()) {
        CampInfo camp_info;
        for (const auto& camp : group.camps()) {
            camp_info.members.push_back({camp.kind(), camp.spawn_position()});
        }
        if (!camp_info.members.empty())
            info.camps.push_back(std::move(camp_info));
    }

    // Keep each altar as a separate objective.
    for (const cambc::vec2 position : cambc::map::altars().positions())
        info.altars.push_back({position});

    // Walls, holes, and altar tiles themselves cannot be used as walking destinations.
    const auto blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();

    // Search to camp tiles and to every legal tile from which an altar can be used.
    cambc::bitboard destinations;
    for (const CampInfo& camp : info.camps)
        for (const CampMember& member : camp.members)
            destinations.set(member.position);

    const int altar_range = cambc::constants::submit_gems_range;
    // Every open tile in the altar's square range is a valid approach candidate.
    for (const AltarInfo& altar : info.altars) {
        for (int y = std::max(altar.position.y - altar_range, 0);
             y <= std::min(altar.position.y + altar_range, cambc::map::height() - 1); ++y) {
            for (int x = std::max(altar.position.x - altar_range, 0);
                 x <= std::min(altar.position.x + altar_range, cambc::map::width() - 1); ++x) {
                const cambc::vec2 stand{ x, y };
                if (!blocked.is_set(stand))
                    destinations.set(stand);
            }
        }
    }

    // compute_to stops after reaching the requested camp and altar-approach tiles, if possible.
    cambc::distance_field our_distances;
    cambc::distance_field enemy_distances;
    cambc::bitboard visited;
    // Use separate fields for the two teams because their spawn regions differ.
    our_distances.compute_to(blocked, cambc::map::spawn_region(cambc::team::ally), destinations, visited);
    visited.clear();
    enemy_distances.compute_to(blocked, cambc::map::spawn_region(cambc::team::enemy), destinations, visited);

    // A grouped camp's distance is the shortest path to any of its member tiles.
    // Retain the closest ally-reachable member as the camp route endpoint.
    for (CampInfo& camp : info.camps) {
        for (const CampMember& member : camp.members) {
            const auto our_distance = our_distances.get(member.position);
            if (our_distance != cambc::distance_field::unreachable &&
                (camp.our_distance < 0 || our_distance < camp.our_distance)) {
                camp.our_distance = our_distance;
                // Save only our team's route; enemy data is kept as a distance for scoring.
                make_ally_route(our_distances, member.position, camp.ally_route);
            }

            const auto enemy_distance = enemy_distances.get(member.position);
            if (enemy_distance != cambc::distance_field::unreachable &&
                (camp.enemy_distance < 0 || enemy_distance < camp.enemy_distance))
                camp.enemy_distance = enemy_distance;
        }
    }

    // An altar's distance is the shortest path to a walkable tile in gem-submission range.
    // Retain the closest reachable altar approach tile to build its ally route.
    for (AltarInfo& altar : info.altars) {
        for (int y = std::max(altar.position.y - altar_range, 0);
             y <= std::min(altar.position.y + altar_range, cambc::map::height() - 1); ++y) {
            for (int x = std::max(altar.position.x - altar_range, 0);
                 x <= std::min(altar.position.x + altar_range, cambc::map::width() - 1); ++x) {
                const cambc::vec2 stand{ x, y };
                if (blocked.is_set(stand))
                    continue;

                const auto our_distance = our_distances.get(stand);
                if (our_distance != cambc::distance_field::unreachable &&
                    (altar.our_distance < 0 || our_distance < altar.our_distance)) {
                    altar.our_distance = our_distance;
                    // Route ends where the champion can submit gems, not on the blocked altar tile.
                    make_ally_route(our_distances, stand, altar.ally_route);
                }

                const auto enemy_distance = enemy_distances.get(stand);
                if (enemy_distance != cambc::distance_field::unreachable &&
                    (altar.enemy_distance < 0 || enemy_distance < altar.enemy_distance))
                    altar.enemy_distance = enemy_distance;
            }
        }
    }

    // Use the closest reachable group as the baseline for each team's score scale.
    int nearest_ours = std::numeric_limits<int>::max();
    int nearest_enemy = std::numeric_limits<int>::max();
    for (const CampInfo& camp : info.camps) {
        if (camp.our_distance >= 0)
            nearest_ours = std::min(nearest_ours, camp.our_distance);
        if (camp.enemy_distance >= 0)
            nearest_enemy = std::min(nearest_enemy, camp.enemy_distance);
    }
    nearest_ours = std::max(nearest_ours, 1);
    nearest_enemy = std::max(nearest_enemy, 1);

    for (CampInfo& camp : info.camps) {
        camp.farm_score = score_for(camp.our_distance, nearest_ours);
        camp.combat_score = score_for(camp.enemy_distance, nearest_enemy);
    }
    std::stable_sort(info.camps.begin(), info.camps.end(),
                     [](const CampInfo& a, const CampInfo& b) { return a.farm_score > b.farm_score; });
    return info;
}

// Serialize the sorted camp records and altar records into the bounded draft-memory buffer.
std::vector<std::uint8_t> encode_draft_info(const DraftInfo& info) {
    // Header: version, map width, map height, camp-group count, altar count. Altar records come first, then camp groups.
    // Each record stores ally/enemy distances, then an ally-only route: start x/y, u16 step count, packed directions.
    // Unavailable distances and routes use 65535; route direction codes follow compass order from north.
    std::vector<std::uint8_t> memory{
        3, static_cast<std::uint8_t>(cambc::map::width()), static_cast<std::uint8_t>(cambc::map::height()), 0, 0};
    std::size_t camps_written = 0;
    std::size_t altars_written = 0;

    // Altar records are written first so camp data cannot crowd out these match objectives.
    for (const AltarInfo& altar : info.altars) {
        if (altars_written == 255)
            break;
        const std::size_t record_size = 6 + route_record_size(altar.ally_route);
        if (memory.size() + record_size > cambc::constants::draft_memory_bytes)
            break;

        memory.push_back(static_cast<std::uint8_t>(altar.position.x));
        memory.push_back(static_cast<std::uint8_t>(altar.position.y));
        append_distance(memory, altar.our_distance);
        append_distance(memory, altar.enemy_distance);
        append_route(memory, altar.ally_route);
        ++altars_written;
    }

    for (const CampInfo& camp : info.camps) {
        if (camp.members.size() > 65535 || camps_written == 255)
            continue;

        const std::size_t record_size = 2 + 3 * camp.members.size() + 6 + route_record_size(camp.ally_route);
        // Camps are sorted by farm score, so stop rather than partially writing a record.
        if (memory.size() + record_size > cambc::constants::draft_memory_bytes)
            continue;

        append_u16(memory, static_cast<std::uint16_t>(camp.members.size()));
        for (const CampMember& member : camp.members) {
            memory.push_back(static_cast<std::uint8_t>(member.type));
            memory.push_back(static_cast<std::uint8_t>(member.position.x));
            memory.push_back(static_cast<std::uint8_t>(member.position.y));
        }
        append_distance(memory, camp.our_distance);
        append_distance(memory, camp.enemy_distance);
        memory.push_back(camp.farm_score);
        memory.push_back(camp.combat_score);
        append_route(memory, camp.ally_route);
        ++camps_written;
    }

    memory[3] = static_cast<std::uint8_t>(camps_written);
    memory[4] = static_cast<std::uint8_t>(altars_written);
    return memory;
}

// Print the analysis summary and per-objective details to the replay log.
void print_diagnostics(const DraftInfo& info, const std::vector<std::uint8_t>& memory,
                       std::uint64_t fuel_before, std::uint64_t fuel_after) {
    const std::uint64_t fuel_used = fuel_after <= fuel_before ? fuel_before - fuel_after : 0;
    const bool fuel_measurement_valid = fuel_after <= fuel_before;
    const std::size_t saved_camps = memory.size() > 3 ? memory[3] : 0;
    const std::size_t saved_altars = memory.size() > 4 ? memory[4] : 0;

    std::printf("mack_draft: map %dx%d, camps %zu/%zu saved, altars %zu/%zu saved, "
                "memory %zu/%u bytes, ",
                cambc::map::width(), cambc::map::height(), saved_camps, info.camps.size(), saved_altars,
                info.altars.size(), memory.size(), static_cast<unsigned>(cambc::constants::draft_memory_bytes));
    if (fuel_measurement_valid) {
        std::printf("analysis fuel %llu, fuel remaining %llu\n", static_cast<unsigned long long>(fuel_used),
                    static_cast<unsigned long long>(fuel_after));
    } else {
        std::printf("analysis fuel unknown (fuel counter reset), fuel remaining %llu\n",
                    static_cast<unsigned long long>(fuel_after));
    }

    for (std::size_t i = 0; i < info.altars.size(); ++i) {
        const AltarInfo& altar = info.altars[i];
        std::printf("  altar[%zu] (%d,%d) ally distance=%d enemy distance=%d",
                    i, altar.position.x, altar.position.y, altar.our_distance, altar.enemy_distance);
        if (altar.ally_route.available)
            std::printf(" ally route start=(%d,%d) steps=%zu\n", altar.ally_route.start.x,
                        altar.ally_route.start.y, altar.ally_route.directions.size());
        else
            std::printf(" ally route unavailable\n");
    }

    for (std::size_t i = 0; i < info.camps.size(); ++i) {
        const CampInfo& camp = info.camps[i];
        std::printf("  camp[%zu] members=%zu ally distance=%d enemy distance=%d farm=%u combat=%u",
                    i, camp.members.size(), camp.our_distance, camp.enemy_distance,
                    static_cast<unsigned>(camp.farm_score), static_cast<unsigned>(camp.combat_score));
        if (camp.ally_route.available)
            std::printf(" ally route start=(%d,%d) steps=%zu\n", camp.ally_route.start.x,
                        camp.ally_route.start.y, camp.ally_route.directions.size());
        else
            std::printf(" ally route unavailable\n");

        for (const CampMember& member : camp.members)
            std::printf("    member %s at (%d,%d)\n", cambc::to_string(member.type).c_str(),
                        member.position.x, member.position.y);
    }
    std::fflush(stdout);
}

std::vector<std::uint8_t> pending_memory;

}  // namespace

void play_draft() {
    // Memory prepared on the previous draft turn must be submitted before this turn's pick.
    if (!pending_memory.empty()) {
        cambc::draft_memory = pending_memory;
        pending_memory.clear();
    }

    // Follow the configured pick order; the game stops requesting picks when the team is full.
    constexpr std::array picks{cambc::champion_draft::cassi_and_issac, cambc::champion_draft::vendetta,
                               cambc::champion_draft::ryth, cambc::champion_draft::marcella,
                               cambc::champion_draft::varg};
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    if (chosen.size() < picks.size())
        cambc::draft::pick(picks[chosen.size()]);

    // Analyze once, after making this turn's pick so the pick is safe if analysis runs out of fuel.
    static bool analysed = false;
    if (analysed)
        return;
    analysed = true;
    const std::uint64_t fuel_before = cambc::fuel_remaining();
    const DraftInfo info = collect_draft_info();
    pending_memory = encode_draft_info(info);
    const std::uint64_t fuel_after = cambc::fuel_remaining();
    print_diagnostics(info, pending_memory, fuel_before, fuel_after);
}
