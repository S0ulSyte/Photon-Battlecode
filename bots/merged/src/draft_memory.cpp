// draft_memory.cpp - see draft_memory.h.
//
// Reads: starter-cpp draft memory (layout in starter's draft.cpp encode_draft_info), the SDK's compass vectors and ally spawn region.
// Writes: a Plan. Edit here to change: how starter camp scores become targets, the fallback route.
#include "draft_memory.h"
#include <cambc.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include "target_select.h"

namespace {

constexpr std::uint16_t kNoRoute = 65535;

// v2's direction table (plan.cpp kDeltas): code -> (dx, dy).
constexpr std::array<std::array<int, 2>, 8> kV2Deltas{{{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}}};

// Starter compass code (0 N, 1 NE, 2 E, 3 SE, 4 S, 5 SW, 6 W, 7 NW) -> v2 code, via the SDK vectors. -1 if it matches nothing (cannot happen).
int to_v2_code(int starter_code) {
    const std::array<cambc::vec2, 8> compass{cambc::vec2::north, cambc::vec2::north_east, cambc::vec2::east,      cambc::vec2::south_east,
                                             cambc::vec2::south, cambc::vec2::south_west, cambc::vec2::west,      cambc::vec2::north_west};
    const cambc::vec2 d = compass[static_cast<std::size_t>(starter_code & 7)];
    for (int k = 0; k < 8; ++k)
        if (kV2Deltas[static_cast<std::size_t>(k)][0] == d.x && kV2Deltas[static_cast<std::size_t>(k)][1] == d.y)
            return k;
    return -1;
}

struct Reader {
    const std::uint8_t* d;
    std::size_t size, pos = 0;
    bool ok = true;
    std::uint8_t u8() {
        if (pos + 1 > size) {
            ok = false;
            return 0;
        }
        return d[pos++];
    }
    std::uint16_t u16() {
        const std::uint16_t lo = u8();
        return static_cast<std::uint16_t>(lo | (u8() << 8));
    }
};

// A route as stored by starter's draft. available == false: the draft could not store it.
struct StarterRoute {
    int start_x = 0, start_y = 0;
    std::vector<std::uint8_t> steps;  // already translated to v2 codes
    bool available = false;
};

StarterRoute read_route(Reader& r) {
    StarterRoute route;
    const int x = r.u8(), y = r.u8();
    const std::uint16_t n = r.u16();
    if (!r.ok || n == kNoRoute)
        return route;
    route.start_x = x;
    route.start_y = y;
    for (std::uint16_t i = 0; i < n && r.ok; i += 2) {
        const std::uint8_t packed = r.u8();
        route.steps.push_back(static_cast<std::uint8_t>(to_v2_code(packed & 15)));
        if (i + 1 < n)
            route.steps.push_back(static_cast<std::uint8_t>(to_v2_code(packed >> 4)));
    }
    route.available = r.ok && std::none_of(route.steps.begin(), route.steps.end(), [](std::uint8_t c) { return c > 7; });
    return route;
}

// Our spawn tile closest (Chebyshev) to (x, y).
cambc::vec2 nearest_spawn(int x, int y) {
    cambc::vec2 best{x, y};
    int best_d = 1 << 30;
    for (cambc::vec2 s : cambc::map::spawn_region(cambc::team::ally).positions()) {
        const int d = std::max(std::abs(s.x - x), std::abs(s.y - y));
        if (d < best_d) {
            best_d = d;
            best = s;
        }
    }
    return best;
}

// A route found now with the engine's distance field when the draft stored none (big maps: the draft's BFS runs out of fuel and falls back to a
// straight-line camp list with no routes). Walls, holes, altars and the enemy spawn are solid, so a route never crosses an altar. Goal: the camp's
// tile, or for an altar every free tile within tribute range of it. The route starts on the ally spawn tile with the shortest way in. Empty if unreachable.
PlanPath field_path(int tx, int ty, bool altar) {
    PlanPath p;
    const cambc::bitboard solid = cambc::map::walls() | cambc::map::holes() | cambc::map::altars() | cambc::map::spawn_region(cambc::team::enemy);
    cambc::bitboard roots;
    roots.clear();
    if (altar) {
        roots.draw_chebyshev(cambc::vec2{tx, ty}, cambc::constants::submit_gems_range);
        roots &= ~solid;
    } else {
        roots.set(cambc::vec2{tx, ty}, true);
    }
    cambc::bitboard visited;
    cambc::distance_field field;
    field.compute(solid, roots, visited);
    cambc::vec2 best{-1, -1};
    int best_d = 1 << 30;
    for (cambc::vec2 s : cambc::map::spawn_region(cambc::team::ally).positions()) {
        const int d = field.get(s);
        if (d != cambc::distance_field::unreachable && d < best_d) {
            best_d = d;
            best = s;
        }
    }
    if (best.x < 0)
        return p;
    p.start_x = best.x;
    p.start_y = best.y;
    cambc::vec2 cur = best;
    for (int guard = 0; guard < 1024; ++guard) {
        const auto step = field.next_move_from(cur);
        if (!step)
            break;
        for (int k = 0; k < 8; ++k)
            if (kV2Deltas[static_cast<std::size_t>(k)][0] == step->x && kV2Deltas[static_cast<std::size_t>(k)][1] == step->y) {
                p.steps.push_back(static_cast<std::uint8_t>(k));
                break;
            }
        cur = cur + *step;
    }
    return p;
}

// The plan path for a stored route; if the draft stored none and `compute` is set (this champion's own targets and the nearest altar), a terrain-aware
// route from field_path, or a straight line as the last resort. Routes nobody here will walk stay empty (just a start tile) to save fuel.
PlanPath make_path(const StarterRoute& route, int tx, int ty, bool altar, bool compute) {
    PlanPath p;
    if (route.available) {
        p.start_x = route.start_x;
        p.start_y = route.start_y;
        p.steps = route.steps;
        return p;
    }
    if (!compute) {
        p.start_x = tx;
        p.start_y = ty;
        return p;
    }
    p = field_path(tx, ty, altar);
    if (!p.steps.empty() || (p.start_x == tx && p.start_y == ty))
        return p;
    const cambc::vec2 sp = nearest_spawn(tx, ty);
    p = straight_path(sp.x, sp.y, tx, ty);
    if (altar && !p.steps.empty())
        p.steps.pop_back();
    return p;
}

struct Camp {
    int kind = 0, x = 0, y = 0, farm = 0, combat = 0, strength = 1;
    StarterRoute route;
};
struct Altar {
    int x = 0, y = 0, our = -1;
    StarterRoute route;
};

}  // namespace

bool decode_starter_memory(const std::uint8_t* data, std::size_t size, Plan& out, int my_slot) {
    out = Plan{};
    if (size < 6 || data[0] != 9)
        return false;
    Reader r{data, size};
    r.pos = 1;
    const int width = r.u8(), height = r.u8();
    const int camp_count = r.u8(), altar_count = r.u8();
    r.u8();  // approximate-ranking flag: not needed

    std::vector<Camp> camps;
    for (int i = 0; i < camp_count && r.ok; ++i) {
        Camp c;
        const std::uint16_t members = r.u16();
        for (std::uint16_t m = 0; m < members && r.ok; ++m) {
            const int kind = r.u8(), x = r.u8(), y = r.u8();
            if (m == 0) {  // the anchor: the camp the draft chose for the cluster
                c.kind = kind;
                c.x = x;
                c.y = y;
            }
        }
        r.u16();  // our_distance
        c.farm = r.u8();
        c.combat = r.u8();
        c.strength = r.u8();  // version 9: the cluster strength = its number of champion slots
        r.u8();  // estimatedRoute flag
        c.route = read_route(r);
        if (r.ok && members > 0)
            camps.push_back(std::move(c));
    }
    std::vector<Altar> altars;
    for (int i = 0; i < altar_count && r.ok; ++i) {
        Altar a;
        a.x = r.u8();
        a.y = r.u8();
        r.u8();  // score: always -1 (altars are never targets); they are only ever used by the BANK state, so nothing here reads it
        const std::uint16_t our = r.u16();
        r.u16();  // enemy_distance
        a.our = our == kNoRoute ? -1 : our;
        a.route = read_route(r);
        if (r.ok)
            altars.push_back(std::move(a));
    }
    if (!r.ok && camps.empty() && altars.empty())
        return false;  // a truncated tail is tolerated: records up to the cut are kept

    out.width = width;
    out.height = height;

    // Targets: v2's own slot rules over the DRAFT's own farm and combat scores, unchanged.
    std::vector<TargetCandidate> candidates;
    for (std::size_t i = 0; i < camps.size(); ++i)
        candidates.push_back({static_cast<int>(i), static_cast<cambc::camp_type>(camps[i].kind), camps[i].farm, camps[i].combat, camps[i].strength});
    const auto all = select_all_targets(candidates);
    for (int slot = 0; slot < kPlanSlots; ++slot)
        for (const Target& t : all[static_cast<std::size_t>(slot)]) {
            const Camp& c = camps[static_cast<std::size_t>(t.id)];
            PlanTarget pt;
            pt.slot = slot;
            pt.attack = !t.farming;
            pt.kind = c.kind;
            pt.x = c.x;
            pt.y = c.y;
            pt.path = make_path(c.route, c.x, c.y, false, slot == my_slot);
            out.targets.push_back(std::move(pt));
        }

    // Altars: closest first; ones with no known route get a straight line that stops next to the altar.
    std::stable_sort(altars.begin(), altars.end(), [](const Altar& a, const Altar& b) {
        const int da = a.our < 0 ? 1 << 20 : a.our, db = b.our < 0 ? 1 << 20 : b.our;
        return da < db;
    });
    bool first_altar = true;
    for (const Altar& a : altars) {
        PlanAltar pa;
        pa.x = a.x;
        pa.y = a.y;
        pa.path = make_path(a.route, a.x, a.y, true, first_altar);  // only the nearest altar's route is ever walked (BANK)
        first_altar = false;
        out.altars.push_back(std::move(pa));
    }
    return true;
}
