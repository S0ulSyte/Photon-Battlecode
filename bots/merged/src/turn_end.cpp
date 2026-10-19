// turn_end.cpp - End-of-turn bookkeeping: fuel statistics, resume detection, the trace line and the FUEL log.
//
// Reads: the fuel counter, acts_, state_fuel_max_. Writes: max_fuel_used_, total_fuel_used_, resumes_, FUEL / TR log lines.
// Edit here to change: the FUEL log format (log_fuel_line), how often it is written, the per-turn trace.
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

// What: one reading of the fuel counter for resume detection (see bot.h). Why: a suspended turn is invisible otherwise and silently changes the game.
// How: the counter only falls inside one budget; a reading above the lowest earlier one means the budget was refilled. Costs about 5 fuel.
// Side effects: may count a resume. Fuel: ~5.
void Bot::note_fuel() {
    const std::uint64_t now = cambc::fuel_remaining();
    if (now > fuel_floor_) {
        ++resumes_;
        last_resume_round_ = round_;
    }
    if (now < fuel_floor_)
        fuel_floor_ = now;
}

// What: end-of-turn bookkeeping: shared memory, the optional trace line, fuel statistics, and (rarely, and only when fuel is plentiful) one FUEL log line.
// Called from every exit of play_turn. Fuel: ~300 on a normal turn; a FUEL log line adds up to ~15,000 on the turn it is written.
void Bot::finish_turn() {
    note_fuel();
#if V2_FUEL_PROFILE
    {
        const std::uint64_t pa = cambc::fuel_remaining(), pb = cambc::fuel_remaining();  // cost of one fuel_remaining() call
        fuel_probe_ = std::max<std::uint64_t>(fuel_probe_, pa - pb);
    }
#endif
    write_shared();  // (change 13) every turn ends here, alive or dead, so the bytes are always fresh
#if V2_TRACE_SLOT >= 0
    if (slot_ == V2_TRACE_SLOT && round_ >= V2_TRACE_FROM && round_ <= V2_TRACE_TO) {
        static const char* const kinds[7] = {"fight", "flee", "campretreat", "bank", "campattack", "default", "channel"};
        const char* act = "none";
        for (int k = 0; k < 7; ++k)
            if (acts_[k] != act_before_[k])
                act = kinds[k];
        auto& me = cambc::self();
        LogLine log;
        log.s("TR ").s(name_).s(" r").n(round_).s(" ").xy(my_pos().x, my_pos().y).s(" hp ").n(static_cast<long long>(me.health())).s(" gems ")
            .n(me.gem_count()).s(" act ").s(act).s(" tgt ").n(static_cast<long long>(target_idx_)).s(returning_ ? " RETURNING" : "").s(capturing_ ? " CAPTURING" : "")
            .s(" enemies ").n(static_cast<long long>(enemies_.size()));
        log.flush();
    }
#endif
    const std::uint64_t left = cambc::fuel_remaining();
    if (left > fuel_start_) {
        // The fuel went UP during the turn: the bot ran dry, was paused, and resumed on a later turn with a fresh budget.
        ++overruns_;
    } else if (turns_played_ == 0) {
        first_turn_fuel_ = fuel_start_ - left;  // the first turn of play has a doubled budget
    } else {
        max_fuel_used_ = std::max(max_fuel_used_, fuel_start_ - left);
        total_fuel_used_ += fuel_start_ - left;  // (refactor) running total over the turns that count (first turn of play and overruns excluded)
        ++fuel_turns_;
    }
    ++turns_played_;
#if V2_FUEL_PROFILE
    if (turns_played_ > 1 && fuel_start_ > left && fuel_start_ - left > cfg::kHeavyTurnFuel && cambc::fuel_remaining() > cfg::kHeavyLogMinLeft) {
        // (profile) a heavy turn: list the pieces that used more than 2,000 fuel in it (inclusive numbers, children are inside parents)
        LogLine log;
        log.s("FUEL heavy ").s(name_).s(" r").n(round_).s(" used ").n(static_cast<long long>(fuel_start_ - left));
        for (int k = 0; k < FP_COUNT; ++k)
            if (g_fuel_stats[k].total - turn_start_totals_[k] > 2000)
                log.s(" ").s(kFuelPieceNames[k]).s(" ").n(static_cast<long long>(g_fuel_stats[k].total - turn_start_totals_[k]));
        log.flush();
    }
#endif
    // (refactor) Logging costs about 25 fuel per character, and a log line written at the end of a heavy turn can push it over the 100,000 limit
    // (that stalls the champion and changes the game). So the FUEL lines are queued every 50 rounds (was 250) and written ONE LINE PER TURN, only
    // on a turn with at least kFuelLogMinLeft fuel left.
    if (fuel_log_pending_ == 0 && round_ - last_fuel_log_ >= cfg::kFuelLogEveryRounds) {
        last_fuel_log_ = round_;
        fuel_log_pending_ = V2_FUEL_PROFILE ? 1 + (FP_COUNT + cfg::kFuelPiecesPerLine - 1) / cfg::kFuelPiecesPerLine : 1;  // the main FUEL line, then the piece table in chunks of 12
    }
    if (fuel_log_pending_ > 0 && cambc::fuel_remaining() >= cfg::kFuelLogMinLeft) {
        FUEL_SCOPE(FP_FINISH_LOG);
        const int total_lines = V2_FUEL_PROFILE ? 1 + (FP_COUNT + cfg::kFuelPiecesPerLine - 1) / cfg::kFuelPiecesPerLine : 1;
        log_fuel_line(total_lines - fuel_log_pending_);
        --fuel_log_pending_;
    }
}

// What: write one line of the FUEL log: step 0 is the main `FUEL` line (the fields the analyzer reads), steps 1.. are chunks of the per-piece
// table (`FUEL pieces`, see fuel_profile.h). Why: finish_turn writes at most one per turn (logging is expensive). How: LogLine, as everywhere.
// Inputs: 0 = the main FUEL line, 1.. = chunks of the per-piece table (profile builds). Side effects: one log line. Fuel: ~8,000 to 15,000.
void Bot::log_fuel_line(int step) {
    if (step == 0) {
        LogLine log;
        log.s("FUEL ").s(name_).s(" r").n(round_).s(" max_used ").n(static_cast<long long>(max_fuel_used_)).s(" overruns ").n(overruns_)
            .s(" first_turn ").n(static_cast<long long>(first_turn_fuel_)).s(" per_state s1 ").n(static_cast<long long>(state_fuel_max_[0]))
            .s(" s2 ").n(static_cast<long long>(state_fuel_max_[1])).s(" s3 ").n(static_cast<long long>(state_fuel_max_[2]))
            .s(" s5 ").n(static_cast<long long>(state_fuel_max_[3])).s(" pre ").n(static_cast<long long>(state_fuel_max_[5]))
            .s(" upd_enemies ").n(static_cast<long long>(state_fuel_max_[6])).s(" upd_camps ").n(static_cast<long long>(state_fuel_max_[7]))
            .s(" ignored_enemy ").n(ignored_enemy_turns_).s(" acts fight ").n(acts_[0]).s(" flee ").n(acts_[1]).s(" campretreat ").n(acts_[2])
            .s(" bank ").n(acts_[3]).s(" campatk ").n(acts_[4]).s(" default ").n(acts_[5]).s(" channel ").n(acts_[6])
            .s(" upd_eng ").n(static_cast<long long>(state_fuel_max_[8])).s(" sim ").n(static_cast<long long>(state_fuel_max_[9]))
            .s(" before_sim ").n(static_cast<long long>(state_fuel_max_[10])).s(" grp_state ").n(static_cast<long long>(state_fuel_max_[11]))
            .s(" grp_calls ").n(group_state_calls_).s(" grp_hits ").n(group_state_hits_)
            .s(" total_used ").n(static_cast<long long>(total_fuel_used_)).s(" turns ").n(fuel_turns_)
            .s(" resumes ").n(resumes_).s(" last_resume_round ").n(last_resume_round_);
        log.flush();
        return;
    }
#if V2_FUEL_PROFILE
    // Chunk number step-1 of the piece table, twelve pieces per line (a log line holds about 600 characters).
    const int first = (step - 1) * cfg::kFuelPiecesPerLine;
    LogLine log;
    log.s("FUEL pieces ").s(name_).s(" r").n(round_);
    if (first == 0)
        log.s(" probe ").n(static_cast<long long>(fuel_probe_));
    for (int k = first; k < FP_COUNT && k < first + cfg::kFuelPiecesPerLine; ++k)
        log.s(" ").s(kFuelPieceNames[k]).s(" ").n(static_cast<long long>(g_fuel_stats[k].peak)).s("/").n(static_cast<long long>(g_fuel_stats[k].total))
            .s("/").n(g_fuel_stats[k].calls);
    log.flush();
#endif
}

