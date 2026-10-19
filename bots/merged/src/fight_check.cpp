// fight_check.cpp - the FIGHT CHECK simulation. See fight_check.h for the rules it follows.
//
// Reads: the units handed in (positions, health, strength, range, skills). Writes: nothing outside (a FightResult is returned).
// Edit here to change: the rules of a fight turn (decide_unit: targets, casts, moves, attacks, resets), when a fight counts as won (simulate_fight),
// which enemy a unit picks (nearest_enemy / scored_enemy). tests/fight_check_test.cpp has the hand-worked cases; tests/fight_check_equiv_test.cpp
// compares this file with the pre-refactor version on random fights - run both after any change here.
// Fuel: the bot's biggest single cost (14,000 on average per call, up to ~60,000): keep the loops in decide_unit short.
#include "fight_check.h"
#include "config.h"  // V2_W_MY_TURNS (change 6)
#include <algorithm>
#include <cstdlib>

namespace {

// Skills recharge 8 turns after they are cast.
constexpr int kCooldown = cfg::kSkillCooldownTurns;
// The simulation handles at most this many units (extra ones are ignored): it runs inside the bot, where compute is limited, so it
// uses fixed-size scratch arrays instead of allocating every turn.
constexpr std::size_t kMaxUnits = cfg::kFightMaxUnits;
// CHANGE 6: the weight on my turns (see config.h). At exactly 1.0 the simulation stops the moment I die, as it always did.
constexpr double kMyTurnsWeight = V2_W_MY_TURNS;

// A unit as the simulation keeps it: plain fields only (no default initialisers), so an array of them costs nothing to create.
// REFACTOR (stage 1): this replaces the old SimUnit that wrapped a FightUnit inside a std::vector; same numbers, no heap allocation. The skills are
// stored in a form that makes the per-turn tests short: `cast_range[k]` is the skill's range if the slot is a damage / stun skill and -1 otherwise
// (a distance is never negative, so "distance <= cast_range" alone means "castable"), and `reset[k]` marks an attack-reset slot.
struct SimUnit {
    int id;
    bool mine;
    bool stun_immune;
    bool alive;           // still alive
    bool melee;           // basic attack range 1 or less (a melee unit can move and attack in the same turn)
    bool immobile;        // MERGED: never moves (a camp's monster)
    int camp_out_pct, camp_in_pct, in_pct;  // MERGED: emblem passives in percent, see FightUnit
    int x, y;             // tile
    int health;
    int strength;         // basic attack damage
    int range;            // basic attack range
    int skip_attacks;     // how many of its next basic attacks it must skip (from stuns that landed on it)
    int target;           // index of the unit it is attacking, -1 = none yet (it keeps the same target until that one dies)
    int next_cast[2];     // for each skill: the first turn on which it may be cast
    int cast_range[2];    // range of a castable skill, -1 for an empty slot or an attack reset
    int cast_damage[2];
    bool cast_stun[2];
    bool cast_excl[2];    // casting it uses up the unit's move
    bool reset[2];        // the slot is an attack reset
};

// Chebyshev distance between two tiles.
inline int cheb(const SimUnit& a, const SimUnit& b) {
    const int dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const int dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}

// The nearest living enemy of unit `unit` among all[0..n), or -1 if there is none. Ties: lower health, then lower id.
// CHANGE 15: a ranged unit sees every enemy inside its attack range as equally far.
int nearest_enemy(const SimUnit* all, std::size_t n, std::size_t unit) {
    int best = -1, best_d = 0;
    const SimUnit& u = all[unit];
    for (std::size_t i = 0; i < n; ++i) {
        const SimUnit& e = all[i];
        if (!e.alive || e.mine == u.mine)
            continue;
        int d = cheb(u, e);
#if V2_CHANGE_RANGE_EQUAL
        if (u.range > 1 && d < u.range)
            d = u.range;
#endif
        if (best < 0) {
            best = static_cast<int>(i);
            best_d = d;
            continue;
        }
        const SimUnit& b = all[static_cast<std::size_t>(best)];
        if (d < best_d || (d == best_d && (e.health < b.health || (e.health == b.health && e.id < b.id)))) {
            best = static_cast<int>(i);
            best_d = d;
        }
    }
    return best;
}

#if V2_CHANGE_TARGET_SCORE
// CHANGE 7: the enemy of lowest V2_W_TARGET_DIST * distance + V2_W_TARGET_HEALTH * (health / 100), or -1 if none is alive. Used for unit
// `me_id` only, the same score Bot::nearest_enemy uses, so the prediction and the fight look at the same target. Ties: the lower id (the
// simulation has no notion of "visible").
int scored_enemy(const SimUnit* all, std::size_t n, std::size_t unit) {
    int best = -1;
    double best_score = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (!all[i].alive || all[i].mine == all[unit].mine)
            continue;
        const double score = V2_W_TARGET_DIST * cheb(all[unit], all[i]) + V2_W_TARGET_HEALTH * (all[i].health / 100.0);
        if (best < 0 || score < best_score || (score == best_score && all[i].id < all[static_cast<std::size_t>(best)].id)) {
            best = static_cast<int>(i);
            best_score = score;
        }
    }
    return best;
}
#endif

inline int sign(int v) { return (v > 0) - (v < 0); }

// Everything the simulation keeps between turns.
struct SimState {
    SimUnit all[kMaxUnits];  // the units (index = position in the input list)
    std::size_t n = 0;       // how many of them are used (the first kMaxUnits of the input)
    int enemies_alive = 0;   // living units on the other side
    bool me_dead = false;    // some unit with id me_id is dead
    int mine_alive = 0;      // MERGED: living units on my side
};

// What the units decided to do in one turn; applied together at the end of the turn.
struct TurnPlan {
    int damage[kMaxUnits];     // damage each unit will take
    int new_skips[kMaxUnits];  // stuns that will land on each unit
    int move_x[kMaxUnits];     // where each unit ends up
    int move_y[kMaxUnits];
};

// What: copy the input units into the simulation's fixed array and set the starting counts.
// Inputs: the input list, the id of "me". Result: the state ready for turn 0. Side effects: none. Fuel: ~100 per unit.
void load_units(SimState& st, const std::vector<FightUnit>& units, int me_id) {
    st.n = units.size() < kMaxUnits ? units.size() : kMaxUnits;  // extra units are ignored, as always
    for (std::size_t i = 0; i < st.n; ++i) {
        const FightUnit& u = units[i];
        SimUnit& s = st.all[i];
        s.id = u.id;
        s.mine = u.mine;
        s.stun_immune = u.stun_immune;
        s.alive = u.health > 0;
        s.melee = u.range <= 1;
        s.immobile = u.immobile;
        s.camp_out_pct = u.camp_out_pct;
        s.camp_in_pct = u.camp_in_pct;
        s.in_pct = u.in_pct;
        s.x = u.x;
        s.y = u.y;
        s.health = u.health;
        s.strength = u.strength;
        s.range = u.range;
        s.skip_attacks = 0;
        s.target = -1;
        for (int k = 0; k < 2; ++k) {
            const FightSkill& sk = u.skills[k];
            const bool castable = sk.exists && !sk.attack_reset;  // (change 4) an attack reset is not a cast: it is used after the basic attack lands
            s.cast_range[k] = castable ? sk.range : -1;
            s.cast_damage[k] = sk.damage;
            s.cast_stun[k] = sk.stun;
            s.cast_excl[k] = sk.excl_move;
            s.reset[k] = sk.exists && sk.attack_reset;
        }
        // Each skill may first be cast when it is ready; the two skills of one unit take different turns (skill 0 first), because
        // a unit casts at most one skill per turn.
        s.next_cast[0] = u.skills[0].ready_in;
        s.next_cast[1] = u.skills[1].ready_in;
        if (u.skills[0].exists && u.skills[1].exists && s.next_cast[1] <= s.next_cast[0])
            s.next_cast[1] = s.next_cast[0] + 1;
        if (s.alive && !s.mine)
            ++st.enemies_alive;
        if (s.alive && s.mine)
            ++st.mine_alive;
        if (!s.alive && s.id == me_id)
            st.me_dead = true;
    }
}

// MERGED: a hit of `dmg` from `attacker` on `target` after the emblem passives (100 = none, so default units are untouched).
inline int scaled_damage(int dmg, const SimUnit& attacker, const SimUnit& target) {
    if (target.immobile && !attacker.immobile && attacker.camp_out_pct != 100)
        dmg = (dmg * attacker.camp_out_pct + 50) / 100;
    if (attacker.immobile && !target.immobile && target.camp_in_pct != 100)
        dmg = (dmg * target.camp_in_pct + 50) / 100;
    if (target.in_pct != 100)
        dmg = (dmg * target.in_pct + 50) / 100;
    return dmg;
}

// What: one living unit decides its turn, from the state at the start of the turn: pick (or keep) a target, cast at most one skill, move and/or
// basic-attack, use an attack reset. Inputs: the state, the unit's index, the turn number, `me_id`, and the plan to add to.
// Result: none. Side effects: adds to `plan`; updates this unit's own target, cooldowns and stun counter (nothing of any other unit).
void decide_unit(SimState& st, std::size_t i, int turn, int me_id, TurnPlan& plan) {
    SimUnit* all = st.all;
    SimUnit& me = all[i];
    // Each unit keeps attacking the enemy it first picked until that enemy dies, then picks the nearest again (focus fire; also much
    // cheaper than re-scanning every enemy every turn).
    int ti = me.target;
    if (ti < 0 || !all[ti].alive) {
#if V2_CHANGE_TARGET_SCORE
        // CHANGE 7: only I pick by score; everyone else picks the nearest.
        ti = me.id == me_id ? scored_enemy(all, st.n, i) : nearest_enemy(all, st.n, i);
#else
        ti = nearest_enemy(all, st.n, i);
#endif
        me.target = ti;
    }
    if (ti < 0)
        return;
    const SimUnit& target = all[ti];
    const int tdx = target.x - me.x, tdy = target.y - me.y;
    const int adx = tdx < 0 ? -tdx : tdx, ady = tdy < 0 ? -tdy : tdy;
    const int dist = adx > ady ? adx : ady;

    // 1) Cast at most one skill: the first one that is due and has its target in range.
    bool cast = false, cast_blocks_move = false;
    for (int k = 0; k < 2; ++k) {
        if (dist > me.cast_range[k] || turn < me.next_cast[k])
            continue;
        cast = true;
        cast_blocks_move = me.cast_excl[k];
        plan.damage[ti] += scaled_damage(me.cast_damage[k], me, target);
        if (me.cast_stun[k] && !target.stun_immune)
            plan.new_skips[ti] += 1;
        me.next_cast[k] = turn + kCooldown;
        // The other skill may not be cast on the same turn.
        if (me.next_cast[1 - k] <= turn)
            me.next_cast[1 - k] = turn + 1;
        break;
    }

    // 2) Move and/or basic-attack.
    int nx = me.x, ny = me.y;
    int d_after = dist;
    bool moved = false;
    if (dist > me.range && !cast_blocks_move && !me.immobile) {
        nx += sign(tdx);
        ny += sign(tdy);
        const int ax = target.x > nx ? target.x - nx : nx - target.x;
        const int ay = target.y > ny ? target.y - ny : ny - target.y;
        d_after = ax > ay ? ax : ay;
        moved = true;
    }
    plan.move_x[i] = nx;
    plan.move_y[i] = ny;
    // A ranged unit cannot attack on a turn it moved; a melee unit can.
    if ((!moved || me.melee) && d_after <= me.range) {
        if (me.skip_attacks > 0) {
            me.skip_attacks -= 1;
        } else {
            plan.damage[ti] += scaled_damage(me.strength, me, target);
            // CHANGE 4: the attack landed. If no skill was cast this turn (a unit casts once per turn) and an attack reset is due,
            // use it: one extra basic attack of `strength`, and it goes on its 8-turn cooldown like any other ability.
            if (!cast) {
                for (int k = 0; k < 2; ++k) {
                    if (!me.reset[k] || turn < me.next_cast[k])
                        continue;
                    plan.damage[ti] += scaled_damage(me.strength, me, target);
                    me.next_cast[k] = turn + kCooldown;
                    // The other slot may not be cast on the same turn either.
                    if (me.next_cast[1 - k] <= turn)
                        me.next_cast[1 - k] = turn + 1;
                    break;
                }
            }
        }
    }
}

// What: end of turn: apply the moves, damage and stuns of all units together, and update the counts of living enemies and of my own death.
// Inputs: the state and the plan of this turn. Result: none. Side effects: positions, health, stun counters, alive flags.
void apply_turn(SimState& st, const TurnPlan& plan, int me_id) {
    for (std::size_t i = 0; i < st.n; ++i) {
        SimUnit& u = st.all[i];
        if (!u.alive)
            continue;
        if (u.target >= 0) {
            u.x = plan.move_x[i];
            u.y = plan.move_y[i];
        }
        u.health -= plan.damage[i];
        u.skip_attacks += plan.new_skips[i];
        if (u.health <= 0) {
            u.alive = false;
            if (!u.mine)
                --st.enemies_alive;
            else
                --st.mine_alive;
            if (u.id == me_id)
                st.me_dead = true;
        }
    }
}

}  // namespace

// What: play the fight out turn by turn and report when each side's last unit dies (see fight_check.h for the rules).
// Inputs: `units` (both sides; the first kMaxUnits are used), `me_id` (the unit whose death matters), `max_turns`.
// Result: FightResult. Side effects: none. Fuel: the bot's biggest single cost (14,000 on average, up to ~60,000, per call).
// How: everything lives in fixed arrays on the stack; per turn every living unit decides from the state at the start of the turn (decide_unit),
//      damage / stuns / moves are collected and applied together at the end (apply_turn); the counts of living enemies and my own death are kept
//      up to date as units die, so no per-turn scan is needed.
FightResult simulate_fight(const std::vector<FightUnit>& units, int me_id, int max_turns) {
    SimState st;
    load_units(st, units, me_id);
    FightResult result;
    for (int turn = 0; turn < max_turns; ++turn) {
        TurnPlan plan;
        for (std::size_t i = 0; i < st.n; ++i) {
            plan.damage[i] = 0;
            plan.new_skips[i] = 0;
        }
        for (std::size_t i = 0; i < st.n; ++i)
            if (st.all[i].alive)
                decide_unit(st, i, turn, me_id, plan);
        apply_turn(st, plan, me_id);

        // Has someone won?
        if (st.enemies_alive == 0 && result.turns_to_kill_enemies == kFightNever)
            result.turns_to_kill_enemies = turn + 1;
        if (st.me_dead && result.turns_until_me_dead == kFightNever)
            result.turns_until_me_dead = turn + 1;
        // Stop when the enemies are all dead, or when I am dead. CHANGE 6: with a weight other than 1.0 keep going after my death (allies may
        // still finish the enemies), because the rule compares weighted kill time against my death time and needs both numbers.
        if (st.enemies_alive == 0 || (st.me_dead && kMyTurnsWeight == 1.0))
            break;
    }
    // WIN = the enemies do die (a kill time exists) and (kill time * weight) is strictly less than my death time. With weight 1.0 this is the
    // old `kill < die`. The kFightNever guard keeps "nobody died" from turning into a win when the weight is below 1.
    result.win = result.turns_to_kill_enemies != kFightNever &&
                 static_cast<double>(result.turns_to_kill_enemies) * kMyTurnsWeight < static_cast<double>(result.turns_until_me_dead);
    return result;
}

// MERGED: see fight_check.h. The turn body is the same as in simulate_fight; what is new: it plays on after my death until the enemies or my whole side
// are dead (team result), and the optional jump over identical cycles.
FightResult simulate_team_fight(const std::vector<FightUnit>& units, int me_id, int max_turns, bool skip_cycles) {
    SimState st;
    load_units(st, units, me_id);
    FightResult result;
    SimState prev;  // the state one cycle ago (valid when have_prev)
    bool have_prev = false, jumped = false;
    int prev_turn = 0;
    int turn = 0;
    while (turn < max_turns && st.enemies_alive > 0 && st.mine_alive > 0) {
        TurnPlan plan;
        for (std::size_t i = 0; i < st.n; ++i) {
            plan.damage[i] = 0;
            plan.new_skips[i] = 0;
        }
        for (std::size_t i = 0; i < st.n; ++i)
            if (st.all[i].alive)
                decide_unit(st, i, turn, me_id, plan);
        apply_turn(st, plan, me_id);
        ++turn;
        if (st.enemies_alive == 0 && result.turns_to_kill_enemies == kFightNever)
            result.turns_to_kill_enemies = turn;
        if (st.me_dead && result.turns_until_me_dead == kFightNever)
            result.turns_until_me_dead = turn;
        if (st.mine_alive == 0 && result.turns_until_team_dead == kFightNever)
            result.turns_until_team_dead = turn;
        if (!skip_cycles || turn % kCooldown != 0)
            continue;
        // A cycle boundary. Is the fight periodic (everything but health equals the state one cycle ago)?
        if (have_prev && !jumped) {
            bool steady = true;
            for (std::size_t i = 0; i < st.n && steady; ++i) {
                const SimUnit& a = prev.all[i];
                const SimUnit& b = st.all[i];
                steady = a.alive == b.alive && a.x == b.x && a.y == b.y && a.target == b.target && a.skip_attacks == b.skip_attacks;
                for (int k = 0; k < 2 && steady; ++k) {
                    const int ra = a.next_cast[k] > prev_turn ? a.next_cast[k] - prev_turn : 0;
                    const int rb = b.next_cast[k] > turn ? b.next_cast[k] - turn : 0;
                    steady = ra == rb;
                }
            }
            if (steady) {
                // Whole cycles nobody dies in: health after m cycles stays at least 1 for every unit that loses health.
                int cycles = (max_turns - turn) / kCooldown;
                for (std::size_t i = 0; i < st.n; ++i) {
                    const int lost = prev.all[i].health - st.all[i].health;
                    if (st.all[i].alive && lost > 0)
                        cycles = std::min(cycles, (st.all[i].health - 1) / lost);
                }
                if (cycles > 0) {
                    for (std::size_t i = 0; i < st.n; ++i) {
                        SimUnit& u = st.all[i];
                        u.health -= cycles * (prev.all[i].health - u.health);
                        u.next_cast[0] += cycles * kCooldown;
                        u.next_cast[1] += cycles * kCooldown;
                    }
                    turn += cycles * kCooldown;
                }
                jumped = true;  // the cycle before the first death is now played exactly
            }
        }
        prev = st;
        prev_turn = turn;
        have_prev = true;
    }
    // WIN (team): the enemies die, strictly before my whole side does.
    result.win = result.turns_to_kill_enemies != kFightNever && result.turns_to_kill_enemies < result.turns_until_team_dead;
    return result;
}
