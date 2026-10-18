// main.cpp - the program of every champion and of the draft: one loop, one call per round.
// Reads: whether this is the draft or play. Writes: nothing itself (play_draft / Bot::play_turn do). Edit here to change: nothing normally; this is the
// outer error guard (an exception from a turn is printed as "bot error: ..." and the turn still ends).
// Dispatch draft and gameplay; a failed turn is logged and still yields.
#include <cambc.h>
#include <iostream>
#include "bot.h"
#include "draft.h"

// What: the loop every champion's program runs: one draft or play turn per round, then end_turn().
int main() {
    Bot bot;
    while (true) {
        try {
            if (cambc::is_draft())
                play_draft();
            else
                bot.play_turn();
        } catch (const std::exception& error) {
            std::cout << "bot error: " << error.what() << std::endl;
        } catch (...) {
            std::cout << "bot error: unknown exception" << std::endl;
        }

        cambc::end_turn();
    }
}
