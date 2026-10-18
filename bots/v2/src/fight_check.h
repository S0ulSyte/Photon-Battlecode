// fight_check.h - FIGHT CHECK: a small turn-by-turn simulation that predicts who wins a fight. Pure C++ (no game SDK), so it can
//
// Reads/writes: nothing (pure functions; the bot builds the input in fight.cpp and the numbers for skills in ability_util.cpp).
// Edit here to change: what a fighter / skill is made of (the structs), the rules of the simulation (listed below; code in fight_check.cpp).
// be unit tested on a normal computer.
//
// How it works (the spec's rules, plus the choices marked "DEFAULT"):
//   - Every living unit, each turn, targets the NEAREST living enemy (Chebyshev distance; ties: lower health, then lower id).
//   - Movement: a unit out of attack range moves one tile toward its target. A MELEE unit (range 1) can move and attack in the same
//     turn; a RANGED unit cannot (it either moves or attacks). A skill that "excludes movement" stops the unit moving that turn.
//   - Basic attack: deals `strength` damage to the target if within `range` after moving.
//   - Skills: assumed to fire as soon as they can ("at fight start"), then again every 8 turns. A unit casts at most ONE skill per turn,
//     so a unit's two skills fire on turns 0 and 1, then 8 and 9, then 16 and 17, ... (skill 0 first). A skill that is on cooldown at the
//     start (ready_in > 0) fires later. A skill is only cast when its target is within the skill's range; otherwise it waits.
//   - Stun: a skill with `stun` delays the target's NEXT basic attack by one turn (the target skips one attack). Stun-immune units ignore it.
//   - Damage is applied at the END of each turn, to everyone at once (so equal fights end as a tie, not a win).
//   - DEFAULT: a unit keeps attacking the enemy it first picked until that enemy dies (focus fire), then picks the nearest again.
//   - DEFAULT: at most 16 units are simulated and at most 40 turns; a fight not decided by then is not a win.
//   - DEFAULT: skills that are neither damage nor stun (dashes, shields, heals) are ignored. CHANGE 4: attack resets are modelled (see
//     FightSkill::attack_reset): a due reset on a turn with a landed basic attack and no other cast adds one more `strength` hit.
//   - DEFAULT: enemies' skills are assumed ready at the start (their cooldowns cannot be seen).
#pragma once
#include <vector>
#include "config.h"  // cfg::kFightMaxTurns

// Used for "never happens within the simulated turns".
constexpr int kFightNever = 1000000;

// One skill, reduced to what the simulation needs.
struct FightSkill {
    // false = this slot has no damage or stun skill (it is ignored).
    bool exists = false;
    // Damage the skill deals on a hit (0 for a pure stun).
    int damage = 0;
    // Whether it stuns its target.
    bool stun = false;
    // How far away the target may be.
    int range = 4;
    // Turns until it can first be cast (0 = ready now).
    int ready_in = 0;
    // true = casting it uses the unit's movement for that turn.
    bool excl_move = false;
    // CHANGE 4: true = this slot is an ATTACK RESET (Issac, Ryth, Vendetta, Marcella, ...). It is not cast like a damage skill: on a turn
    // when the unit lands a basic attack and has not cast anything else, the reset (if due) gives ONE extra basic attack (`strength` damage).
    // `exists` is also true for it, so it takes part in the "one cast per turn" ordering of the two slots. Marcella's enhanced reset also
    // stuns in the real game; that stun is not modelled (she is stun-immune herself, and the extra hit is what counts).
    bool attack_reset = false;
};

// One fighter.
struct FightUnit {
    // Unique number; the caller uses it to say which unit is "me".
    int id = 0;
    // true = on our side.
    bool mine = false;
    // Map tile.
    int x = 0, y = 0;
    // Current health.
    int health = 0;
    // Basic attack damage.
    int strength = 0;
    // Basic attack range: 1 = melee, 3 = ranged.
    int range = 1;
    // Immune to stuns (Marcella).
    bool stun_immune = false;
    // The two ability slots.
    FightSkill skills[2];
};

// What the simulation found.
struct FightResult {
    // Number of turns until every enemy is dead (kFightNever if that does not happen).
    int turns_to_kill_enemies = kFightNever;
    // Number of turns until unit `me_id` is dead (kFightNever if it survives).
    int turns_until_me_dead = kFightNever;
    // true when we kill them strictly sooner than they kill me. A tie, or nobody dying, is NOT a win.
    bool win = false;
};

// Simulates the fight for up to `max_turns` turns. `units` holds both sides (use FightUnit::mine); `me_id` is the unit whose
// survival matters for the result.
FightResult simulate_fight(const std::vector<FightUnit>& units, int me_id, int max_turns = cfg::kFightMaxTurns);
