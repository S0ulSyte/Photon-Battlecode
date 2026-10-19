// ability_util.h - what the bot knows how to do with champion abilities: read them for FIGHT CHECK, cast them, attack with a reset, dash.
//
// What it is for: (1) turn an SDK ability into the plain numbers FIGHT CHECK needs (skill_info / to_fight_skill / fill_design_skills), (2) the actions
//                 that use my abilities in a fight (cast_skill_at, basic_attack_with_reset, dash_next_to).
// Reads: my abilities (set once with g_my_abilities in Bot::init), the engine (cooldowns, valid tiles, targets). Writes: casts, attacks, dashes.
// Used by: bot_setup.cpp, fight.cpp, state files (through Bot::attack_camp / fight_enemy).
// Edit here to change: how ability metadata becomes simulation numbers (skill_info), which skill is cast first (cast_skill_at), how an attack reset
//                      is used (basic_attack_with_reset), where a dash lands (dash_next_to).
#pragma once
#include <cambc.h>
#include <array>
#include "config.h"
#include "fight_check.h"
#include "fuel_profile.h"

// What FIGHT CHECK needs to know about one ability.
struct SkillInfo {
    bool exists = false;  // damage, area damage, stun or attack reset
    int damage = 0;
    bool stun = false;
    bool aoe = false;
    int range = 4;
    bool excl_move = false;
    bool attack_reset = false;  // CHANGE 4: the ability is an attack reset (one extra basic attack when used)
};

// Reads an ability's metadata (no engine call: it is cached in the ability). `damage_override >= 0` replaces modifier * focus (used for our own
// abilities, whose real damage the engine can tell us).
SkillInfo skill_info(const cambc::ability& a, float focus, int damage_override = -1);

// The SkillInfo as the plain struct FIGHT CHECK takes, with the turns until it is ready.
FightSkill to_fight_skill(const SkillInfo& s, int ready_in);

// Fills unit.skills from a champion TYPE's design data (used for enemies and allies, whose cooldowns we cannot see: assumed ready now).
// Design data never changes, so each type is read from the engine once and cached (about 1,000 fuel once, then about 100).
void fill_design_skills(FightUnit& unit, cambc::champion_type type, float focus);

// My two ability objects, set once in Bot::init (the SDK's me.abilities() copies them on every call). nullptr until then.
extern const std::array<cambc::ability, 2>* g_my_abilities;

// The same test as cambc::ability::is_ready() (metadata present, not on cooldown, owner can cast), without the champion object that the SDK builds
// inside is_ready() just to ask can_cast() (~800 fuel). `can_cast` is passed in because every caller has just asked for it.
bool ability_ready(const cambc::ability& a, bool can_cast);

// What: cast the first ready damage, area-damage or stun skill that reaches `pos` (the skills are tried in slot order).
// Casts the first ready damage, area-damage or stun skill that reaches `pos`. `target` is the champion or camp aimed at.
// When `allow_exclusive` is false, skills that stop us moving are skipped. Returns true if a skill was cast.
template <class T>
bool cast_skill_at(const T& target, cambc::vec2 pos, bool allow_exclusive, bool* cast_was_exclusive = nullptr) {
    FUEL_SCOPE(FP_CAST);
    auto& me = cambc::self();
    if (!me.can_cast())
        return false;
    for (const auto& a : *g_my_abilities) {  // (refactor: the cached pair, not a copy from me.abilities())
        if (!(a.is_damage() || a.is_aoe_damage() || a.is_cc()) || !ability_ready(a, true))  // (can_cast was checked just above)
            continue;
        const SkillInfo si = skill_info(a, me.focus());
        if (si.excl_move && !allow_exclusive)
            continue;
        try {
            if (!a.valid_tiles().is_set(pos))
                continue;
            const bool ok = a.is_aoe_damage() ? a.use(pos) : a.use(target);
            if (ok) {
                if (cast_was_exclusive)
                    *cast_was_exclusive = si.excl_move;
                return true;
            }
        } catch (const cambc::error&) {
            // Refused (target not castable right now): try the next skill.
        }
    }
    return false;
}

// Basic attack, then (if an attack-reset skill is ready) reset and attack again. Returns true if we attacked at least once.
template <class T>
bool basic_attack_with_reset(const T& target) {
    FUEL_SCOPE(FP_BASIC_ATTACK);
    auto& me = cambc::self();
    if (!me.can_attack() || !target.is_visible() || target.health() <= 0 || !me.attack_tiles().is_set(target.position()))
        return false;
    try {
        me.attack(target);
    } catch (const cambc::error&) {
        return false;
    }
    if (me.can_cast()) {
        for (const auto& a : *g_my_abilities) {  // (refactor: the cached pair)
            if (!a.is_attack_reset() || !ability_ready(a, true))  // (can_cast was checked just above)
                continue;
            try {
                if (a.use() && me.can_attack() && target.health() > 0)
                    me.attack(target);
            } catch (const cambc::error&) {
            }
            break;
        }
    }
    return true;
}


#if V2_CHANGE_DASH
// CHANGE 8. Dash to a free tile next to `target_pos`. Returns true if we dashed. `who` is the champion's name for the DASH log line. See ability_util.cpp.
bool dash_next_to(cambc::vec2 target_pos, const char* who);
#endif
