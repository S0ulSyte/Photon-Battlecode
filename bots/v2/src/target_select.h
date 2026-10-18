// target_select.h - decides WHICH CAMPS each champion should go after.
//
// Reads/writes: nothing (pure functions). Edit here to change: nothing; the rules are in target_select.cpp.
//
// You give it the champion you are and the list of camps with their farm and attack scores; it gives back the (up to) two
// camps that champion should target. It works from a shared list of free "slots" on each camp (explained at the top of
// target_select.cpp): champions are served in a fixed order and each uses up slots that the later ones then cannot take.
// Every champion runs the same deterministic calculation on the same data, so each one can work out the WHOLE assignment
// for the team and read off its own part, and no two champions need to talk to each other.

// Include guard: read this file only once per source file.
#pragma once
// The game's SDK: gives us cambc::champion_type (who we are) and cambc::camp_type (wolf, elk, ...).
#include <cambc.h>
// std::array: the fixed-size list returned by select_all_targets.
#include <array>
// std::span: a read-only view of an array or vector without copying it.
#include <span>
// std::vector: a resizable array (the result).
#include <vector>

// One camp the champions may target, together with its two scores.
struct TargetCandidate {
    // Any number the caller chooses to identify this camp, handed back unchanged in the result. (In bot.cpp the natural
    // choice is the camp's row number in the route table, so the result leads straight to that camp's path.)
    int id;
    // What kind of camp it is. Elks matter to the rules (they can take two champions); monoliths are never targeted.
    cambc::camp_type kind;
    // Farm score, 0-255: high = short walk from OUR spawn, so a good camp to farm.
    int farm;
    // Attack (combat) score, 0-255: high = short walk from the ENEMY spawn, so enemies are likely there.
    int combat;
};

// One camp that a champion has been told to target.
struct Target {
    // The id of the camp, exactly as given in TargetCandidate::id.
    int id;
    // true  = chosen because of its FARM score (go farm it);
    // false = chosen because of its ATTACK score (go and fight there).
    bool farming;
};

// Works out the targets for champion `who`.
//   who   - which champion we are. The champions with rules are: Issac, Cassi, Vendetta, Ryth, Marcella and Varg (the draft
//           order of the squad). Any other champion gets an empty result.
//   camps - every camp on the map with its scores.
// Returns up to two targets (fewer only if there are not enough camps left that this champion may take), best first.
// The result is a pure function of the inputs: calling it again with the same arguments gives the same answer.
std::vector<Target> select_targets(cambc::champion_type who, std::span<const TargetCandidate> camps);

// The targets of the whole squad in one pass. Index: 0 Issac, 1 Cassi, 2 Vendetta, 3 Ryth, 4 Marcella, 5 Varg.
std::array<std::vector<Target>, 6> select_all_targets(std::span<const TargetCandidate> camps);
