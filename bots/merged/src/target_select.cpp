// target_select.cpp - decides which camps each champion targets, using a shared list of free "slots".
//
// Reads: the scored camps handed in by plan.cpp. Writes: the list of targets per champion slot.
// Edit here to change: the slot rules (monolith 0, elk 2, others 1), which champion calls which picking function (select_all_targets), tie-breaking.
//
// ---------------------------------------------------------------------------------------------------------------------
// THE IDEA
// Every camp has two scores: a FARM score (close to our spawn) and an ATTACK score (close to the enemy spawn). Both scores
// of every camp go into ONE list, sorted from the highest score to the lowest. An entry of that list means "camp X, farm
// score" or "camp X, attack score". Choosing an entry means: target that camp, for farming (a farm entry) or for fighting
// (an attack entry).
//
// SLOTS
// At the start we make a list that says, for every camp, how many champions may still go to it, called its SLOTS:
//   monolith = 0 slots (never targeted),  elk = 2 slots,  every other camp = 1 slot.
// Choosing a camp uses up one of its slots. A camp with no slot left cannot be chosen any more.
//
// TWO PICKING FUNCTIONS (each one picks for ONE champion, changes the slot list, and returns the 2 camps it chose)
//   issac_cassi_pick   doubles every FARM score, then takes the 2 highest-scoring entries whose camp still has a free slot.
//   other_pick         leaves the scores as they are, then takes the 2 highest-scoring entries whose camp still has a free slot.
//
// WHICH CALLS EACH CHAMPION MAKES (the champions are served in the order Issac, Cassi, Vendetta, Ryth, Marcella, Varg, and
// each call of a picking function stands for one champion, so a champion sees the slots used by everyone before it)
//   Issac     issac_cassi_pick once.                                              Keep that result.
//   Cassi     issac_cassi_pick twice.                                             Keep the second result.
//   Vendetta  issac_cassi_pick twice, then other_pick once.                       Keep the other_pick result.
//   Ryth      issac_cassi_pick twice, other_pick once (for Vendetta), then once more.         Keep the last one.
//   Marcella  issac_cassi_pick twice, other_pick twice (Vendetta, Ryth), then once more.      Keep the last one.
//   Varg      issac_cassi_pick twice, other_pick three times (Vendetta, Ryth, Marcella), then once more. Keep the last one.
//   In words: after the two issac_cassi_pick calls, a champion calls other_pick once for every champion (other than Issac and
//   Cassi) picked before it in the draft, and then one more time for itself.
//
// CHOICES I HAD TO MAKE WHERE THE DESCRIPTION WAS NOT EXPLICIT (all easy to change below)
//   1. One call returns 2 DIFFERENT camps. If a camp's farm entry and attack entry are both near the top, only the first of
//      them is used by that call (the other is skipped, and no slot is used for it).
//   2. Ties in score are broken by camp order (the order the camps were given in); a camp's farm entry comes before its
//      attack entry. This keeps the result deterministic.
//   3. Entries with a score of 0 (a camp that cannot be reached) are ignored.
//   4. If fewer than 2 camps have a free slot, a call returns fewer than 2 camps.
// ---------------------------------------------------------------------------------------------------------------------

// Our own declarations (TargetCandidate, Target, select_targets).
#include "target_select.h"
// std::sort.
#include <algorithm>
// std::size_t.
#include <cstddef>

// An anonymous namespace: everything inside is private to this file, so names cannot clash with other files.
namespace {

// How many camps one call of a picking function chooses.
constexpr std::size_t camps_per_call = 2;

// One line of the sorted score list: a camp together with ONE of its two scores.
struct ScoreEntry {
    // Which camp: its position in the list of candidates we were given.
    std::size_t camp;
    // true = this is the camp's farm score; false = this is its attack (combat) score.
    bool farming;
    // The score itself (already doubled for the champions whose rule says so).
    int score;
};

// Makes the slot list: one number per camp = how many more champions may go to that camp.
std::vector<int> make_slots(std::span<const TargetCandidate> camps) {
    // One number per camp, in the same order as `camps`.
    std::vector<int> slots(camps.size());
    // Fill it in camp by camp.
    for (std::size_t i = 0; i < camps.size(); ++i) {
        // MERGED: a camp whose strength the draft stored has that many slots: one champion per point of difficulty.
        if (camps[i].strength >= 0) {
            slots[i] = camps[i].strength;
            continue;
        }
        // Monoliths are never targeted, so they have no slots at all.
        if (camps[i].kind == cambc::camp_type::monolith)
            slots[i] = 0;
        // Elks are the big camps that take two champions.
        else if (camps[i].kind == cambc::camp_type::elk)
            slots[i] = 2;
        // Every other camp (wolf, gem cluster, gold cluster) takes one champion.
        else
            slots[i] = 1;
    }
    // Hand the finished list back.
    return slots;
}

// Builds the sorted list of all scores: two entries per camp (farm and attack), highest first.
// double_farm = true doubles every farm score (this is what Issac and Cassi do).
std::vector<ScoreEntry> sorted_scores(std::span<const TargetCandidate> camps, bool double_farm) {
    // The list we build.
    std::vector<ScoreEntry> list;
    // Look at every camp.
    for (std::size_t i = 0; i < camps.size(); ++i) {
        // The farm entry, doubled if asked. A score of 0 means the camp cannot be reached, so leave it out.
        const int farm = camps[i].farm * (double_farm ? 2 : 1);
        if (farm > 0)
            list.push_back({i, true, farm});
        // The attack entry. Again a score of 0 is left out.
        if (camps[i].combat > 0)
            list.push_back({i, false, camps[i].combat});
    }
    // Sort the highest score first. Ties: the lower camp number first, then the farm entry before the attack entry.
    std::sort(list.begin(), list.end(), [](const ScoreEntry& a, const ScoreEntry& b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.camp != b.camp)
            return a.camp < b.camp;
        return a.farming && !b.farming;
    });
    // Hand the sorted list back.
    return list;
}

// The shared work of both picking functions: go down the sorted list `list` and take the first 2 entries whose camp still has
// a free slot, using up one slot of each camp taken. Returns the 2 camps taken (fewer if there are not enough free slots).
// The list is passed in (already sorted) so that it is built only once per kind of call, not once per champion: sorting costs
// fuel, and a champion's first turn has little of it.
std::vector<Target> pick_two(std::span<const TargetCandidate> camps, std::vector<int>& slots, const std::vector<ScoreEntry>& list) {
    // The camps this call has taken so far.
    std::vector<Target> picked;
    // Walk down the sorted list from the highest score.
    for (const ScoreEntry& e : list) {
        // Stop as soon as we have 2 camps.
        if (picked.size() >= camps_per_call)
            break;
        // No free slot left on this camp? Go on to the next entry ("one camp over").
        if (slots[e.camp] <= 0)
            continue;
        // Did this call already take this camp through its other score? Then skip it: a call returns 2 DIFFERENT camps.
        bool already = false;
        for (const Target& t : picked)
            if (t.id == camps[e.camp].id)
                already = true;
        if (already)
            continue;
        // Take it: use up one slot of the camp...
        --slots[e.camp];
        // ...and record which camp and whether it was chosen for farming or for attacking.
        picked.push_back({camps[e.camp].id, e.farming});
    }
    // Hand back the camps taken by this call.
    return picked;
}

// ISSAC and CASSI: take the 2 highest-scoring entries that have a free slot. Changes `slots`.
// `doubled_list` is the sorted score list in which every farm score has been DOUBLED (sorted_scores(camps, true)).
std::vector<Target> issac_cassi_pick(std::span<const TargetCandidate> camps, std::vector<int>& slots,
                                     const std::vector<ScoreEntry>& doubled_list) {
    return pick_two(camps, slots, doubled_list);
}

// EVERYONE ELSE: take the 2 highest-scoring entries that have a free slot. Changes `slots`.
// `plain_list` is the sorted score list with the scores as they are (sorted_scores(camps, false)).
std::vector<Target> other_pick(std::span<const TargetCandidate> camps, std::vector<int>& slots,
                               const std::vector<ScoreEntry>& plain_list) {
    return pick_two(camps, slots, plain_list);
}

}  // namespace

// The whole squad's answer in one pass: the runs are done once, in order, instead of replaying them for every champion.
// Result index: 0 Issac, 1 Cassi, 2 Vendetta, 3 Ryth, 4 Marcella, 5 Varg. (Used by the draft, which stores everyone's targets.)
std::array<std::vector<Target>, 6> select_all_targets(std::span<const TargetCandidate> camps) {
    std::array<std::vector<Target>, 6> all;
    std::vector<int> slots = make_slots(camps);
    const std::vector<ScoreEntry> doubled = sorted_scores(camps, true);
    const std::vector<ScoreEntry> plain = sorted_scores(camps, false);
    all[0] = issac_cassi_pick(camps, slots, doubled);
    all[1] = issac_cassi_pick(camps, slots, doubled);
    for (std::size_t i = 2; i < 6; ++i)
        all[i] = other_pick(camps, slots, plain);
    return all;
}

// The public function; see target_select.h.
std::vector<Target> select_targets(cambc::champion_type who, std::span<const TargetCandidate> camps) {
    // How many champions other than Issac and Cassi were picked BEFORE this one in the draft
    // (Vendetta has 0 before it, Ryth 1, Marcella 2, Varg 3).
    int others_before = 0;
    // Work out who we are.
    switch (who) {
    case cambc::champion_type::issac: {
        // Issac: one issac_cassi_pick, keep it.
        std::vector<int> slots = make_slots(camps);
        return issac_cassi_pick(camps, slots, sorted_scores(camps, true));
    }
    case cambc::champion_type::cassi: {
        // Cassi: two issac_cassi_picks (the first one is Issac's), keep the second. The sorted list is built once and reused.
        std::vector<int> slots = make_slots(camps);
        const std::vector<ScoreEntry> doubled = sorted_scores(camps, true);
        issac_cassi_pick(camps, slots, doubled);
        return issac_cassi_pick(camps, slots, doubled);
    }
    case cambc::champion_type::vendetta:
        others_before = 0;
        break;
    case cambc::champion_type::ryth:
        others_before = 1;
        break;
    case cambc::champion_type::marcella:
        others_before = 2;
        break;
    case cambc::champion_type::varg:
        others_before = 3;
        break;
    default:
        // A champion without a rule gets no targets.
        return {};
    }

    // Vendetta, Ryth, Marcella and Varg: first replay the two issac_cassi_picks (Issac's and Cassi's), so their slots are used.
    std::vector<int> slots = make_slots(camps);
    const std::vector<ScoreEntry> doubled = sorted_scores(camps, true);
    issac_cassi_pick(camps, slots, doubled);
    issac_cassi_pick(camps, slots, doubled);
    // Then replay the other_pick of every champion (other than Issac and Cassi) picked before us. Their list is sorted once.
    const std::vector<ScoreEntry> plain = sorted_scores(camps, false);
    for (int i = 0; i < others_before; ++i)
        other_pick(camps, slots, plain);
    // Finally our own other_pick; its 2 camps are the answer.
    return other_pick(camps, slots, plain);
}
