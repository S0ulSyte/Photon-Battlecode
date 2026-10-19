// config.h - every tunable number and switch of v2, in one place.
//
// What it is for: the switches that choose between the spec taken literally and my DEFAULT fixes (V2_LITERAL_*), the 15 requested changes
// (V2_CHANGE_*, each 1 = on / 0 = off), the weights, and the plain constants (namespace cfg). A switch is a macro, so it can be overridden on the
// compiler command line (-DV2_CHANGE_DASH=0) without editing this file; a constant in cfg is a constexpr.
// Reads: nothing. Writes: nothing. Included by almost every file.
// Edit here to change: any threshold or weight below. To change HOW a switch behaves, go to the file in the "used in" column.
// Every constant marked DEFAULT is a choice I made where the spec was silent or contradictory; each is listed in v2_report/transition_table.md (D1-D32).
//
// ======================================== TABLE OF EVERY TUNABLE (name, value, what it does, used in) ========================================
//  SPEC-LITERAL SWITCHES (all 0 = my DEFAULT fix, 1 = the spec taken literally; they exist so a hole in the spec can be shown with a real match)
//   V2_LITERAL_FULL_HEALTH           0     state 3 switches away the moment a camp is at full health              state_camp_attack.cpp camp_hit_group
//   V2_NO_SWITCH_AFTER_FARM_CAPTURE  0     no target switch after capturing a farm camp                              state_default.cpp farm_group_step
//   V2_LITERAL_BANK_RECALL           0     BANK recalls every turn with no altar in sight (endless loop)             state_bank.cpp state_bank
//   V2_LITERAL_FLAG_RESET            0     standardCombatAllowed stays false during BANK                             bot.cpp run_states
//  REQUESTED CHANGES (1 = on)
//   V2_GEM_THRESHOLD                 10    change 1 (off): BANK when carrying more than this many gems               below (cfg::kGemThreshold)
//   V2_LOW_HP_PERCENT                35    change 2: recall below this % of max health                               turn: bot.cpp low_hp_recall
//   V2_CHANGE_DEPOSIT_AT_SPAWN       1     change 3: BANK also starts on spawn with any gem (with a latch, D20)       state_bank.cpp bank_wanted, bot.cpp
//   V2_CHANGE_RESET_DAMAGE           1     change 4: FIGHT CHECK counts attack resets                                 ability_util.cpp skill_info, fight_check.cpp
//   V2_W_MY_TURNS                    1.0   change 6: weight on my turns in the WIN test                               fight_check.cpp simulate_fight
//   V2_CHANGE_TARGET_SCORE           0     change 7 (off): pick the enemy by a score, not by distance                 enemy_memory.cpp nearest_enemy, fight_check.cpp
//   V2_W_TARGET_DIST / _HEALTH       1.0/1.0  change 7: weights of distance and health/100 in that score             same
//   V2_CHANGE_DASH                   1     change 8: dash next to the enemy in a won fight                            ability_util.cpp dash_next_to, fight.cpp
//   V2_CHANGE_FREE_SLOT              1     change 9: take a free camp on the way to a farm target                     state_default.cpp, bot_setup.cpp, camp_memory.cpp
//   V2_CHANGE_REWARD_SCORES          0     change 10 (off): farm score = reward per step                              plan.cpp score_candidates
//   V2_W_GOLD_PER_GEM                0.25  change 10: gems one gold is worth                                          plan.cpp
//   V2_CHANGE_WAIT_STATE             1     change 11: wait beside the camp that respawns first                        state_camp_attack.cpp, state_default.cpp
//   V2_CHANGE_FIGHT_IN_CAMP          1     change 12: fight back in state 3 when FIGHT CHECK says WIN                 state_camp_attack.cpp camp_fight_back
//   V2_CHANGE_SHARED_ALLIES          1     change 13: allies out of sight from shared memory                          fight.cpp, shared_memory.cpp, champion_util.cpp
//   V2_CHANGE_FUEL                   1     change 14: one camp-group state read per group per round                   camp_memory.cpp group_state
//   V2_CHANGE_RANGE_EQUAL            0     change 15 (off): ranged units see every enemy in range as equally far     enemy_memory.cpp nearest_enemy, fight_check.cpp
//  TOOLS
//   V2_FUEL_PROFILE                  0     measure the fuel of each piece (FUEL_SCOPE) and log it                     fuel_profile.h, turn_end.cpp
//   V2_TRACE_SLOT / _FROM / _TO      -1/0/0  per-turn trace line for one champion slot between two rounds               turn_end.cpp finish_turn
//  CONSTANTS in namespace cfg (below)
//   kGemThreshold 10, kMemoryTurns 3, kLowHpPercent 35, kReturnNearSpawnSteps 5, kReturnEnemyRange 6, kCampRange 3, kEnemyVision 8,
//   kCampActiveTurns 2, kFullHealthGraceTurns 3, kEngagementQuietTurns 4, kEngagementMaxTurns 80, kDetourMaxTurns 150, kDetourUnscratchedTurns 6,
//   kDetourBanRounds 150, kAllyNearCampRange 4, kSkillReach 4, kEnemyStatsRefreshRounds 10, kShardsPerEmblem 5, kFightMaxUnits 16, kFightMaxTurns 40,
//   kSkillCooldownTurns 8, kChampionTypes 12, respawn rounds 40 / 50 / 60, kTileKeyStride 256, kGroupMatchRadius 2, kSharedMaxTiles 4096,
//   kSharedTileBits 12, kSharedHealthLevels 15, kFuelLogMinLeft 35000, kFuelLogEveryRounds 50, kFuelPiecesPerLine 12, kHeavyTurnFuel 70000,
//   kHeavyLogMinLeft 15000. Not moved here: the draft-memory file format numbers in plan.h / plan.cpp (they define a format, not a tuning), the
//   2-camps-per-pick rule in target_select.cpp, and the trivial 0 / 1 / 2.
#pragma once
#include <cstddef>
#include <cstdint>

// ==================================================== SECTION 1: SPEC-LITERAL SWITCHES ====================================================
// Define one of these on the compiler command line (or edit here) to build a variant that follows the spec LITERALLY at that point.
// They exist so a hole in the spec can be demonstrated with a real match instead of an argument.
#ifndef V2_LITERAL_FULL_HEALTH
// Spec, state 3: "If it's at full health ... switch target to the other camp". Literal reading: the moment we reach the camp it IS at full
// health, so we switch away at once. DEFAULT (0): only switch if it is still at full health after we have been hitting it for a while.
#define V2_LITERAL_FULL_HEALTH 0
#endif
#ifndef V2_NO_SWITCH_AFTER_FARM_CAPTURE
// Spec: only state 3 (attack camps) ever switches target. Literal reading: after capturing a FARM camp nothing sends us elsewhere.
// DEFAULT (0): after a farm capture we switch to the other camp too.
#define V2_NO_SWITCH_AFTER_FARM_CAPTURE 0
#endif

#ifndef V2_LITERAL_BANK_RECALL
// Spec, state 2: "If an altar is in vision, BFS to it. Otherwise recall and follow the path to the closest altar in draft memory."
// Literal reading: EVERY turn with no altar in sight and not standing on a spawn tile triggers a recall, so after a recall brings us to
// spawn, the first step along the route (off the spawn tiles) triggers another recall: an endless recall loop that never banks.
// DEFAULT (0): recall once per trip; after arriving at spawn, walk the route without recalling again.
#define V2_LITERAL_BANK_RECALL 0
#endif

#ifndef V2_LITERAL_FLAG_RESET
// Spec: standardCombatAllowed is set false by state 3 and set back to true only by state 4, which runs only on turns that no earlier state
// handles. Literal reading: while we carry gems, BANK (state 2) handles every turn, so the flag stays false and state 1 never runs: a
// champion being hit during BANK neither fights nor flees (and keeps restarting a recall that every hit interrupts).
// DEFAULT (0): any turn that BANK handles also sets the flag back to true.
#define V2_LITERAL_FLAG_RESET 0
#endif

// ===================================================== SECTION 2: REQUESTED CHANGES (CHANGE 3 TO 15) ========================================
// (Changes 1 and 2 are the plain constants V2_GEM_THRESHOLD and V2_LOW_HP_PERCENT in section 4. Change 5 needed no code.)
#ifndef V2_CHANGE_DEPOSIT_AT_SPAWN
// CHANGE 3: BANK (state 2) also starts when we stand on a spawn tile carrying ANY gem (not only more than kGemThreshold): a champion that
// is already home with a few gems goes to the altar instead of carrying them back out to be lost on a death. 1 = on, 0 = the old rule.
// Together with change 1 (a higher threshold) this is the "batch banking" proposal (fix 5).
#define V2_CHANGE_DEPOSIT_AT_SPAWN 1
#endif

#ifndef V2_CHANGE_RESET_DAMAGE
// CHANGE 4: FIGHT CHECK counts attack resets (D9 used to ignore them): skill_info (bot.cpp) marks a unit's attack-reset ability, and the
// simulation (fight_check.cpp) gives a due reset one extra basic attack on a turn with a landed attack and no other cast. 1 = on, 0 = old.
#define V2_CHANGE_RESET_DAMAGE 1
#endif

#ifndef V2_W_MY_TURNS
// CHANGE 6: weight on MY turns in FIGHT CHECK. The check says WIN when (turns the fight needs to kill every enemy) * V2_W_MY_TURNS is
// STRICTLY less than (turns until I am dead). "My turns" = the turns I (and my side) need to kill them. 1.0 = the old rule. Below 1.0 the
// bot is bolder (a tie becomes a win; see fight_check.cpp: the simulation then keeps running after my death so the kill time exists);
// above 1.0 it is more careful. A plain number, override with -DV2_W_MY_TURNS=0.8 to tune it later.
#define V2_W_MY_TURNS 1.0
#endif

#ifndef V2_CHANGE_TARGET_SCORE
// CHANGE 7: pick the enemy to fight by a SCORE instead of "nearest": the enemy with the lowest
//   V2_W_TARGET_DIST * chebyshev_distance + V2_W_TARGET_HEALTH * (health / 100)
// (ties: a visible enemy first, then the lower id). Bot::nearest_enemy uses it, and so does unit 0 (me) in FIGHT CHECK's focus pick; every
// other simulated unit still picks the nearest. 1 = on, 0 = nearest enemy as before.
// TURNED OFF (0): with 1, v2 as team A LOST to v1 (v1 reached 1000 gems in round 1408, gems 528 to 1012; evidence/change_07_target_score.txt),
// which trips the keep rule. The code is kept; build with -DV2_CHANGE_TARGET_SCORE=1 to get it.
#define V2_CHANGE_TARGET_SCORE 0
#endif
#ifndef V2_W_TARGET_DIST
// CHANGE 7 weight on the distance term (DEFAULT 1.0, to be optimised later; override with -D).
#define V2_W_TARGET_DIST 1.0
#endif
#ifndef V2_W_TARGET_HEALTH
// CHANGE 7 weight on the health term, where health enters as health/100 (DEFAULT 1.0, to be optimised later; override with -D).
#define V2_W_TARGET_HEALTH 1.0
#endif

#ifndef V2_CHANGE_DASH
// CHANGE 8: when FIGHT CHECK says WIN and the enemy is more than 1 tile away, a champion with a ready dash ability (Issac and Vendetta of the
// drafted ones; Cassi, Ryth, Marcella and Varg have none) dashes to a legal free tile next to the enemy before attacking. 1 = on, 0 = off.
#define V2_CHANGE_DASH 1
#endif

#ifndef V2_CHANGE_FREE_SLOT
// CHANGE 9: a champion walking to a FARM target takes the first alive camp within its attack reach that has a free slot (fewer allies standing on
// the camp group's tiles than the camp's capacity: elk 2, wolf / gem / gold cluster 1, monolith 0; the group must be alive or have its reward
// available, not respawning), instead of only its planned camp. Fixes a champion stuck beside another champion's camp (see
// evidence/change_09_free_slot_instance.txt). 1 = on, 0 = off.
#define V2_CHANGE_FREE_SLOT 1
#endif

#ifndef V2_CHANGE_REWARD_SCORES
// CHANGE 10 (proposed fix 1): the draft's FARM score of a camp is its group's reward per walking step instead of just 1 / distance:
//   (gems + V2_W_GOLD_PER_GEM * gold) / steps, scaled so the best camp scores 255. Gold clusters (no gems) therefore stop looking as good as
// wolves. The plan version byte is 3 because each target now also carries its gems and gold. If no camp has reward data the old scores are used.
// 1 = on, 0 = old distance-only scores.
// TURNED OFF (0): with 1, v2 LOST to v1 from both spawns (as A: v1 reached 1000 gems in round 984, gems 290 to 1012; as B: round limit-ish, v1 won
// at round 1581, gems 790 to 1012; evidence/change_10_reward_scores.txt). The plan now sends Cassi/Issac/Ryth to elks (21,3) and (37,3), 16 to
// 32 steps away; why that loses was not isolated. Build with -DV2_CHANGE_REWARD_SCORES=1 to get it. (Plan version byte stays 3.)
#define V2_CHANGE_REWARD_SCORES 0
#endif
#ifndef V2_W_GOLD_PER_GEM
// CHANGE 10: how many gems one gold is worth when scoring camps (DEFAULT 0.25, the user's answer: gold counts a quarter, because gold buys
// shards and is the second tiebreak but does not win the game).
#define V2_W_GOLD_PER_GEM 0.25
#endif

#ifndef V2_CHANGE_WAIT_STATE
// CHANGE 11 (proposed fix 2): when both of a champion's target camps are respawning, wait beside the one whose respawn ends first instead of
// switching between them ("camp already captured" ping-pong, hole 5). The bot remembers each target's respawn round itself: the remaining
// timer the engine reports when the camp is in sight, or, if that is unusable, the respawn time of the camp kind (wolf 40, gold cluster 50,
// elk 60, gem cluster 60 rounds); a target never seen respawning counts as available. 1 = on, 0 = old behaviour.
#define V2_CHANGE_WAIT_STATE 1
#endif

#ifndef V2_CHANGE_FIGHT_IN_CAMP
// CHANGE 12 (proposed fix 4): in state 3 (camp attack), when an enemy is in sight and FIGHT CHECK says WIN, fight that enemy (fight_enemy)
// instead of hitting the camp. Before, those turns only counted in `ignored_enemy` (hole 9). 1 = on, 0 = old behaviour.
#define V2_CHANGE_FIGHT_IN_CAMP 1
#endif

#ifndef V2_CHANGE_SHARED_ALLIES
// CHANGE 13 (proposed fix 3): ally positions and health through the team's 8 shared bytes, so FIGHT CHECK also counts allies that are out of
// vision. Layout: 2 bytes per champion slot 0-3 (Issac, Cassi, Vendetta, Ryth), little endian: bits 0-11 = tile index (y * map width + x), bits
// 12-15 = health level (1-15 = ceil(15 * health / max health), 0 = dead or never written). Slots 4 and 5 write nothing and unused bytes stay
// zero. Each champion writes ONLY its own two bytes, at the end of every turn (shared memory is writable all match, unlike draft memory, whose
// write after the pick gave engine error 18). A map with more than 4096 tiles turns the feature off (the 12-bit index would not fit).
// 1 = on, 0 = off.
// MERGED: now 0. The 8 shared bytes are used for SHARED TARGETS instead (one byte per champion slot, see shared_memory.cpp), which needs the same bytes.
#define V2_CHANGE_SHARED_ALLIES 0
#endif

#ifndef V2_CHANGE_FUEL
// CHANGE 14 (proposed fix 6): read each camp group's state at most once per round (group.state() is called by state 3, state 5 and the
// change-9 detour, sometimes twice in a turn) and keep a cache of the answer, including "not in vision". The answers cannot change inside a
// turn before the action that would change them, so the games must come out identical; only the fuel changes. 1 = on, 0 = every call asks the
// engine. The FUEL log lines also report the fuel of an uncached state() call and the call / cache-hit counts. (No cap on simulated units or
// turns: it is only to be added if the peak fuel after changes 1-13 is above 90,000.)
#define V2_CHANGE_FUEL 1
#endif

#ifndef V2_CHANGE_RANGE_EQUAL
// CHANGE 15: in standard combat a RANGED champion (attack range above 1) treats every enemy within its attack range as the same distance
// (distance = max(real distance, attack range)), so it no longer walks past a weak enemy in range to chase the "nearest" one. Ties then go to a
// visible enemy, then the lower health, then the lower id (DEFAULT D32; the simulation already broke ties by lower health, then id).
// Used by Bot::nearest_enemy (the fight target) and by FIGHT CHECK's nearest-enemy pick for every simulated ranged unit, mine and theirs, so
// the prediction and the fight look at the same target. 1 = on, 0 = plain nearest.
// TURNED OFF (0): with 1 v2 beat v1 from both spawns (A round 1681, B round 1610) but in the B game one turn ran out of fuel (overruns 1, peak
// 98,539), which trips the keep rule (evidence/change_15_range_equal.txt). Build with -DV2_CHANGE_RANGE_EQUAL=1 to get it.
#define V2_CHANGE_RANGE_EQUAL 0
#endif

// ============================================================ SECTION 3: TOOLS AND DEBUG AIDS ============================================================
#ifndef V2_FUEL_PROFILE
// REFACTOR (stage 1): measure the fuel of each piece of the turn with FUEL_SCOPE (fuel_profile.h) and print `FUEL pieces` log lines every 50
// rounds. The measuring itself costs fuel (about two fuel_remaining() calls per scope; on the heaviest turn that was enough to push one of the
// check games over the limit), so the default is OFF and tools/fuel_profile.sh builds a copy of the bot (bots/v2_profile) with it ON. 1 = on, 0 = off.
#define V2_FUEL_PROFILE 0
#endif

#ifndef V2_TRACE_SLOT
// Debug aid: set to a champion slot (0 Issac, 1 Cassi, 2 Vendetta, 3 Ryth, 4 Marcella, 5 Varg) to log one line per turn for it
// between V2_TRACE_FROM and V2_TRACE_TO. -1 = off (the default; logging every turn costs fuel).
#define V2_TRACE_SLOT -1
#endif
#ifndef V2_TRACE_FROM
#define V2_TRACE_FROM 0
#endif
#ifndef V2_TRACE_TO
#define V2_TRACE_TO 0
#endif

// ================================================ SECTION 4: CONSTANTS (namespace cfg) ================================================
namespace cfg {
// ---- BANK and health ----
// BANK state: tribute when carrying MORE than this many gems. DEFAULT 10 (the spec left {THRESHOLD} blank): 10 gems is exactly one
// tribute tick (ceil(gems/10) turns), and any camp reward is bigger than that.
// CHANGE 1 (tried 50 instead of 10, to bank a batch of camps per trip; this also covers proposed fix 5 together with change 3): TURNED OFF.
// With 50, v2 as team B LOST to v1 (round limit 2000, gems 572 to 946; evidence/change_01_gem_threshold_50.txt), which trips the keep rule.
// The old value is the default again; rebuild with -DV2_GEM_THRESHOLD=50 to get the changed behaviour.
#ifndef V2_GEM_THRESHOLD
#define V2_GEM_THRESHOLD 10
#endif
constexpr int kGemThreshold = V2_GEM_THRESHOLD;
// ---- Memory and engagements ----
// How many turns an enemy that went out of sight is still assumed to be there (spec: 3).
constexpr int kMemoryTurns = 3;
// ---- Recall and the walk home ----
// Low-HP recall below this percent of maximum health (spec: 20). CHANGE 2: raised to 35 (old value 20) so a hurt champion leaves earlier.
// Override with -DV2_LOW_HP_PERCENT=20 to get the old behaviour.
#ifndef V2_LOW_HP_PERCENT
#define V2_LOW_HP_PERCENT 35
#endif
constexpr int kLowHpPercent = V2_LOW_HP_PERCENT;
// "Within 5 steps of spawn" and "enemy within vision range 6" in state 3's return trip (spec numbers).
constexpr int kReturnNearSpawnSteps = 5;
constexpr int kReturnEnemyRange = 6;
// ---- Camps ----
// "Within range 3 of an active camp" (camps attack out to range 3).
constexpr int kCampRange = 3;
// MERGED: a live camp this close (Chebyshev) to me makes the champion run FIGHT CHECK against the camps before attacking or walking on (camp_check).
constexpr int kCampCheckRange = 6;
// MERGED: the most camps put into that FIGHT CHECK (the nearest ones).
constexpr int kCampCheckMaxCamps = 3;
// MERGED: the most units (me, camps, allies, shared allies, enemy champions) in a camp FIGHT CHECK; the simulation's cost grows with it.
constexpr int kCampCheckMaxUnits = 8;
// MERGED: turns FIGHT CHECK simulates for a fight against camps (camps have 500-2400 health, so a champion fight's 40 turns is far too short).
constexpr int kCampFightMaxTurns = 400;
// MERGED: a camp FIGHT CHECK verdict (win or loss) is reused for this many rounds, unless my health fell more than 10% or the number of camps in range changed.
constexpr int kCampCheckReuseRounds = 3;
// MERGED: "about to die": if the camp FIGHT CHECK has me dead within this many turns, I leave (retreat, then recall) whatever the team verdict says.
constexpr int kCampAboutToDieTurns = 4;
// MERGED (shared targets): a champion below this % of max health ignores its allies' targets and does what it normally would.
constexpr int kFollowMinHpPercent = 10;
// MERGED (low-health retreat): below this % of my own health, and with my opponent (the nearest enemy champion in sight or camp that can hit) above the second %, I step out of
// its attack range and recall. An opponent at or below the second % is nearly dead: finish it.
constexpr int kRetreatMyHpPercent = 25;
constexpr int kRetreatOpponentHpPercent = 15;
// MERGED (low-health retreat): keep retreating until every opponent is more than its reach + this many tiles away (or out of sight), and only then recall; give up
// retreating and recall after this many rounds of it.
constexpr int kRetreatExtraTiles = 4;
constexpr int kRetreatMaxRounds = 8;
// MERGED (shared targets): the largest offset (in tiles, each axis) a target can have from the champion that writes it; a farther target is not shared.
constexpr int kShareMaxOffset = 7;
// MERGED: the camp check is skipped on a turn with less fuel than this left (the simulation can cost 50,000+ against camps; the limit is 100,000 a turn).
constexpr std::uint64_t kCampCheckMinFuel = 85000;
// MERGED: after FIGHT CHECK predicts a loss at a camp: below this % of max health the champion recalls to heal; at or above it the camp is simply too strong, so it switches target.
constexpr int kCampHurtPercent = 90;
// MERGED: on a spawn tile and below max health the champion stays this many turns in a row at most, so spawn healing (the engine heals spawn_healing = 100% each round) fills it.
constexpr int kSpawnHoldMax = 3;
// Enemy vision radius used to find "a tile outside the enemy's vision range" when fleeing (engine: 8 for every champion).
constexpr int kEnemyVision = 8;
// DEFAULT: a camp counts as ACTIVE (awake) if its health changed within this many turns. The engine has no "awake" flag.
constexpr int kCampActiveTurns = 2;
// DEFAULT: turns we must have been hitting a camp before "at full health" means "we are not hurting it".
constexpr int kFullHealthGraceTurns = 3;
// An engagement with no enemy seen for this many turns is over (logged as DISENGAGED), and one this long is logged as TIMEOUT.
constexpr int kEngagementQuietTurns = 4;
constexpr int kEngagementMaxTurns = 80;
// ---- FUEL log (refactor, stage 1) ----
// (refactor) A FUEL log line is only written on a turn that has at least this much fuel left (a line costs up to ~15,000 fuel).
constexpr std::uint64_t kFuelLogMinLeft = 35000;
// ---- Free-slot detour (change 9, D22) ----
// (refactor) change 9 (D22): a detour is dropped after this many turns, or after this many turns without scratching the camp; the group is then avoided.
constexpr int kDetourMaxTurns = 150;
constexpr int kDetourUnscratchedTurns = 6;
constexpr int kDetourBanRounds = 150;
// (refactor) "an ally is fighting it": an ally within this many tiles of a damaged camp (state 5, help_allies_hit_camp).
constexpr int kAllyNearCampRange = 4;
// ---- Shared memory (change 13) ----
// (refactor) change 13: shared memory packs a tile into 12 bits, so it is used only on maps with at most this many tiles (collision has 1,681).
constexpr int kSharedMaxTiles = 4096;  // = 1 << kSharedTileBits (defined below)
constexpr int kSharedTileBits = 12;                           // a tile index takes the low 12 bits of a champion's 16-bit shared-memory word
constexpr unsigned kSharedTileMask = (1u << kSharedTileBits) - 1;  // 0xFFF
constexpr int kSharedHealthLevels = 15;                        // the high 4 bits hold a health level 1..15 (0 = dead or never written)

// ---- Fighting ----
constexpr int kSkillReach = 4;           // every champion ability reaches 4 tiles (engine facts): a skill is tried only on a target within this distance
constexpr int kSkillCooldownTurns = 8;   // every ability's cooldown in the engine is 8 turns (evidence/engine_facts.txt); FIGHT CHECK assumes it
constexpr std::size_t kFightMaxUnits = 16;  // FIGHT CHECK simulates at most this many units (extra ones are ignored)
constexpr int kFightMaxTurns = 40;       // FIGHT CHECK simulates at most this many turns; a fight not decided by then is not a win
constexpr int kChampionTypes = 12;       // number of champion types in the SDK (champion_type 0..11)
constexpr int kEnemyStatsRefreshRounds = 10;  // an enemy's strength and focus only change when it buys items, so they are re-read this often

// ---- Shop ----
constexpr std::uint32_t kShardsPerEmblem = 5;  // an emblem is built from 5 shards of its family

// ---- Camp facts ----
constexpr int kKindWolf = 0;            // camp_type numbers used in the plan: 0 wolf, 1 elk, 2 gem cluster, 3 gold cluster, 4 monolith
constexpr int kKindGoldCluster = 3;
constexpr int kWolfRespawnRounds = 40;       // rounds a cleared camp takes to respawn (evidence/engine_facts.txt)
constexpr int kGoldClusterRespawnRounds = 50;
constexpr int kOtherRespawnRounds = 60;      // elk and gem cluster
// Respawn rounds of a camp of the given plan kind (used when the engine's own timer cannot be read).
inline int respawn_rounds(int kind) {
    return kind == kKindWolf ? kWolfRespawnRounds : kind == kKindGoldCluster ? kGoldClusterRespawnRounds : kOtherRespawnRounds;
}
constexpr int kTileKeyStride = 256;     // a tile (x, y) is keyed y * 256 + x in the camp maps (every map is narrower than 256)
constexpr int kGroupMatchRadius = 2;    // a camp belongs to the group with a member spawn tile within this many tiles (a camp may stand a step off its spawn)

// ---- FUEL log (refactor): how it is written ----
constexpr int kFuelLogEveryRounds = 50;     // a FUEL line is queued this often
constexpr int kFuelPiecesPerLine = 12;      // pieces per `FUEL pieces` line (a log line holds ~600 characters)
constexpr std::uint64_t kHeavyTurnFuel = 70000;   // (profile builds) a turn that used more than this is listed in a `FUEL heavy` line
constexpr std::uint64_t kHeavyLogMinLeft = 15000; // ... only if at least this much fuel is left to write it
}  // namespace cfg
