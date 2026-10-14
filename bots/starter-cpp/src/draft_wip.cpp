// draft_wip.cpp - WORK-IN-PROGRESS draft. Self-contained: it needs no other new file, so the rest of the bot is untouched.
//
// FUEL IT TOOK TO RUN (measured, one run on collision.map26a which is 41x41 tiles, with the per-turn fuel limit raised to
// 1,000,000,000 so that nothing could be cut off; the first draft turn then has 10x that = 10,000,000,000):
//     flood from OUR spawn ........................ 184,540
//     flood from the ENEMY spawn .................. 184,540   (identical because this map is mirror-symmetric)
//     whole analysis ............................... 537,922   (both floods + scoring + packing the memory bytes; printing
//                                                                the report is NOT included)
//     fuel left in that turn after the analysis .... 9,999,441,337 of 10,000,000,000
// For scale: with the NORMAL limit (100,000 per turn, 10x on the first draft turn) the first draft turn has about 1,000,000
// fuel, so this analysis uses a little over half of it. An earlier, slower version of this search (a separate bfs.cpp, since deleted) needed 777,033 for ONE flood on this same map.
// These numbers grow with the size of the map, roughly in proportion to the number of tiles (estimate, not measured: a
// 100x100 map has about 5-6 times as many tiles, so each flood would cost around 1,000,000, which is why GUARD 1 and
// GUARD 2 in analyse_and_publish() skip the analysis when there is not enough fuel).
//
// What play_draft_wip() does, in order, on every draft turn:
//   0. (turns after the first only) SUBMIT the draft memory that the previous turn prepared. This has to happen BEFORE the
//      pick of the same turn, because the game rejects a draft-memory write once the turn's pick has been made.
//   1. PICK: Cassi & Issac (one pick), then Vendetta, then the highest-health melee champions that are left
//      (Ryth, Marcella, Varg) for however many pick slots remain. The pick happens before any heavy work on purpose: if
//      anything later runs out of fuel, the pick for this turn has already been made (a draft bot that fails to pick gets
//      a RANDOM pick).
//   2. ANALYSE (first draft turn only, because that turn has 10x fuel):
//        a. Flood (BFS) the map from OUR spawn tiles  -> shortest walking route to every camp and altar.
//        b. Flood (BFS) the map from the ENEMY spawn tiles -> how far every camp is from the enemy.
//        c. FARM score  (0-255) per camp: high when the camp is SHORT to reach from our spawn.
//           COMBAT score (0-255) per camp: high when the camp is SHORT to reach from the enemy spawn (so enemies will
//           probably be there, and a fight is likely).
//        d. Build the routes from our spawn into DRAFT MEMORY bytes (the 1,024-byte buffer that is read-only for the
//           champions during the match; the other "shared memory" is only 8 bytes, far too small for paths). The bytes are
//           built on the first turn but SUBMITTED in step 0 of the second draft turn.
//        e. Print a short report (scores, fuel used, memory used) to the bot's log.
//
// Vocabulary used below:
//   - "tile"    one square of the map, written (x, y): x = the column (grows towards the east), y = the row (grows towards the north: the SDK defines north as +y).
//   - "flood"   a breadth-first search (BFS): spread outwards from the starting tiles one step at a time, so every tile is
//               first reached by a shortest route.
//   - "fuel"    the game's per-turn computation budget (roughly one unit per CPU instruction). Everything costs fuel, so
//               this file avoids slow constructs (iostream, per-tile calls into the game, bounds checks in the hot loop).
//   - "padded"  the map with an extra ring of blocked tiles around it (explained at struct Grid).
//
// ---------------------------------------------------------------------------------------------------------------------
// DRAFT MEMORY LAYOUT (everything is whole bytes; coordinates are 0-based map tiles, x = column, y = row)
//
//   byte 0      format version (currently 1)
//   byte 1      map width
//   byte 2      map height
//   byte 3      number of entries that follow (E)
//   then E entries back to back. Altars come first, then camps in descending FARM score. Each entry is:
//     byte 0    kind: 0 wolf, 1 elk, 2 gem cluster, 3 gold cluster, 4 monolith (the camp_type numbers), or 255 = altar
//     byte 1    target x   (the camp's tile, or the altar's own tile)
//     byte 2    target y
//     byte 3    farm score   (0-255; always 0 for altars)
//     byte 4    combat score (0-255; always 0 for altars)
//     byte 5    start x    (the spawn tile the route begins on)
//     byte 6    start y
//     byte 7    S = number of steps in the route (0-255)
//     then ceil(S / 2) bytes of steps, two per byte: step 0 in the LOW 4 bits of the first byte, step 1 in the HIGH 4
//     bits, step 2 in the low bits of the next byte, and so on. Each step is a direction code 0-7:
//         0 = (-1,-1)  1 = (0,-1)  2 = (+1,-1)
//         3 = (-1, 0)              4 = (+1, 0)
//         5 = (-1,+1)  6 = (0,+1)  7 = (+1,+1)        written as (dx,dy); x grows east and y grows north (the SDK defines north as +y), so code 1 = (0,-1) is a step south.
//     To rebuild the route: start at (start x, start y) and add each step's (dx,dy) in order. For an altar the route
//     ends on the tile you stand on to use it (next to the altar), not on the altar itself.
//   Entries with no walkable route are left out. If memory fills up, the lowest-priority entries are dropped.
// ---------------------------------------------------------------------------------------------------------------------

// Our own declaration (play_draft_wip). Including our own header also lets the compiler check the two agree.
#include "draft_wip.h"
// The game's SDK: positions, the map, drafting, fuel_remaining(), draft memory and so on.
#include <cambc.h>
// std::reverse (flip a list), std::stable_sort (sort keeping the order of equal items), std::min, std::max.
#include <algorithm>
// std::array: fixed-size array whose size is known at compile time (used for the direction tables and the pick list).
#include <array>
// Fixed-size integer types such as std::uint8_t (one byte) and std::int64_t (64-bit signed), the same on every machine.
#include <cstdint>
// std::fwrite and std::fflush: write raw bytes to the bot's log and push them out immediately.
#include <cstdio>
// std::unique_ptr and std::make_unique_for_overwrite: memory that frees itself when it goes out of scope.
#include <memory>
// std::move: hand an object over without copying it (used when adding entries to the list).
#include <utility>
// std::vector: a resizable array.
#include <vector>

// An anonymous namespace: everything inside is private to this file, so names cannot clash with other files.
namespace {

// ---- Constants ------------------------------------------------------------------------------------------------------

// A tile has 8 neighbours (4 straight, 4 diagonal), because champions can move in all 8 directions.
// A named constant instead of the bare number 8 so every loop and array below visibly belongs together.
constexpr int direction_count = 8;
// Value stored in dir[] for a starting tile: "this tile has no tile before it". Real direction codes are 0-7, so 255 can
// never be confused with one. route_to() stops walking backwards when it sees this value.
constexpr std::uint8_t root_code = 255;
// Kind byte used in the memory layout for an altar (camps use their camp_type number 0-4, so 255 cannot clash).
constexpr std::uint8_t altar_kind = 255;
// Version number written to byte 0 of draft memory, so a reader can tell if the layout ever changes.
constexpr std::uint8_t format_version = 1;
// Fixed bytes at the start of each entry (kind, x, y, farm, combat, start x, start y, step count) = 8, as in the layout above.
constexpr std::size_t entry_header_bytes = 8;
// The best possible score. The nearest camp gets this; others get less. 255 because a score is stored in ONE byte.
constexpr int max_score = 255;
// Rough guess of fuel one flood costs PER TILE of the padded grid. Only used to decide whether there is enough fuel to
// start; the real cost is measured and printed. A wrong guess here cannot break anything: it only makes us skip too early or
// too late, and the second guard below re-checks using the MEASURED cost of the first flood.
constexpr std::uint64_t fuel_per_tile_estimate = 150;
// Fuel we refuse to touch: it pays for tracing routes, packing memory and printing after the floods are done.
// 150'000 is just 150000 with digit separators (a C++ readability feature).
constexpr std::uint64_t fuel_reserve = 150'000;

// The 8 directions as {dx, dy}. The INDEX in this table is the direction code stored in draft memory (see the layout above),
// so the order here is part of the stored format: do not reorder it without bumping format_version.
// The extra pair of braces is how C++ initialises an array that holds arrays.
constexpr std::array<std::array<int, 2>, direction_count> direction_deltas{
    {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}}};

// ---- The map as a flat array --------------------------------------------------------------------------------------

// The map copied into plain arrays, with an extra ring of blocked tiles all around it.
// Why the ring ("padding"): when the search looks at the 8 neighbours of a tile on the map's edge, the neighbour might
// be off the map. With a blocked ring those neighbours simply exist and are blocked, so the search needs NO "is this
// inside the map?" checks, which makes every neighbour test several times cheaper in fuel.
struct Grid {
    // Real map size in tiles (number of columns).
    int width = 0;
    // Real map size in tiles (number of rows).
    int height = 0;
    // Size including the ring: one extra column on each side, so width + 2.
    int padded_width = 0;
    // Size including the ring: one extra row on top and bottom, so height + 2.
    int padded_height = 0;
    // terrain[i] == 1 means padded tile i can never be walked on (the ring, walls, holes, altars). 0 = open floor.
    // One byte per tile (not one bit) because reading a byte is a single cheap instruction.
    std::vector<std::uint8_t> terrain;
    // How much to ADD to a padded index to move one step in direction k. Filled in make_grid().
    // Example: moving "right" is +1, moving "down" is +padded_width, so "down-right" is padded_width + 1.
    std::array<int, direction_count> offsets{};

    // Real tile (x, y) -> padded flat index. The +1s skip the ring; row * width + column makes it one number.
    // Why one number: a flat array indexed by one integer is the cheapest structure to read and write.
    int index(cambc::vec2 p) const { return (p.y + 1) * padded_width + (p.x + 1); }
    // Padded flat index -> real tile (x, y): the reverse of index(). % gives the column, / gives the row; -1 removes the ring.
    cambc::vec2 tile(int i) const { return {i % padded_width - 1, i / padded_width - 1}; }
};

// Builds the Grid from the game's map data. Costs a few calls into the game but is only done once.
Grid make_grid() {
    // The object we fill in and return.
    Grid grid;
    // Real map width, from the game engine.
    grid.width = cambc::map::width();
    // Real map height, from the game engine.
    grid.height = cambc::map::height();
    // Width with the blocked ring around it.
    grid.padded_width = grid.width + 2;
    // Height with the blocked ring around it.
    grid.padded_height = grid.height + 2;
    // One byte per padded tile, all 0 (open) to start. assign() fills the whole vector with that value.
    // static_cast to size_t first so the multiplication cannot overflow a plain int on a huge map.
    grid.terrain.assign(static_cast<std::size_t>(grid.padded_width) * grid.padded_height, 0);

    // Block the ring: the top and bottom rows, one column at a time...
    for (int x = 0; x < grid.padded_width; ++x) {
        // Top row (row 0): mark this column blocked.
        grid.terrain[x] = 1;
        // Bottom row (row padded_height - 1): same column, blocked.
        grid.terrain[(grid.padded_height - 1) * grid.padded_width + x] = 1;
    }
    // ...and the left and right columns, one row at a time.
    for (int y = 0; y < grid.padded_height; ++y) {
        // Left column (column 0) of this row: blocked.
        grid.terrain[y * grid.padded_width] = 1;
        // Right column (column padded_width - 1) of this row: blocked.
        grid.terrain[y * grid.padded_width + grid.padded_width - 1] = 1;
    }

    // Walls: ONE call into the game gives every wall tile; copy each into our array. (Asking tile by tile would cost far more.)
    for (cambc::vec2 p : cambc::map::walls().positions())
        // Convert the real tile to a padded index and mark it blocked.
        grid.terrain[grid.index(p)] = 1;
    // Holes: you can see over them but not stand on them, so for walking they block too.
    for (cambc::vec2 p : cambc::map::holes().positions())
        // Same as for walls: mark it blocked.
        grid.terrain[grid.index(p)] = 1;
    // Altars: nobody can step onto an altar tile, so routes must not pass through them.
    for (cambc::vec2 p : cambc::map::altars().positions())
        // Same again: mark it blocked.
        grid.terrain[grid.index(p)] = 1;

    // Turn each direction's {dx, dy} into a single number to add to a flat index: dy rows down = dy * padded_width.
    for (int k = 0; k < direction_count; ++k)
        // [1] is dy and [0] is dx in the {dx, dy} pair; combine them into one offset.
        grid.offsets[k] = direction_deltas[k][1] * grid.padded_width + direction_deltas[k][0];
    // Done.
    return grid;
}

// ---- The flood (breadth-first search) ------------------------------------------------------------------------------

// What one flood leaves behind. Both arrays are indexed by padded tile index.
struct Flood {
    // dist1[i] = (number of steps from the nearest start tile to tile i) + 1. 0 means "never reached".
    // The +1 lets 0 mean "unreached", and zero-filled memory is the cheapest kind to create.
    std::vector<std::uint16_t> dist1;
    // dir[i] = the direction code (0-7) of the LAST step taken to arrive at tile i, or root_code for a start tile.
    // This is the "where did I come from" information: the tile before i is  i - offsets[dir[i]].
    std::vector<std::uint8_t> dir;
    // Fuel this flood cost (measured), or -1 if it could not be measured (bot paused for a new turn) or it was skipped.
    // Signed so that -1 can mean "unknown".
    std::int64_t fuel = -1;
};

// Floods the grid from all `starts` at once (multi-source BFS): every tile is reached by a shortest walk from its nearest
// start, and the tiles are processed in order of distance, so the first arrival at a tile is always a shortest one.
Flood flood(const Grid& grid, const std::vector<cambc::vec2>& starts) {
    // The object we fill in and return.
    Flood result;
    // Start of the fuel stopwatch: how much fuel is left right now.
    const std::uint64_t fuel_before = cambc::fuel_remaining();
    // Number of padded tiles; every array below has this many slots.
    const std::size_t cells = grid.terrain.size();
    // Distances: all 0 = nothing reached yet. (Zero fill is a single fast memory operation.)
    result.dist1.assign(cells, 0);
    // Directions: all 0 to start; a slot only has meaning once its tile is reached.
    result.dir.assign(cells, 0);
    // `closed[i]` = 1 means "do not enter tile i": it starts as a copy of the terrain and ALSO gets set for every tile we
    // reach, so one array answers both "is it blocked?" and "have I been here already?" with a single read.
    std::vector<std::uint8_t> closed = grid.terrain;
    // The BFS queue as a plain array with a write position `tail`. Sized for every tile, and each tile enters at most once,
    // so it can never overflow, and writing queue[tail++] is cheaper than vector::push_back (no capacity check).
    std::vector<std::int32_t> queue(cells);
    // Next free slot in the queue (also = how many tiles have been queued so far).
    std::size_t tail = 0;

    // Seed the queue with every start tile.
    for (cambc::vec2 start : starts) {
        // Padded index of this start tile.
        const int i = grid.index(start);
        // Skip it if it is blocked or already queued (two starts can be the same tile).
        if (closed[i])
            continue;
        // Mark it reached so it is never queued again.
        closed[i] = 1;
        // It has no tile before it: mark it as a root so route_to() knows where to stop.
        result.dir[i] = root_code;
        // Distance 0 steps, stored as 0 + 1.
        result.dist1[i] = 1;
        // Put it in the queue and advance the write position.
        queue[tail++] = i;
    }

    // Take tiles out of the queue in the order they went in. `head` is the next tile to take out; the queue grows as we go,
    // which is why the loop re-checks `tail` every time. Taking tiles out in FIFO order is what makes this a BFS: nearer
    // tiles are always processed before farther ones.
    for (std::size_t head = 0; head < tail; ++head) {
        // The tile we are exploring now.
        const int here = queue[head];
        // Every neighbour we discover from here is one step further away than `here`.
        const std::uint16_t next_dist = static_cast<std::uint16_t>(result.dist1[here] + 1);
        // Try each of the 8 directions.
        for (int k = 0; k < direction_count; ++k) {
            // The neighbour's padded index: just add the direction's offset. No bounds check needed thanks to the ring.
            const int next = here + grid.offsets[k];
            // Blocked or already reached? Then ignore it (an earlier arrival was at least as short).
            if (closed[next])
                continue;
            // First arrival at this neighbour: mark it reached...
            closed[next] = 1;
            // ...remember the step we took to get there (so routes can be rebuilt)...
            result.dir[next] = static_cast<std::uint8_t>(k);
            // ...remember how far it is...
            result.dist1[next] = next_dist;
            // ...and queue it so ITS neighbours get explored later.
            queue[tail++] = next;
        }
    }

    // End of the fuel stopwatch: how much fuel is left now?
    const std::uint64_t fuel_after = cambc::fuel_remaining();
    // Fuel only goes down within a turn, so cost = before - after. A bigger value than at the start means the bot was
    // paused and resumed with a fresh budget and the cost can no longer be known, so we store -1.
    result.fuel = fuel_after <= fuel_before ? static_cast<std::int64_t>(fuel_before - fuel_after) : -1;
    // Hand the result back.
    return result;
}

// A route rebuilt from a flood: the start tile and the direction codes to follow, in walking order.
struct Route {
    // Tile the route starts on (one of the spawn tiles).
    cambc::vec2 start{0, 0};
    // Direction code of each step, first step first.
    std::vector<std::uint8_t> codes;
};

// Rebuilds the route that the flood used to reach padded tile `end` (which must have been reached).
Route route_to(const Grid& grid, const Flood& flood_result, int end) {
    // The object we fill in and return.
    Route route;
    // Walk BACKWARDS from the end tile...
    int i = end;
    // ...until we hit a start tile (the only tiles whose dir is root_code).
    while (flood_result.dir[i] != root_code) {
        // The step that led INTO tile i. Collected backwards, so we flip the list afterwards.
        route.codes.push_back(flood_result.dir[i]);
        // Undo that step: subtracting the offset gives the tile we came from.
        i -= grid.offsets[flood_result.dir[i]];
    }
    // We stopped on the start tile; convert its padded index back to a real (x, y).
    route.start = grid.tile(i);
    // Flip so the first step is first.
    std::reverse(route.codes.begin(), route.codes.end());
    // Hand the route back.
    return route;
}

// The best tile to stand on to use an altar: among the tiles within tribute range of it, the one the flood reached
// soonest. Returns its padded index, or -1 if none of them was reached.
int stand_tile_for(const Grid& grid, const Flood& flood_result, cambc::vec2 altar, int reach) {
    // Best padded index found so far (-1 = none).
    int best = -1;
    // Square of radius `reach` around the altar, clamped so we never leave the real map: rows...
    for (int y = std::max(altar.y - reach, 0); y <= std::min(altar.y + reach, grid.height - 1); ++y) {
        // ...and columns.
        for (int x = std::max(altar.x - reach, 0); x <= std::min(altar.x + reach, grid.width - 1); ++x) {
            // Padded index of this candidate tile.
            const int i = grid.index({x, y});
            // Skip tiles the flood never reached (this also skips the altar tile itself, which is blocked).
            if (flood_result.dist1[i] == 0)
                continue;
            // Keep it if it is the first one found, or closer than the best so far.
            if (best < 0 || flood_result.dist1[i] < flood_result.dist1[best])
                best = i;
        }
    }
    // The winner (or -1).
    return best;
}

// ---- Scores --------------------------------------------------------------------------------------------------------

// Turns a distance into a 0-255 score: 255 for the nearest camp, falling off as 1 / distance (twice as far = half the
// score), 0 if the camp cannot be reached at all.
// Why 1/distance: walking time is proportional to distance, so reward-per-walking-time is proportional to 1/distance.
// Example: if the nearest camp is 5 steps away, a camp 10 steps away scores 255 * 5 / 10 = 127.
// steps         = this camp's distance in steps, or -1 if unreachable.
// nearest_steps = the smallest distance among all reachable camps (at least 1).
std::uint8_t score_for(int steps, int nearest_steps) {
    // Unreachable camps score nothing.
    if (steps < 0)
        return 0;
    // nearest / max(steps, nearest) is between 0 and 1; scale it to 0-255. max() protects against dividing by zero.
    return static_cast<std::uint8_t>(max_score * nearest_steps / std::max(steps, nearest_steps));
}

// ---- One camp or altar, with everything we know about it ------------------------------------------------------------

// One row of the result: either a camp or an altar, with its distances, scores and route.
struct Entry {
    // true for an altar, false for a camp.
    bool is_altar = false;
    // Which monster, if it is a camp (ignored for altars). {} gives it the first enum value, which is harmless.
    cambc::camp_type camp_kind{};
    // The camp's tile, or the altar's own tile.
    cambc::vec2 target{0, 0};
    // Steps from our spawn, or -1 if unreachable.
    int our_steps = -1;
    // Steps from the enemy spawn, or -1 if unreachable or the enemy flood was skipped.
    int enemy_steps = -1;
    // 0-255, high = short walk from our spawn. Only meaningful for camps.
    std::uint8_t farm = 0;
    // 0-255, high = short walk from the enemy spawn. Only meaningful for camps.
    std::uint8_t combat = 0;
    // The route from our spawn (empty if unreachable).
    Route route;
};

// ---- Raw-buffer printing ---------------------------------------------------------------------------------------------
// std::cout and std::string appends burn far too much fuel per character, so lines are assembled in a plain character
// buffer by these tiny helpers (each takes "where to write next" and returns "where the next write goes").

// Copies a C string (null-terminated) to `out`; returns the pointer just past it.
char* put_text(char* out, const char* text) {
    // Copy until the terminating null character (a C string always ends with one).
    while (*text)
        // Copy one character, then advance both the source and the destination pointers.
        *out++ = *text++;
    // The caller continues writing from here.
    return out;
}

// Writes a whole number in decimal at `out`; returns the pointer just past it.
// How: peel digits off the end with % 10 (last digit first) into a scratch array, then write them back in reverse.
char* put_int(char* out, std::int64_t value) {
    // Negative: write a minus sign and continue with the positive value.
    if (value < 0) {
        // The minus sign.
        *out++ = '-';
        // Flip to positive so the digit loop below only deals with 0-9.
        value = -value;
    }
    // Scratch space for the digits (a 64-bit number has at most 19).
    char digits[20];
    // Number of digits stored.
    int count = 0;
    // do-while so that 0 still produces one digit.
    do {
        // value % 10 is the last digit; adding '0' converts it to its character.
        digits[count++] = static_cast<char>('0' + value % 10);
        // Drop the last digit.
        value /= 10;
        // Repeat while digits remain.
    } while (value);
    // Digits were stored backwards; write them out last-to-first so the number reads correctly.
    while (count)
        // Take the digit stored last (the leading one) and write it.
        *out++ = digits[--count];
    // The caller continues writing from here.
    return out;
}

// Writes a tile as "(x,y)"; returns the pointer just past it.
char* put_tile(char* out, cambc::vec2 p) {
    // Opening bracket.
    *out++ = '(';
    // The x coordinate.
    out = put_int(out, p.x);
    // Separator.
    *out++ = ',';
    // The y coordinate.
    out = put_int(out, p.y);
    // Closing bracket.
    *out++ = ')';
    // The caller continues writing from here.
    return out;
}

// Sends the finished line [begin, end) to the bot's log and flushes it at once, so an already-printed line can never be
// lost if the turn or the draft ends before the next line is written.
void emit(const char* begin, const char* end) {
    // Write (end - begin) bytes, as items of 1 byte each, to the standard output stream (which becomes the bot's log).
    std::fwrite(begin, 1, static_cast<std::size_t>(end - begin), stdout);
    // Force the text out right now instead of waiting for the stream's buffer to fill.
    std::fflush(stdout);
}

// Prints a fuel number, or "unknown" for the -1 meaning "could not be measured / skipped".
char* put_fuel(char* out, std::int64_t fuel) {
    // The ?: operator picks one of two results: a real number if fuel is 0 or more, the word "unknown" otherwise.
    return fuel >= 0 ? put_int(out, fuel) : put_text(out, "unknown");
}

// Prints a distance, or "-" for -1 (unreachable / not computed).
char* put_steps(char* out, int steps) {
    // Same idea as put_fuel: a number when we have one, a dash when we do not.
    return steps >= 0 ? put_int(out, steps) : put_text(out, "-");
}

// A line buffer big enough for any report line here (the longest is a camp line of well under 200 characters).
std::unique_ptr<char[]> line_buffer() {
    // make_unique_for_overwrite does not zero the memory (we overwrite it anyway, and zeroing costs fuel).
    // The returned unique_ptr frees the memory automatically when it goes out of scope.
    return std::make_unique_for_overwrite<char[]>(256);
}

// ---- The analysis ---------------------------------------------------------------------------------------------------

// The finished draft-memory bytes waiting to be submitted. analyse_and_publish() fills it on the first draft turn;
// play_draft_wip() submits it at the start of the next turn and empties it. Empty = nothing waiting.
std::vector<std::uint8_t> pending_memory;

// Floods from both spawns, scores the camps, builds the draft-memory bytes and prints a report. Called once, on the first
// draft turn. (The bytes are only PREPARED here; play_draft_wip() submits them on the next turn.)
void analyse_and_publish() {
    // Start of the stopwatch for the whole analysis.
    const std::uint64_t fuel_at_start = cambc::fuel_remaining();
    // A scratch buffer for report lines, reused for every line below.
    auto line = line_buffer();

    // GUARD 1: is there enough fuel to even start? If not, skip the analysis entirely so the picks of the later draft
    // turns are not endangered (a draft bot that runs out of fuel gets random picks). Estimate = padded tiles * guess + reserve.
    // Number of padded tiles, computed in 64-bit so the multiplication cannot overflow.
    const std::uint64_t padded_tiles = static_cast<std::uint64_t>(cambc::map::width() + 2) * (cambc::map::height() + 2);
    // Not enough fuel for the estimated cost plus the reserve?
    if (fuel_at_start < padded_tiles * fuel_per_tile_estimate + fuel_reserve) {
        // Tell the reader why there is no data in draft memory.
        char* out = put_text(line.get(), "draft_wip: analysis skipped, not enough fuel for this map size\n");
        // Send the message to the log.
        emit(line.get(), out);
        // Give up: the picks are what matter, and they are already made.
        return;
    }

    // The map as flat arrays with a blocked ring.
    const Grid grid = make_grid();

    // The spawn tiles of each team: the places the floods start from. Ours...
    const std::vector<cambc::vec2> our_spawn = cambc::map::spawn_region(cambc::team::ally).positions();
    // ...and the enemy's.
    const std::vector<cambc::vec2> enemy_spawn = cambc::map::spawn_region(cambc::team::enemy).positions();

    // Flood #1: shortest routes from OUR spawn. These give the farm scores and the paths we store.
    const Flood ours = flood(grid, our_spawn);

    // GUARD 2: flood #2 (from the enemy) needs about as much fuel as flood #1 actually used (measured, not guessed), plus
    // a quarter margin, plus the reserve for everything after it. If we cannot afford it, skip it: combat scores stay 0.
    // Starts empty with fuel = -1, which is exactly what "skipped" looks like to the rest of the code.
    Flood theirs;
    // Only run it if flood #1's cost is known (>= 0) and the remaining fuel covers 1.25x that cost plus the reserve.
    if (ours.fuel >= 0 && cambc::fuel_remaining() >= static_cast<std::uint64_t>(ours.fuel) * 5 / 4 + fuel_reserve)
        // Flood #2: how far every tile is from the ENEMY spawn.
        theirs = flood(grid, enemy_spawn);

    // ---- Collect every altar and camp with its distances and route ----
    // Tribute range: how close you must stand to an altar (1 tile in the rules).
    const int reach = cambc::constants::submit_gems_range;
    // All entries, altars first, then camps.
    std::vector<Entry> entries;

    // Altars: one call into the game lists them all.
    for (cambc::vec2 altar : cambc::map::altars().positions()) {
        // A fresh entry for this altar.
        Entry entry;
        // Mark it as an altar (the default is camp).
        entry.is_altar = true;
        // Remember where the altar itself is.
        entry.target = altar;
        // The tile next to it that we reach first from our spawn (or -1).
        const int stand = stand_tile_for(grid, ours, altar, reach);
        // Did we find one?
        if (stand >= 0) {
            // dist1 is steps + 1, so steps = dist1 - 1.
            entry.our_steps = ours.dist1[stand] - 1;
            // The route we will store in memory.
            entry.route = route_to(grid, ours, stand);
        }
        // Add it to the list. std::move hands the entry over without copying its route vector.
        entries.push_back(std::move(entry));
    }
    // Remember where the camps start, because only the camps get sorted by score below.
    const std::size_t first_camp = entries.size();

    // Camps: the game groups touching monsters into groups; walk every monster of every group.
    for (const auto& group : cambc::map::camp_groups()) {
        // Each group holds one or more individual monsters.
        for (const auto& camp : group.camps()) {
            // A fresh entry for this camp.
            Entry entry;
            // What kind of monster it is.
            entry.camp_kind = camp.kind();
            // The tile it stands on.
            entry.target = camp.spawn_position();
            // Padded index of the tile the monster stands on (camp tiles are walkable floor once cleared).
            const int i = grid.index(entry.target);
            // Reached from our spawn? (dist1 != 0 means yes.)
            if (ours.dist1[i] != 0) {
                // Steps = dist1 - 1.
                entry.our_steps = ours.dist1[i] - 1;
                // The route we will store in memory.
                entry.route = route_to(grid, ours, i);
            }
            // Reached from the enemy spawn? (An empty array means the enemy flood was skipped.)
            if (!theirs.dist1.empty() && theirs.dist1[i] != 0)
                // Steps = dist1 - 1. No route is needed for the enemy side, only the distance.
                entry.enemy_steps = theirs.dist1[i] - 1;
            // Add it to the list (moving, not copying).
            entries.push_back(std::move(entry));
        }
    }

    // ---- Scores: compare every camp with the nearest reachable one ----
    // Smallest distance from our spawn / from the enemy spawn among reachable camps. Start high (about a billion), shrink;
    // at least 1 later.
    int nearest_ours = 1 << 30;
    // Same for the enemy side.
    int nearest_theirs = 1 << 30;
    // Look at every camp (the altars before first_camp are not scored).
    for (std::size_t n = first_camp; n < entries.size(); ++n) {
        // -1 means unreachable; skip those.
        if (entries[n].our_steps >= 0)
            // Keep the smaller of the best so far and this camp's distance.
            nearest_ours = std::min(nearest_ours, entries[n].our_steps);
        // Same check for the enemy distance.
        if (entries[n].enemy_steps >= 0)
            // Keep the smaller one.
            nearest_theirs = std::min(nearest_theirs, entries[n].enemy_steps);
    }
    // A camp 0 steps away would make the formula divide by zero; treat the nearest as at least 1.
    nearest_ours = std::max(nearest_ours, 1);
    // Same protection for the enemy side.
    nearest_theirs = std::max(nearest_theirs, 1);
    // Give every camp its two scores.
    for (std::size_t n = first_camp; n < entries.size(); ++n) {
        // Farm score: from our distance.
        entries[n].farm = score_for(entries[n].our_steps, nearest_ours);
        // Combat score: from the enemy's distance.
        entries[n].combat = score_for(entries[n].enemy_steps, nearest_theirs);
    }
    // Most valuable-to-farm camps first, so if memory runs out it is the least valuable ones that get dropped.
    // stable_sort keeps the original order between equal scores, which keeps the output deterministic.
    // Only the camp part of the list is sorted (from first_camp to the end); the altars stay in front.
    // The last argument is a tiny function that says "a goes before b if a's farm score is higher".
    std::stable_sort(entries.begin() + static_cast<std::ptrdiff_t>(first_camp), entries.end(),
                     [](const Entry& a, const Entry& b) { return a.farm > b.farm; });

    // ---- Pack the routes into draft memory ----
    // How many bytes the game lets us store (1,024).
    const std::size_t capacity = cambc::constants::draft_memory_bytes;
    // The bytes we will submit. Header first (version, width, height); the entry count (byte 3) is a 0 for now and is
    // filled in at the end.
    std::vector<std::uint8_t> memory{format_version, static_cast<std::uint8_t>(grid.width),
                                     static_cast<std::uint8_t>(grid.height), 0};
    // How many entries actually fit and were written.
    std::size_t written = 0;
    // Go through the entries in priority order: altars first, then camps by farm score.
    for (const Entry& entry : entries) {
        // No walkable route: nothing to store.
        if (entry.our_steps < 0)
            continue;
        // Number of steps in this route.
        const std::size_t steps = entry.route.codes.size();
        // The step count is stored in ONE byte, so a longer route cannot be represented: skip it.
        if (steps > 255)
            continue;
        // Bytes this entry needs: fixed header + two steps per byte (rounded up).
        const std::size_t need = entry_header_bytes + (steps + 1) / 2;
        // Out of space: stop. Everything after this entry has lower priority, so nothing important is lost first.
        if (memory.size() + need > capacity)
            break;
        // Kind: the camp_type number, or 255 for an altar.
        memory.push_back(entry.is_altar ? altar_kind : static_cast<std::uint8_t>(entry.camp_kind));
        // Target tile, x...
        memory.push_back(static_cast<std::uint8_t>(entry.target.x));
        // ...and y.
        memory.push_back(static_cast<std::uint8_t>(entry.target.y));
        // The farm score...
        memory.push_back(entry.farm);
        // ...and the combat score.
        memory.push_back(entry.combat);
        // Where the route starts, x...
        memory.push_back(static_cast<std::uint8_t>(entry.route.start.x));
        // ...and y.
        memory.push_back(static_cast<std::uint8_t>(entry.route.start.y));
        // How many steps follow.
        memory.push_back(static_cast<std::uint8_t>(steps));
        // The steps, two per byte: first step in the low 4 bits, second in the high 4 bits. s counts steps, two at a time.
        for (std::size_t s = 0; s < steps; s += 2) {
            // First step of the pair goes in the low 4 bits (direction codes are 0-7, so they fit in 4 bits).
            std::uint8_t packed = entry.route.codes[s];
            // Second step of the pair, if there is one (the last byte of an odd-length route has only one).
            if (s + 1 < steps)
                // Shift the second step up by 4 bits and combine it with the first using OR.
                packed = static_cast<std::uint8_t>(packed | (entry.route.codes[s + 1] << 4));
            // Store the finished byte.
            memory.push_back(packed);
        }
        // One more entry written.
        ++written;
    }
    // Patch the entry count into byte 3 of the header.
    memory[3] = static_cast<std::uint8_t>(written);
    // Keep the finished bytes for the NEXT draft turn instead of submitting them now. This turn's pick has already been
    // made, and the game rejects a draft-memory submission after the pick (engine error 18, invalid_action), so
    // play_draft_wip() submits them at the start of the next turn, before that turn's pick.
    pending_memory = memory;

    // ---- Report ----
    // Stopwatch end: fuel left after everything so far.
    const std::uint64_t fuel_now = cambc::fuel_remaining();
    // Fuel used by everything so far (analysis only; the printing below is not included). -1 if a new turn began meanwhile.
    const std::int64_t analysis_fuel = fuel_now <= fuel_at_start ? static_cast<std::int64_t>(fuel_at_start - fuel_now) : -1;

    // Header line: memory used, entries stored, and the fuel numbers. `out` is the write cursor into the line buffer.
    char* out = put_text(line.get(), "draft_wip: memory ");
    // Bytes of memory used...
    out = put_int(out, static_cast<std::int64_t>(memory.size()));
    // ...out of...
    out = put_text(out, "/");
    // ...the capacity.
    out = put_int(out, static_cast<std::int64_t>(capacity));
    // Label for the entry counts.
    out = put_text(out, " bytes, entries ");
    // Entries that were stored...
    out = put_int(out, static_cast<std::int64_t>(written));
    // ...out of...
    out = put_text(out, "/");
    // ...all entries found (stored ones can be fewer: unreachable or no space).
    out = put_int(out, static_cast<std::int64_t>(entries.size()));
    // Label for the first fuel number.
    out = put_text(out, ", fuel: our flood ");
    // Cost of flood #1.
    out = put_fuel(out, ours.fuel);
    // Label for the second fuel number.
    out = put_text(out, ", enemy flood ");
    // Cost of flood #2 ("unknown" if it was skipped).
    out = put_fuel(out, theirs.fuel);
    // Label for the total.
    out = put_text(out, ", whole analysis ");
    // Cost of everything from the start of the analysis to just before this report.
    out = put_fuel(out, analysis_fuel);
    // Label for the remainder.
    out = put_text(out, ", left ");
    // Fuel still left in this turn's budget.
    out = put_int(out, static_cast<std::int64_t>(fuel_now));
    // End the line.
    *out++ = '\n';
    // Send it to the log.
    emit(line.get(), out);

    // One line per entry, in the order they were stored.
    for (const Entry& entry : entries) {
        // Altars and camps print differently.
        if (entry.is_altar) {
            // "  altar (x,y) ours N" : the label and position...
            out = put_text(line.get(), "  altar ");
            // ...the altar's tile...
            out = put_tile(out, entry.target);
            // ...a label...
            out = put_text(out, " ours ");
            // ...and its distance from our spawn ("-" if unreachable).
            out = put_steps(out, entry.our_steps);
        } else {
            // "  camp <kind> (x,y) farm F combat C ours N enemy M" : the label...
            out = put_text(line.get(), "  camp ");
            // ...the monster's name (to_string makes a temporary std::string; that is safe because put_text finishes
            // copying it before the end of this statement, when the temporary is destroyed)...
            out = put_text(out, cambc::to_string(entry.camp_kind).c_str());
            // ...a space...
            *out++ = ' ';
            // ...the camp's tile...
            out = put_tile(out, entry.target);
            // ...label and farm score...
            out = put_text(out, " farm ");
            out = put_int(out, entry.farm);
            // ...label and combat score...
            out = put_text(out, " combat ");
            out = put_int(out, entry.combat);
            // ...label and our distance...
            out = put_text(out, " ours ");
            out = put_steps(out, entry.our_steps);
            // ...label and the enemy's distance.
            out = put_text(out, " enemy ");
            out = put_steps(out, entry.enemy_steps);
        }
        // End the line.
        *out++ = '\n';
        // Send it to the log.
        emit(line.get(), out);
    }
}

}  // namespace

// The WIP draft's entry point; one call per draft turn. See the header comment at the top of this file.
void play_draft_wip() {
    // Submit the memory prepared on the previous turn, if there is one. This MUST come before this turn's pick: the game
    // refuses a memory submission once the turn's pick has been made.
    if (!pending_memory.empty()) {
        // A failed submission must never cost us the pick below, so catch the error, report it, and carry on.
        try {
            // Hand the bytes to the game. Draft memory is writable only during the draft and is read-only for the champions later.
            cambc::draft_memory = pending_memory;
        } catch (const cambc::error& error) {
            // Report which engine error it was and its message, so the problem is visible in the bot's log.
            auto line = line_buffer();
            // Start of the message.
            char* out = put_text(line.get(), "draft_wip: draft memory submit failed, error ");
            // The numeric error code (for example 18 = invalid_action).
            out = put_int(out, static_cast<std::int64_t>(error.code()));
            // Separator.
            out = put_text(out, ": ");
            // The engine's own description of what went wrong (may be empty).
            out = put_text(out, error.message().c_str());
            // End the line.
            *out++ = '\n';
            // Send it to the log.
            emit(line.get(), out);
        }
        // Whether it worked or not, never try the same bytes again.
        pending_memory.clear();
    }

    // The champions to pick, in order. Cassi & Issac is ONE pick that gives both bodies. After Vendetta, the remaining
    // slots go to the highest-health melee champions not yet taken, by base health from the rules:
    //   Ryth 1200 (Strength 50), Marcella 1200 (Strength 45), Varg 1000. (Ren is 900, Issac 900 and Vendetta 800 are
    //   lower or already picked.) Ryth is listed before Marcella because it wins the 1200-health tie on Strength.
    // Maps allow 3 to 5 champions; the draft simply stops calling us once our team is full, so unused entries are harmless.
    // `constexpr std::array picks{...}` builds a fixed list whose size the compiler works out from the items.
    constexpr std::array picks{cambc::champion_draft::cassi_and_issac, cambc::champion_draft::vendetta,
                               cambc::champion_draft::ryth, cambc::champion_draft::marcella, cambc::champion_draft::varg};
    // How many champions we have already picked = which entry of the list is next.
    const std::size_t taken = cambc::draft::current_draft(cambc::team::ally).size();
    // Pick first (cheap, and must not be lost to a fuel problem later in this turn). The check stops us reading past the
    // end of the list if the draft ever asks for more picks than we listed.
    if (taken < picks.size())
        // Tell the game our pick for this turn.
        cambc::draft::pick(picks[taken]);

    // The map analysis is expensive, so do it only once. `static` keeps this variable's value between calls.
    static bool analysed = false;
    // Already done on an earlier draft turn: nothing more to do.
    if (analysed)
        return;
    // Mark it done BEFORE running, so even if it is interrupted we never start it a second time.
    analysed = true;
    // Do the flood, scoring, memory preparation and report.
    analyse_and_publish();
}
