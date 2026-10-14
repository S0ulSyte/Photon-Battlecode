// draft_wip.h - declares the WORK-IN-PROGRESS draft (see draft_wip.cpp for everything it does).
//
// Nothing calls this yet, on purpose, so the working bot is unchanged. To try it, replace the call to play_draft()
// in main.cpp with play_draft_wip() (and include this header there).

// Include guard: read this file only once per source file.
#pragma once

// Runs one draft turn of the WIP draft: makes this turn's pick, and on the first call also analyses the map
// (shortest paths to camps and altars, farm/combat scores) and stores the result in draft memory.
void play_draft_wip();
