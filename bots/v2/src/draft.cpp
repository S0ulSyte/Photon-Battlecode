// draft.cpp - v2's draft: picks the squad, and analyses the map once for the whole team.
//
// Reads: the map (walls, holes, altars, spawn regions, camp groups) through the SDK, the current draft. Writes: the draft pick, draft memory (the plan).
// Edit here to change: who is picked (the `picks` list in play_draft), what the analysis hands to plan.cpp (analyse_map).
//
// Picks, in order: Cassi & Issac (one pick, two bodies), Vendetta, then the highest-health melee champions left: Ryth (1200), Marcella
// (1200), Varg (1000). The draft stops calling us once our team is full (3 to 5 picks depending on the map).
//
// Analysis (first turn only, BEFORE this turn's pick: the game refuses a draft-memory write after the pick): the engine's own
// distance_field floods the map from our spawn and from the enemy spawn (host speed, so it costs almost no fuel); plan.cpp turns the
// two distance arrays into scores, every champion's two target camps, and the routes; the result is written to draft memory.
#include <cambc.h>
#include "draft.h"
#include <array>
#include <cstdio>
#include <vector>
#include "log.h"
#include "plan.h"

namespace {

// What: the one-time analysis of the map. Inputs: none (reads the map through the SDK). Result: the plan bytes for draft memory.
// Side effects: a DRAFT log line. Fuel: ~195,000 once, on the first draft turn (it has a 10x budget).
std::vector<std::uint8_t> analyse_map() {
    const std::uint64_t fuel_before = cambc::fuel_remaining();
    const int w = cambc::map::width(), h = cambc::map::height();
    // Nobody can stand on walls, holes or altars; camp tiles are ordinary floor once a camp is cleared.
    const cambc::bitboard solid = cambc::map::walls() | cambc::map::holes() | cambc::map::altars();
    cambc::bitboard visited;
    cambc::distance_field ours, theirs;
    ours.compute(solid, cambc::map::spawn_region(cambc::team::ally), visited);
    theirs.compute(solid, cambc::map::spawn_region(cambc::team::enemy), visited);

    PlanInput in;
    in.width = w;
    in.height = h;
    in.reach = cambc::constants::submit_gems_range;
    in.our_dist = ours.export_linear_buffer();
    in.enemy_dist = theirs.export_linear_buffer();
    for (const auto& group : cambc::map::camp_groups())
        for (const auto& camp : group.camps())
            // (change 10: the group's reward rides along; every camp of a group carries the whole group's reward)
            in.camps.push_back({static_cast<int>(camp.kind()), camp.spawn_position().x, camp.spawn_position().y, static_cast<int>(group.gem_value()),
                                static_cast<int>(group.gold_value())});
    for (cambc::vec2 a : cambc::map::altars().positions())
        in.altars.push_back({a.x, a.y});

    const Plan plan = build_plan(in);
    const auto bytes = encode_plan(plan, cambc::constants::draft_memory_bytes);

    LogLine log;
    log.s("DRAFT plan map ").n(w).s("x").n(h).s(" camps ").n(static_cast<long long>(in.camps.size())).s(" altars ").n(static_cast<long long>(plan.altars.size()))
        .s(" targets ").n(static_cast<long long>(plan.targets.size())).s(" bytes ").n(static_cast<long long>(bytes.size())).s("/")
        .n(cambc::constants::draft_memory_bytes).s(" fuel ").n(static_cast<long long>(fuel_before - cambc::fuel_remaining()));
    log.flush();
    return bytes;
}

}  // namespace

// What: one draft turn: on the first, analyse the map and write the plan into draft memory BEFORE picking (a write after the pick gives engine error 18); then make the next pick.
// Inputs: none. Result: none. Side effects: draft memory, one draft pick.
void play_draft() {
    constexpr std::array picks{cambc::champion_draft::cassi_and_issac, cambc::champion_draft::vendetta, cambc::champion_draft::ryth,
                               cambc::champion_draft::marcella, cambc::champion_draft::varg};

    static bool analysed = false;
    if (!analysed) {
        analysed = true;
        try {
            cambc::draft_memory = analyse_map();
        } catch (const std::exception& error) {
            // Never let a failed analysis cost us the pick below.
            LogLine log;
            log.s("DRAFT analysis failed: ").s(error.what());
            log.flush();
        }
    }

    const auto taken = cambc::draft::current_draft(cambc::team::ally).size();
    if (taken < picks.size())
        cambc::draft::pick(picks[taken]);
}
