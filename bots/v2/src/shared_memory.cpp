// shared_memory.cpp - Publishing my tile and health in the team's 8 shared bytes (CHANGE 13).
//
// Reads: my record. Writes: my two shared-memory bytes.
// Edit here to change: the byte layout (see also fight.cpp, which reads it), how often it is written.
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

// CHANGE 13. What: publish my tile and health level in my two shared-memory bytes. Why: vision is not shared between champions, so FIGHT CHECK
// cannot see allies out of my sight (hole 8). How: slot s (0-3) owns bytes 2s and 2s+1; value = tile index (12 bits) | health level << 12 (4 bits),
// little endian; a dead champion writes 0 (level 0 = dead). Nobody else writes these bytes and slots 4, 5 write nothing.
// Inputs: none. Side effects: writes my two shared-memory bytes. Fuel: ~220 per turn.
void Bot::write_shared() {
    FUEL_SCOPE(FP_WRITE_SHARED);
#if V2_CHANGE_SHARED_ALLIES
    if (slot_ < 0 || slot_ > 3 || map_w_ * map_h_ > cfg::kSharedMaxTiles)
        return;
    auto& me = cambc::self();
    unsigned value = 0;
    if (me.is_alive()) {
        const vec2 p = my_pos();
        const auto record = snapshot_of(me);  // (refactor: one record for health and max health)
        const int level = std::clamp(static_cast<int>(static_cast<float>(cfg::kSharedHealthLevels) * record.health / std::max(record.max_health, 1.0f) + 0.999f), 1, cfg::kSharedHealthLevels);  // ceil, at least 1 while alive
        value = static_cast<unsigned>(p.y * map_w_ + p.x) | (static_cast<unsigned>(level) << cfg::kSharedTileBits);
    }
    try {
        const std::array<std::byte, 2> bytes{static_cast<std::byte>(value & 255u), static_cast<std::byte>(value >> 8)};
        cambc::shared_memory.write(static_cast<std::size_t>(2 * slot_), std::span<const std::byte>(bytes));
    } catch (const std::exception& error) {
        if (!shared_error_logged_) {
            shared_error_logged_ = true;
            LogLine log;
            log.s("SHARED ").s(name_).s(" r").n(round_).s(" write failed: ").s(error.what());
            log.flush();
        }
    }
#endif
}

