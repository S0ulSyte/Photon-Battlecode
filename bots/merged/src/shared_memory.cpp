// shared_memory.cpp - Publishing my tile and health in the team's 8 shared bytes (CHANGE 13).
//
// Reads: my record. Writes: my two shared-memory bytes.
// Edit here to change: the byte layout (see also fight.cpp, which reads it), how often it is written.
#include <algorithm>
#include <cstdlib>
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

// (CHANGE 13's tile + health layout was here; it is replaced by SHARED TARGETS below and left switched off by V2_CHANGE_SHARED_ALLIES 0.)

// ======================================= SHARED TARGETS (MERGED) =======================================
// Each champion owns ONE byte of the team's shared memory: the byte at its draft slot (0 Issac, 1 Cassi, 2 Vendetta, 3 Ryth, 4 Marcella, 5 Varg).
// The byte says where the thing it is fighting or attacking is, RELATIVE to the champion: 0 = nothing; otherwise high nibble = dx + 8, low nibble =
// dy + 8 with dx and dy each in -7..7 (so a real target is never 0). It is written at the end of every turn, from the champion's final tile; a champion
// that did not engage anything that turn, or is dead, writes 0. A target more than 7 tiles away on an axis is not shared.
// Every champion then looks at the allies in its own vision: an ally's target is that ally's current tile plus the offset in the ally's byte.

// What: remember what I fought or attacked this turn. Inputs: its tile. Side effects: tgt_valid_, tgt_pos_ (cleared at the start of each turn).
void Bot::note_target(vec2 p) {
    tgt_valid_ = true;
    tgt_pos_ = p;
}

// What: publish my byte (see above). Called at the end of every turn, alive or dead. Fuel: ~250.
void Bot::write_shared() {
    FUEL_SCOPE(FP_WRITE_SHARED);
    if (slot_ < 0 || slot_ > 5)
        return;
    std::uint8_t value = 0;
    if (tgt_valid_ && cambc::self().is_alive()) {
        const vec2 here = my_pos();
        const int dx = tgt_pos_.x - here.x, dy = tgt_pos_.y - here.y;
        if (std::abs(dx) <= cfg::kShareMaxOffset && std::abs(dy) <= cfg::kShareMaxOffset)
            value = static_cast<std::uint8_t>(((dx + 8) << 4) | (dy + 8));
    }
    try {
        const std::array<std::byte, 1> bytes{static_cast<std::byte>(value)};
        cambc::shared_memory.write(static_cast<std::size_t>(slot_), std::span<const std::byte>(bytes));
    } catch (const std::exception& error) {
        if (!shared_error_logged_) {
            shared_error_logged_ = true;
            LogLine log;
            log.s("SHARED ").s(name_).s(" r").n(round_).s(" write failed: ").s(error.what());
            log.flush();
        }
    }
}

// What: if an ally in my vision has published a target, go toward it and attack it if I can. Overridden (returns false, normal behaviour) only below
// cfg::kFollowMinHpPercent of my health. Several allies: the one whose target is nearest to me. A target tile with nothing on it that I am already next to
// is ignored (the fight is over). Inputs: none. Result: true if this turn was spent on it. Side effects: moves, attacks, my own published target.
// Fuel: ~1,500 with no ally in sight, ~3,000 with one.
bool Bot::follow_ally_target() {
    if (slot_ < 0 || slot_ > 5)
        return false;
    auto& me = cambc::self();
    const auto record = snapshot_of(me);
    if (record.health * 100.0f < record.max_health * cfg::kFollowMinHpPercent)
        return false;
    const auto& allies = ally_view();
    if (allies.size() <= 1)
        return false;  // nobody but me in sight
    std::array<std::byte, 8> raw{};
    try {
        cambc::shared_memory.read(0, std::span<std::byte>(raw));
    } catch (const std::exception&) {
        return false;
    }
    const vec2 here = my_pos();
    bool found = false;
    vec2 target{0, 0};
    int best = 1 << 30;
    for (const auto& a : allies) {
        if (a == me || !a.is_alive())
            continue;
        const int s = const_of(a).slot;
        if (s < 0 || s > 5)
            continue;
        const unsigned b = static_cast<unsigned>(raw[static_cast<std::size_t>(s)]);
        if (b == 0)
            continue;
        const vec2 t{a.position().x + static_cast<int>(b >> 4) - 8, a.position().y + static_cast<int>(b & 15) - 8};
        const int d = dist(here, t);
        if (d < best) {
            best = d;
            target = t;
            found = true;
        }
    }
    if (!found)
        return false;
    where_ = "follow_ally";
    const int reach = std::max(me.attack_range(), 1);
    // A camp on the target tile.
    if (cambc::vision::has_camp(target)) {
        const auto camp = cambc::vision::camp_at(target);
        if (camp.health() > 0) {
            if (dist(my_pos(), target) > reach)
                move_towards_closest(tiles_around(target, reach));
            attack_camp(camp);
            note_target(target);
            return true;
        }
    }
    // An enemy champion at (or next to) the target tile.
    for (const EnemyMem& e : enemies_)
        if (e.visible && dist(e.pos, target) <= 1) {
            fight_enemy(e, false);  // moves toward it, casts, attacks (and publishes it as my target)
            return true;
        }
    // Nothing visible there: walk toward the spot (the thing may be just out of my sight) unless I am already next to it.
    if (dist(here, target) <= 1)
        return false;
    move_towards_closest(tiles_around(target, 1));
    return true;
}
