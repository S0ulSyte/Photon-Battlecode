// ability_util.cpp - see ability_util.h.
#include "ability_util.h"
#include <algorithm>
#include <map>
#include <type_traits>
#include <variant>
#include "log.h"
#include "moves.h"

const std::array<cambc::ability, 2>* g_my_abilities = nullptr;

// What: see ability_util.h. Fuel: ~110 (one cooldown query).
bool ability_ready(const cambc::ability& a, bool can_cast) {
    return a.info().has_value() && !a.is_on_cooldown() && can_cast;
}

// Inputs: an ability, my focus, an optional real damage. Result: its numbers for FIGHT CHECK. No engine call. Fuel: ~50.
// Reads an ability's metadata; `damage_override >= 0` replaces modifier * focus (used for our own abilities, whose real damage the
// engine can tell us).
SkillInfo skill_info(const cambc::ability& a, float focus, int damage_override) {
    SkillInfo s;
    const auto& info = a.info();
    if (!info)
        return s;
    std::visit(
        [&](const auto& d) {
            using D = std::decay_t<decltype(d)>;
            s.excl_move = d.exclusive_with_movement;
            if constexpr (std::is_same_v<D, cambc::ability_damage>) {
                s.exists = true;
                s.damage = damage_override >= 0 ? damage_override : static_cast<int>(d.modifier * focus);
                s.range = static_cast<int>(d.range);
            } else if constexpr (std::is_same_v<D, cambc::ability_aoe_damage>) {
                s.exists = true;
                s.aoe = true;
                s.damage = damage_override >= 0 ? damage_override : static_cast<int>(d.modifier * focus);
                s.range = static_cast<int>(d.range);
                s.stun = d.duration > 0;
#if V2_CHANGE_RESET_DAMAGE
            } else if constexpr (std::is_same_v<D, cambc::ability_attack_reset>) {
                // CHANGE 4: what/why/how: an attack reset deals no damage by itself, but the unit attacks a second time; the simulation
                // needs to know the slot is one, so mark it (the same code serves my own, allies' and enemies' abilities).
                s.exists = true;
                s.attack_reset = true;
#endif
            } else if constexpr (std::is_same_v<D, cambc::ability_cc>) {
                s.exists = true;
                s.damage = damage_override >= 0 ? damage_override : static_cast<int>(d.modifier * focus);
                s.range = static_cast<int>(d.range);
                s.stun = true;
            }
        },
        *info);
    return s;
}

// What: copy a SkillInfo into the plain FightSkill struct. No engine call.
FightSkill to_fight_skill(const SkillInfo& s, int ready_in) {
    FightSkill f;
    f.exists = s.exists;
    f.damage = s.damage;
    f.stun = s.stun;
    f.range = s.range;
    f.ready_in = ready_in;
    f.excl_move = s.excl_move;
    f.attack_reset = s.attack_reset;  // change 4
    return f;
}

// Inputs: the unit to fill, the champion TYPE, the unit's focus. Result: none. Side effects: unit.skills, a static per-type cache (reading a design costs ~1,000 once).
// The skills of a champion type from its design data (used for enemies and allies, whose cooldowns we cannot see: assumed ready).
// Design data never changes, so each type is read from the engine once and cached (reading it costs a lot of fuel).
void fill_design_skills(FightUnit& unit, cambc::champion_type type, float focus) {
    struct Cached {
        SkillInfo skill[2];
        float modifier[2] = {0, 0};
    };
    // REFACTOR: the champion_type numbers are 0..11, so a plain array replaces the std::map (a lookup was ~150 fuel, one per unit per FIGHT CHECK).
    static Cached cache[cfg::kChampionTypes];
    static bool filled[cfg::kChampionTypes] = {};
    const int key = static_cast<int>(type);
    if (key < 0 || key >= cfg::kChampionTypes) {
        // Unknown number: no skills (cannot happen with the 12 real champion types).
        unit.skills[0] = FightSkill{};
        unit.skills[1] = FightSkill{};
        return;
    }
    if (!filled[key]) {
        Cached c;
        try {
            const cambc::champion design(type);
            const auto abilities = design.abilities();
            for (std::size_t k = 0; k < 2; ++k) {
                c.skill[k] = skill_info(abilities[k], 1.0f);  // damage here = modifier * 1
                c.modifier[k] = static_cast<float>(c.skill[k].damage);
            }
        } catch (const std::exception&) {
            // Unknown design: the unit simply fights without skills.
        }
        cache[key] = c;
        filled[key] = true;
    }
    for (std::size_t k = 0; k < 2; ++k) {
        SkillInfo si = cache[key].skill[k];
        si.damage = static_cast<int>(cache[key].modifier[k] * focus);
        unit.skills[k] = to_fight_skill(si, 0);
    }
}


#if V2_CHANGE_DASH
// Inputs: where the enemy is, my name for the log. Result: true if a dash happened. Side effects: the dash, a DASH log line. Fuel: ~1,700 on average.
// CHANGE 8. What: dash to a free tile next to `target_pos`. Why: closing 2 tiles with one dash starts the damage a turn or two earlier.
// How: take the first ready dash ability; the tiles in its range (valid_tiles is range only) that are not blocked (wall, hole, altar, enemy
// spawn, any visible champion, camp or teleport; blocked_tiles()) and touch the target; try the one nearest to us first, and fall back to
// the next two if the engine refuses (a path it dislikes). Returns true if we dashed.
bool dash_next_to(cambc::vec2 target_pos, const char* who) {
    FUEL_SCOPE(FP_DASH);
    auto& me = cambc::self();
    const bool can_cast = me.can_cast();  // (refactor: asked once; is_ready() used to build a champion object to ask it)
    for (const auto& a : *g_my_abilities) {  // (refactor: the cached pair)
        if (!a.is_dash() || !ability_ready(a, can_cast))
            continue;
        try {
            const cambc::bitboard landing = a.valid_tiles() & ~blocked_tiles() & tiles_around(target_pos, 1);
            if (landing.is_empty())
                return false;  // no legal tile next to the target: do nothing different
            std::vector<cambc::vec2> tiles = landing.positions();
            const cambc::vec2 here = me.position();
            std::sort(tiles.begin(), tiles.end(), [&](cambc::vec2 x, cambc::vec2 y) { return dist(here, x) < dist(here, y); });
            int attempts = 0;
            for (cambc::vec2 t : tiles) {
                if (attempts++ >= 3)
                    break;
                try {
                    note_moved();  // (refactor) the cached position is stale from here on
                    if (a.use(t)) {
                        LogLine log;
                        log.s("DASH ").s(who).s(" r").n(static_cast<long long>(cambc::round_number())).s(" from ").xy(here.x, here.y).s(" to ").xy(t.x, t.y).s(" target ").xy(target_pos.x, target_pos.y);
                        log.flush();
                        return true;
                    }
                } catch (const cambc::error&) {
                    // Refused: try the next landing tile.
                }
            }
        } catch (const cambc::error&) {
        }
        return false;
    }
    return false;
}
#endif

