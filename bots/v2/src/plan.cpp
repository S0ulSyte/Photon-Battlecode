// plan.cpp - see plan.h.
//
// Reads: the PlanInput (distance fields, camps, altars). Writes: the Plan, and the draft-memory bytes (encode_plan).
// Edit here to change: how camps are scored (score_candidates; V2_CHANGE_REWARD_SCORES), how routes are traced (trace_path), the byte format (encode_plan /
// decode_plan - change the version byte too). Which camp goes to which champion is decided in target_select.cpp.
#include "plan.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include "target_select.h"
#include "config.h"  // V2_CHANGE_REWARD_SCORES, V2_W_GOLD_PER_GEM (change 10)

namespace {

// Direction code -> (dx, dy). The code of the opposite direction is 7 - code.
constexpr std::array<std::array<int, 2>, 8> kDeltas{{{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}}};

// 255 for the nearest camp, falling off as 1 / distance; 0 for an unreachable camp (steps < 0).
std::uint8_t score_for(int steps, int nearest_steps) {
    if (steps < 0)
        return 0;
    return static_cast<std::uint8_t>(255 * nearest_steps / std::max(steps, nearest_steps));
}

// What: the walking distance of a tile from the spawn tiles in a distance field, or -1 if unreachable (or no field).
int steps_at(const std::vector<std::uint16_t>& dist, int width, int x, int y) {
    if (dist.empty())
        return -1;
    const std::uint16_t d = dist[static_cast<std::size_t>(y * width + x)];
    return d == kUnreachable ? -1 : d;
}

// What: append a route to draft-memory bytes: start x, y, step count, then the steps two per byte.
void put_path(std::vector<std::uint8_t>& out, const PlanPath& p) {
    out.push_back(static_cast<std::uint8_t>(p.start_x));
    out.push_back(static_cast<std::uint8_t>(p.start_y));
    out.push_back(static_cast<std::uint8_t>(p.steps.size()));
    for (std::size_t s = 0; s < p.steps.size(); s += 2) {
        std::uint8_t packed = p.steps[s];
        if (s + 1 < p.steps.size())
            packed = static_cast<std::uint8_t>(packed | (p.steps[s + 1] << 4));
        out.push_back(packed);
    }
}

// What: read a route back from draft-memory bytes (the inverse of put_path). Result: false if the bytes run out.
bool get_path(const std::uint8_t* d, std::size_t size, std::size_t& pos, PlanPath& p) {
    if (pos + 3 > size)
        return false;
    p.start_x = d[pos];
    p.start_y = d[pos + 1];
    const std::size_t n = d[pos + 2];
    pos += 3;
    const std::size_t bytes = (n + 1) / 2;
    if (pos + bytes > size)
        return false;
    p.steps.clear();
    for (std::size_t s = 0; s < n; ++s) {
        const std::uint8_t byte = d[pos + s / 2];
        p.steps.push_back(static_cast<std::uint8_t>((s % 2 == 0) ? (byte & 15) : (byte >> 4)));
    }
    pos += bytes;
    return true;
}

}  // namespace

// What: the route from a spawn tile to (end_x, end_y), following strictly decreasing distances of the field. See plan.h.
PlanPath trace_path(const std::vector<std::uint16_t>& dist, int width, int height, int end_x, int end_y) {
    PlanPath path;
    std::vector<std::uint8_t> backwards;
    int x = end_x, y = end_y;
    while (dist[static_cast<std::size_t>(y * width + x)] > 0) {
        const std::uint16_t here = dist[static_cast<std::size_t>(y * width + x)];
        bool moved = false;
        for (int k = 0; k < 8 && !moved; ++k) {
            const int nx = x + kDeltas[static_cast<std::size_t>(k)][0], ny = y + kDeltas[static_cast<std::size_t>(k)][1];
            if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                continue;
            if (dist[static_cast<std::size_t>(ny * width + nx)] == static_cast<std::uint16_t>(here - 1)) {
                // The step that walks FROM (nx,ny) TO (x,y) has the opposite direction of the step from (x,y) to (nx,ny).
                backwards.push_back(static_cast<std::uint8_t>(7 - k));
                x = nx;
                y = ny;
                moved = true;
            }
        }
        if (!moved)
            break;  // cannot happen on a consistent distance field; stop rather than loop forever
    }
    path.start_x = x;
    path.start_y = y;
    path.steps.assign(backwards.rbegin(), backwards.rend());
    return path;
}

// What: score every camp that can be reached on foot from our spawn: a farm score (CHANGE 10: reward per walking step, or just closeness) and a combat
// score (closeness to the ENEMY spawn). Inputs: the draft's view of the map. Result: one TargetCandidate per reachable camp (target_select.h).
static std::vector<TargetCandidate> score_candidates(const PlanInput& in) {
    // ---- Camps: distances from both spawns, then farm and combat scores ----
    struct Scored {
        int camp;       // index into in.camps
        int our_steps;  // steps from our spawn
        int enemy_steps;
    };
    std::vector<Scored> reachable;
    int nearest_our = 1 << 30, nearest_enemy = 1 << 30;
    for (std::size_t i = 0; i < in.camps.size(); ++i) {
        const int ours = steps_at(in.our_dist, in.width, in.camps[i].x, in.camps[i].y);
        if (ours < 0)
            continue;  // no route from our spawn: nobody can be sent there
        const int theirs = steps_at(in.enemy_dist, in.width, in.camps[i].x, in.camps[i].y);
        reachable.push_back({static_cast<int>(i), ours, theirs});
        nearest_our = std::min(nearest_our, ours);
        if (theirs >= 0)
            nearest_enemy = std::min(nearest_enemy, theirs);
    }
    nearest_our = std::max(nearest_our, 1);
    nearest_enemy = std::max(nearest_enemy, 1);

    // CHANGE 10. What: farm score = reward per walking step. Why: the distance-only score gave 4 of 8 targets to gold clusters, which pay no gems
    // (Ryth made one trip for 22 gems). How: value = gems + V2_W_GOLD_PER_GEM * gold; density = value / steps (at least 1 step); the best density
    // scores 255, the others scale down in proportion; a camp worth nothing (a monolith) scores 0. If no camp carries any reward data
    // (best density 0) the distance-only score is used, as before.
    std::vector<int> farm_scores(reachable.size(), -1);  // -1 = use the distance-only score
#if V2_CHANGE_REWARD_SCORES
    {
        std::vector<double> density(reachable.size(), 0.0);
        double best_density = 0.0;
        for (std::size_t r = 0; r < reachable.size(); ++r) {
            const PlanCamp& c = in.camps[static_cast<std::size_t>(reachable[r].camp)];
            const double value = c.gems + V2_W_GOLD_PER_GEM * c.gold;
            density[r] = value / std::max(reachable[r].our_steps, 1);
            best_density = std::max(best_density, density[r]);
        }
        if (best_density > 0.0)
            for (std::size_t r = 0; r < reachable.size(); ++r)
                // Round to the nearest whole score; a camp that pays something never rounds down to 0 (0 means "cannot be reached").
                farm_scores[r] = density[r] <= 0.0 ? 0 : std::max(1, static_cast<int>(255.0 * density[r] / best_density + 0.5));
    }
#endif

    std::vector<TargetCandidate> candidates;
    for (std::size_t r = 0; r < reachable.size(); ++r) {
        const Scored& s = reachable[r];
        candidates.push_back({s.camp, static_cast<cambc::camp_type>(in.camps[static_cast<std::size_t>(s.camp)].kind),
                              farm_scores[r] >= 0 ? farm_scores[r] : score_for(s.our_steps, nearest_our), score_for(s.enemy_steps, nearest_enemy)});
    }
    return candidates;
}

// What: hand every champion its two camps (target_select) and trace the route to each. Inputs: the input, the plan to fill, the scored camps.
// Result: none. Side effects: plan.targets.
static void add_targets(const PlanInput& in, Plan& plan, const std::vector<TargetCandidate>& candidates) {
    const auto all = select_all_targets(candidates);
    for (int slot = 0; slot < kPlanSlots; ++slot) {
        for (const Target& t : all[static_cast<std::size_t>(slot)]) {
            const PlanCamp& c = in.camps[static_cast<std::size_t>(t.id)];
            PlanTarget pt;
            pt.slot = slot;
            pt.attack = !t.farming;
            pt.kind = c.kind;
            pt.gems = c.gems;  // change 10: carried in the plan so a champion (and the logs) can see what its target pays
            pt.gold = c.gold;
            pt.x = c.x;
            pt.y = c.y;
            pt.path = trace_path(in.our_dist, in.width, in.height, c.x, c.y);
            plan.targets.push_back(std::move(pt));
        }
    }
}

// What: for each altar, the tile next to it that we reach soonest, and the route there; closest altar first. Inputs: the input, the plan to fill.
// Result: none. Side effects: plan.altars.
static void add_altars(const PlanInput& in, Plan& plan) {
    for (const auto& altar : in.altars) {
        int best_steps = -1, bx = 0, by = 0;
        for (int y = std::max(altar.second - in.reach, 0); y <= std::min(altar.second + in.reach, in.height - 1); ++y) {
            for (int x = std::max(altar.first - in.reach, 0); x <= std::min(altar.first + in.reach, in.width - 1); ++x) {
                const int d = steps_at(in.our_dist, in.width, x, y);
                if (d >= 0 && (best_steps < 0 || d < best_steps)) {
                    best_steps = d;
                    bx = x;
                    by = y;
                }
            }
        }
        if (best_steps < 0)
            continue;
        PlanAltar pa;
        pa.x = altar.first;
        pa.y = altar.second;
        pa.path = trace_path(in.our_dist, in.width, in.height, bx, by);
        plan.altars.push_back(std::move(pa));
    }
    std::stable_sort(plan.altars.begin(), plan.altars.end(),
                     [](const PlanAltar& a, const PlanAltar& b) { return a.path.steps.size() < b.path.steps.size(); });
}

// What: the whole draft analysis: score the camps, pick every champion's targets, find the altar routes. Inputs: the draft's view of the map.
// Result: the plan (encode_plan packs it into draft memory). Side effects: none. Fuel: ~100,000 once, on the first draft turn.
Plan build_plan(const PlanInput& in) {
    Plan plan;
    plan.width = in.width;
    plan.height = in.height;
    const std::vector<TargetCandidate> candidates = score_candidates(in);
    add_targets(in, plan, candidates);
    add_altars(in, plan);
    return plan;
}

// What: pack a Plan into at most `capacity` bytes (version 3). See plan.h for the layout.
std::vector<std::uint8_t> encode_plan(const Plan& plan, std::size_t capacity) {
    std::vector<std::uint8_t> out{3, static_cast<std::uint8_t>(plan.width), static_cast<std::uint8_t>(plan.height), 0, 0};
    std::size_t altars = 0, targets = 0;
    for (const PlanAltar& a : plan.altars) {
        std::vector<std::uint8_t> entry{static_cast<std::uint8_t>(a.x), static_cast<std::uint8_t>(a.y)};
        put_path(entry, a.path);
        if (out.size() + entry.size() > capacity || a.path.steps.size() > 255)
            break;
        out.insert(out.end(), entry.begin(), entry.end());
        ++altars;
    }
    for (const PlanTarget& t : plan.targets) {
        std::vector<std::uint8_t> entry{static_cast<std::uint8_t>(t.slot | (t.attack ? 0x80 : 0)), static_cast<std::uint8_t>(t.kind),
                                        static_cast<std::uint8_t>(std::min(t.gems, 255)), static_cast<std::uint8_t>(std::min(t.gold, 255)),
                                        static_cast<std::uint8_t>(t.x), static_cast<std::uint8_t>(t.y)};
        put_path(entry, t.path);
        if (out.size() + entry.size() > capacity || t.path.steps.size() > 255)
            break;
        out.insert(out.end(), entry.begin(), entry.end());
        ++targets;
    }
    out[3] = static_cast<std::uint8_t>(altars);
    out[4] = static_cast<std::uint8_t>(targets);
    return out;
}

// What: read a Plan back from draft memory; false if the bytes are not a valid version-3 plan. See plan.h.
bool decode_plan(const std::uint8_t* d, std::size_t size, Plan& out) {
    if (size < 5 || d[0] != 3)
        return false;
    out = Plan{};
    out.width = d[1];
    out.height = d[2];
    const std::size_t altars = d[3], targets = d[4];
    std::size_t pos = 5;
    for (std::size_t i = 0; i < altars; ++i) {
        if (pos + 2 > size)
            return false;
        PlanAltar a;
        a.x = d[pos];
        a.y = d[pos + 1];
        pos += 2;
        if (!get_path(d, size, pos, a.path))
            return false;
        out.altars.push_back(std::move(a));
    }
    for (std::size_t i = 0; i < targets; ++i) {
        if (pos + 6 > size)
            return false;
        PlanTarget t;
        t.slot = d[pos] & 7;
        t.attack = (d[pos] & 0x80) != 0;
        t.kind = d[pos + 1];
        t.gems = d[pos + 2];
        t.gold = d[pos + 3];
        t.x = d[pos + 4];
        t.y = d[pos + 5];
        pos += 6;
        if (!get_path(d, size, pos, t.path))
            return false;
        out.targets.push_back(std::move(t));
    }
    return true;
}

// What: the tiles of a route in walking order (the start tile first).
std::vector<std::pair<int, int>> path_tiles(const PlanPath& path) {
    std::vector<std::pair<int, int>> tiles;
    int x = path.start_x, y = path.start_y;
    tiles.push_back({x, y});
    for (std::uint8_t code : path.steps) {
        x += kDeltas[code][0];
        y += kDeltas[code][1];
        tiles.push_back({x, y});
    }
    return tiles;
}
