# CODEMAP - where everything is in bots/v2/src

Written for the 2026-10-15 refactor (stage 2). No line numbers (they move): find a function with `grep -n "Bot::name" bots/v2/src/*.cpp`.
The state machine is described in `v2_report/transition_table.md`; its step names (P0-P4, STATE 1-5) are used in the code comments.
Every `.cpp` / `.h` starts with a header saying what it is for, what it reads and writes, and an "edit here to change ..." list. Fuel numbers in the
function headers are averages per call from `v2_report/evidence/refactor_fuel_profile_after.txt`.

## Files

| file | what it holds |
|---|---|
| `main.cpp` | the program loop: draft or `Bot::play_turn`, once per round |
| `bot.h` | `class Bot`, the structs it keeps (`EnemyMem`, `CampSeen`, `Engagement`, `CampGroupInfo`, `ChampionView`) and every data member, grouped by topic |
| `bot.cpp` | `play_turn` and the steps before the states (P0-P4); runs states 1, 2, 3, 4, 5 |
| `bot_setup.cpp` | `Bot::init` (first turn: plan, camp groups, routes, abilities) and `const_of` |
| `enemy_memory.cpp` | champions in sight (views), the enemy memory (P0), logged engagements, `nearest_enemy` |
| `camp_memory.cpp` | camps seen, awake camps, camp group states and groups |
| `targets.cpp` | my two camp targets: current target, its route and group, switching |
| `movement.cpp` | my position (cached), spawn tests, walking a route one step |
| `fight.cpp` | FIGHT CHECK input building, `fight_enemy`, `flee`, `retreat_from_camps`, camp attacks |
| `shop.cpp` | buying strength shards |
| `shared_memory.cpp` | publishing my tile and health in the 8 shared bytes (change 13) |
| `state_combat.cpp` | STATE 1, standard combat |
| `state_bank.cpp` | STATE 2, BANK, and `bank_wanted` |
| `state_camp_attack.cpp` | STATE 3, camp attack: wait for respawn, capture, fight back, walk home |
| `state_default.cpp` | STATE 5, default: help allies, free-slot detour, capture, walk the route |
| `turn_end.cpp` | end-of-turn: fuel statistics, resume detection, the trace and FUEL log lines |
| `ability_util.h/.cpp` | ability metadata for FIGHT CHECK, `cast_skill_at`, `basic_attack_with_reset`, `dash_next_to` |
| `champion_util.h/.cpp` | `slot_of`, `id_of`, the one-call champion record `snapshot_of`, design stats of a slot |
| `moves.h/.cpp` | blocked tiles, `move_towards_closest`, the move counter (`note_moved`) |
| `fight_check.h/.cpp` | the FIGHT CHECK simulation (pure C++; unit tested natively) |
| `plan.h/.cpp` | the draft analysis: camp scores, routes, the draft-memory format |
| `target_select.h/.cpp` | which camps each champion gets (the slot rules) |
| `draft.h/.cpp` | the draft: the picks and the one-time map analysis |
| `config.h` | every switch and constant, with a table of all of them at the top |
| `fuel_profile.h` | the `FUEL_SCOPE` profiler (only with `V2_FUEL_PROFILE`) |
| `log.h` | `LogLine`, the cheap logger (about 25 fuel per character) |
| `random.h` | a seeded random number generator (not used today) |

## Functions

### bot.cpp
- `Bot::play_turn` - one turn: P0 memory, P1 dead, P2 channel/stunned, P3 shop, P4 low-HP recall, then the states.
- `Bot::begin_turn` - read the round, start the fuel bookkeeping, run `init` on the first turn.
- `Bot::update_memory` - P0: `update_enemies`, `update_camps`, `update_engagement`, with their fuel recorded.
- `Bot::update_bank_flags` - the deposit latch (D20) and the BANK flags.
- `Bot::act_dead` - P1: reset the transient flags (D4) and shop.
- `Bot::cannot_act` - P2: recalling / tributing / teleporting / stunned.
- `Bot::low_hp_recall` - P4: start a recall under `cfg::kLowHpPercent` health.
- `Bot::guarded` - run one state, catching engine errors (logged as `ERR`), recording its fuel.
- `Bot::run_states` - states 1, 2, 3, 4 (reset the flag), 5, first match wins.

### bot_setup.cpp
- `Bot::init` - first turn: decode the plan, find my targets' groups and routes, remember every camp, log `INIT`.
- `Bot::const_of` - type, range, slot and stun immunity of a champion, read once per handle.

### enemy_memory.cpp
- `Bot::champions_in_sight` - fill a `ChampionView` with the champions of one side in sight (cached objects).
- `Bot::enemy_view`, `Bot::ally_view` - those views, built at most once per round.
- `Bot::update_enemies` - P0: refresh `enemies_` (position, health, stats), forget the unseen / dead.
- `Bot::nearest_enemy` - the enemy to fight (nearest; or by score / range-equal with the switches).
- `Bot::update_engagement`, `Bot::end_engagement` - step and close the logged fight (`ENG` line).

### camp_memory.cpp
- `Bot::update_camps` - record tile, health and last-change round of every camp in sight; fills `seen_now_`.
- `Bot::active_camps` - the camps seen this round whose health changed lately ("awake", D8).
- `Bot::group_state` - a camp group's state, one engine read per group per round (change 14).
- `Bot::group_of` - which camp group owns a camp at a tile.

### targets.cpp
- `Bot::target` - my current target from the plan. `Bot::target_tiles` - its route (cached). `Bot::target_group` - its camp group if in sight.
- `Bot::switch_target` - go for my other target and reset the per-camp counters (`SWITCH` line).

### movement.cpp
- `Bot::my_pos` - my tile, cached between moves. `Bot::at_spawn` - am I on a spawn tile.
- `Bot::steps_from_spawn_on` - steps from spawn of my nearest route tile (D17).
- `Bot::follow_forward`, `Bot::follow_backward` - one step along a route toward its end / start.

### fight.cpp
- `Bot::fight_check` - FIGHT CHECK: build the fight, simulate, open the logged engagement.
- `Bot::fc_unit_me`, `Bot::fc_add_allies`, `Bot::fc_add_shared_allies`, `Bot::fc_add_enemies` - the four parts of the input.
- `Bot::fc_open_engagement` - the first FIGHT CHECK of a fight: `eng_` and the `ENGSTART` line.
- `Bot::fight_enemy` - dash (won fight), cast, close in, attack with the reset. `Bot::flee` - run away, or fight if cornered (D10).
- `Bot::retreat_from_camps` - leave the range of awake camps. `Bot::opportunistic_attacks` - hit the nearest enemy on the way somewhere.
- `Bot::attack_camp` - cast on / attack a camp.

### shop.cpp, shared_memory.cpp
- `Bot::shop` - buy shards in the shop order (D15). `Bot::write_shared` - write my two shared-memory bytes.

### State 1 / 2 / 3 / 5
- `Bot::state_standard_combat` (state_combat.cpp) - retreat from awake camps, else FIGHT CHECK and fight or flee.
- `Bot::state_bank`, `Bot::set_bank_phase`, `Bot::bank_wanted` (state_bank.cpp) - tribute at an altar in sight, else recall once, then walk the altar route.
- `Bot::state_camp_attack` (state_camp_attack.cpp) calls, in order: `camp_wait_for_respawn` (change 11), `camp_after_capture`, `camp_stand_on_reward`,
  `camp_fight_back` (change 12), `camp_hit_group` (full-health rule D6); `Bot::do_return` is the walk home.
- `Bot::state_default` (state_default.cpp) calls `help_allies_hit_camp`, `free_slot_detour` (= `detour_continue` / `detour_start`, change 9),
  `farm_group_step` (capture / switch after a farm capture, D7), then hits the target camp and walks the route.

### turn_end.cpp
- `Bot::note_fuel` - one fuel reading for resume detection. `Bot::finish_turn` - shared memory, trace line, fuel statistics, queued FUEL lines.
- `Bot::log_fuel_line` - one line of the FUEL log (the main line, or a chunk of the per-piece table).

### ability_util.cpp / .h
- `skill_info`, `to_fight_skill`, `fill_design_skills` - ability metadata -> the numbers FIGHT CHECK uses.
- `ability_ready` - is_ready() without the champion object. `cast_skill_at`, `basic_attack_with_reset` (templates in the header), `dash_next_to` (change 8).

### champion_util.cpp, moves.cpp
- `slot_of`, `id_of`, `snapshot_of`, `has_flag`, `type_of_slot`, `design_stats_of_slot`.
- `dist`, `tiles_around`, `blocked_tiles`, `tile_is_blocked`, `move_towards_closest` (+ internal `fixed_obstacles`, `blocked_tiles_with`).

### fight_check.cpp, plan.cpp, target_select.cpp, draft.cpp
- `simulate_fight` (+ `load_units`, `decide_unit`, `apply_turn`, `nearest_enemy`, `scored_enemy`) - the FIGHT CHECK simulation.
- `build_plan` (+ `score_candidates`, `add_targets`, `add_altars`), `trace_path`, `encode_plan`, `decode_plan`, `path_tiles` - the draft plan and its memory format.
- `select_all_targets`, `select_targets` (+ `make_slots`, `sorted_scores`, `pick_two`) - the slot rules for who gets which camp.
- `play_draft`, `analyse_map` - the draft.

## I want to change X - edit Y

| I want to change | edit |
|---|---|
| the gem threshold for banking | `config.h`: `V2_GEM_THRESHOLD` (`cfg::kGemThreshold`) |
| the low-health recall threshold | `config.h`: `V2_LOW_HP_PERCENT` |
| when BANK starts (also on spawn with any gem) | `state_bank.cpp` `bank_wanted`; `config.h` `V2_CHANGE_DEPOSIT_AT_SPAWN` |
| what BANK does (recall, altar route, tribute) | `state_bank.cpp` `state_bank` |
| how long enemies are remembered | `config.h`: `cfg::kMemoryTurns` |
| which enemy is fought (nearest / score / range-equal) | `enemy_memory.cpp` `nearest_enemy`; `config.h` `V2_CHANGE_TARGET_SCORE`, `V2_W_TARGET_*`, `V2_CHANGE_RANGE_EQUAL` |
| how fights are decided (win / lose) | `fight_check.cpp` `simulate_fight` / `decide_unit`; the weight `V2_W_MY_TURNS` in `config.h` |
| what FIGHT CHECK sees (units, stats, cooldowns) | `fight.cpp` `fc_unit_me`, `fc_add_allies`, `fc_add_shared_allies`, `fc_add_enemies`; skill numbers in `ability_util.cpp` `skill_info` |
| what "fight" does (dash, cast, attack order) | `fight.cpp` `fight_enemy`; `ability_util.cpp` `cast_skill_at`, `basic_attack_with_reset`, `dash_next_to` |
| where to flee | `fight.cpp` `flee`; `config.h` `cfg::kEnemyVision` |
| the free-slot detour | `state_default.cpp` `detour_continue` / `detour_start`; limits in `config.h` (`kDetour*`) |
| waiting for a respawn | `state_camp_attack.cpp` `camp_wait_for_respawn`; respawn rounds in `config.h` |
| the full-health rule at an attack camp | `state_camp_attack.cpp` `camp_hit_group`; `cfg::kFullHealthGraceTurns` |
| the walk home after a capture | `state_camp_attack.cpp` `do_return`; `cfg::kReturnNearSpawnSteps`, `cfg::kReturnEnemyRange` |
| the shop order | `shop.cpp` `shop` (the `order` list) |
| which camps each champion targets | `target_select.cpp` (slot rules), `plan.cpp` `score_candidates` (scores; `V2_CHANGE_REWARD_SCORES`) |
| the draft picks | `draft.cpp` `play_draft` (the `picks` list) |
| the order of the states, or a dead / stunned turn | `bot.cpp` `play_turn`, `run_states`, `act_dead`, `cannot_act` |
| the shared-memory layout | `shared_memory.cpp` `write_shared` AND `fight.cpp` `fc_add_shared_allies`; `config.h` `cfg::kShared*` |
| a log line | `grep -n '"NAME '` in `src`: `INIT` bot_setup.cpp, `ENGSTART` fight.cpp, `ENG` enemy_memory.cpp, `SWITCH` targets.cpp, `BANK` state_bank.cpp, `CAMP`/`WAIT` state_camp_attack.cpp and state_default.cpp, `DETOUR` state_default.cpp, `DASH` ability_util.cpp, `SHOP` shop.cpp, `SHARED` shared_memory.cpp, `ERR` bot.cpp, `FUEL` / `TR` turn_end.cpp, `DRAFT` draft.cpp |
| the FUEL log (fields, how often) | `turn_end.cpp` `log_fuel_line`, `finish_turn`; `config.h` `cfg::kFuelLog*` |
| add a switch | `config.h` (a block in the same `#ifndef` / comment / `#define` style, and a row in the table at the top) |
| measure where fuel goes | `tools/fuel_profile.sh` (builds `bots/v2_profile` with `V2_FUEL_PROFILE=1`); `fuel_profile.h` to add a piece |
| prove a change did not alter play | `tools/check_identical.sh check` (4 games, hashes) and the native tests in `tests/` |
