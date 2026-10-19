// camp_memory.cpp - What the bot knows about camps: which are awake (health changing), which group a camp belongs to, and the state of a camp group.
//
// Reads: the engine (camps in sight, camp group states). Writes: camps_seen_, seen_now_, group_cache_.
// Edit here to change: what counts as an awake camp (cfg::kCampActiveTurns, D8), how camps are matched to groups (group_of), how group states are cached (group_state).
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <variant>
#include "ability_util.h"
#include "bot.h"
#include "champion_util.h"
#include "config.h"
#include "fuel_profile.h"
#include "log.h"
#include "moves.h"

using cambc::team;
using cambc::vec2;

// What: record, for every camp in sight, its tile and health, and the round its health last changed (the engine has no "awake" flag).
// Why: state 1 needs to know which camps are active, and the detour needs their health.
// How: REFACTOR (stage 1): the old code asked the SDK to build a camp object for every visible tile (vision::camps(), ~1,000 fuel each). A
//      camp that stands on its own spawn tile is already in map_camps_, so we only check that it really is at the tile (position()) and read its
//      health; any other case falls back to camp_at(), exactly the old path. Same camps, same order (tile scan order), same numbers.
// Inputs: none. Result: none. Side effects: writes camps_seen_ and seen_now_. Fuel: ~1,400 per turn.
void Bot::update_camps() {
    FUEL_SCOPE(FP_UPD_CAMPS);
    seen_now_.clear();  // refilled below with the camps seen this round
    for (const vec2 tile : cambc::vision::camp_tiles().positions()) {
        vec2 p = tile;
        int hp = 0;
        bool known = false;
        const auto it = map_camp_by_spawn_.find(tile.y * cfg::kTileKeyStride + tile.x);
        if (it != map_camp_by_spawn_.end()) {
            // ONE engine call gives both the position and the health (camp::position() and camp::health() each make this same call).
            // It uses the SDK's low-level import directly; a non-zero code means "not visible", and we fall back below.
            cambc::detail::camp_state state{};
            const cambc::camp& cached = map_camps_[it->second];
            if (cambc::detail::sdk_camp(static_cast<std::uint32_t>(static_cast<cambc::handle_camp_t>(cached)), &state) == 0 && state.x == tile.x && state.y == tile.y) {
                hp = static_cast<int>(state.health);  // the camp with this spawn tile really stands here now
                known = true;
            }
        }
        if (!known) {
            const cambc::camp c = cambc::vision::camp_at(tile);
            p = c.position();
            hp = static_cast<int>(c.health());
        }
        CampSeen& s = camps_seen_[p.y * cfg::kTileKeyStride + p.x];
        if (s.last_seen < 0 || hp != s.health)
            s.last_change = round_;
        s.pos = p;
        s.health = hp;
        s.last_seen = round_;
        seen_now_.push_back(&s);
    }
    // Same order as a walk over camps_seen_ (its key is y * cfg::kTileKeyStride + x).
    std::sort(seen_now_.begin(), seen_now_.end(), [](const CampSeen* a, const CampSeen* b) { return a->pos.y * cfg::kTileKeyStride + a->pos.x < b->pos.y * cfg::kTileKeyStride + b->pos.x; });
}

// What: the tiles of the camps seen this round whose health changed in the last kCampActiveTurns rounds ("awake" camps, DEFAULT D8). Fuel: ~180.
std::vector<vec2> Bot::active_camps() const {
    FUEL_SCOPE(FP_ACTIVE_CAMPS);
    std::vector<vec2> out;
    for (const CampSeen* s : seen_now_)  // (refactor: was a walk over all of camps_seen_ keeping the ones with last_seen == round_)
        if (s->health > 0 && round_ - s->last_change <= cfg::kCampActiveTurns)
            out.push_back(s->pos);
    return out;
}

// CHANGE 14. What: camp_group::state() with a one-round cache. Why: it costs fuel, and a turn could ask the same group two or three times (state 3,
// then state 5, then the detour check). How: remember the answer (or the "not in vision" failure) per group id for the current round; with
// V2_CHANGE_FUEL = 0 every call goes to the engine. Returns false when the engine refused (the group is not in vision); `out` is then untouched.
// Inputs: a camp group and where to put its state. Result: true if the state could be read (false = no member in sight; `out` untouched).
// Side effects: group_cache_. Fuel: ~550 per real engine call, ~30 when the answer is cached for this round.
bool Bot::group_state(const cambc::map::camp_group& g, cambc::map::camp_group_state& out) {
    FUEL_SCOPE(FP_GROUP_STATE);
    ++group_state_calls_;
    const std::uint32_t key = static_cast<std::uint32_t>(static_cast<cambc::handle_camp_group_t>(g));
    GroupStateCache* c = nullptr;
    for (GroupStateCache& e : group_cache_)  // (refactor: a short list searched in order instead of a std::map)
        if (e.id == key) {
            c = &e;
            break;
        }
    if (!c) {
        group_cache_.push_back({});
        c = &group_cache_.back();
        c->id = key;
    }
#if V2_CHANGE_FUEL
    if (c->round == round_) {
        ++group_state_hits_;
        if (c->ok)
            out = c->st;
        return c->ok;
    }
#endif
    c->round = round_;
    try {
        c->st = g.state();
        c->ok = true;
    } catch (const cambc::error&) {
        c->ok = false;
    }
    if (c->ok)
        out = c->st;
    return c->ok;
}

#if V2_CHANGE_FREE_SLOT
// What: which camp group owns a camp that stands at `pos`. Why: the vision gives camps, but slots and states belong to groups. How: the group
// with a member whose spawn tile is nearest to `pos` and within 2 tiles (a camp may stand a step off its spawn tile); -1 if none.
// Inputs: a tile. Result: the index into groups_ of the group with a member whose spawn tile is within 2 of it, or -1. No engine call; fuel ~400.
int Bot::group_of(vec2 pos) const {
    int best = -1, best_d = cfg::kGroupMatchRadius + 1;  // (a member must be strictly nearer than radius + 1, i.e. within the radius)
    for (std::size_t g = 0; g < groups_.size(); ++g)
        for (vec2 t : groups_[g].tiles)
            if (dist(pos, t) < best_d) {
                best_d = dist(pos, t);
                best = static_cast<int>(g);
            }
    return best;
}

#endif

