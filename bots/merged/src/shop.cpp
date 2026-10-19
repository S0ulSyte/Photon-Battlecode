// shop.cpp - Buying strength shards with the gold we have (shop order D15).
//
// Reads: gold, item counts, my targets (farm or attack). Writes: purchases.
// Edit here to change: the shop order (which emblem families, in which order), when to keep saving.
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

// What: buy strength shards with the gold we have, in the order of shop order D15 (treasure, durability, monster+jungle or home, then bruiser forever).
// Inputs: none. Result: none. Side effects: purchases; SHOP log line on an engine error. Fuel: ~1,250 per call (only called on spawn tiles or when dead).
void Bot::shop() {
    FUEL_SCOPE(FP_SHOP);
    auto& me = cambc::self();
    using IT = cambc::item_type;
    struct Family {
        IT shard, emblem;
    };
    std::vector<Family> order;
    order.push_back({IT::treasure_shard_of_strength, IT::treasure_emblem_of_strength});
    order.push_back({IT::durability_shard_of_strength, IT::durability_emblem_of_strength});
    const bool both_farm = my_targets_.size() >= 2 && !plan_.targets[my_targets_[0]].attack && !plan_.targets[my_targets_[1]].attack;
    if (both_farm) {
        order.push_back({IT::monster_shard_of_strength, IT::monster_emblem_of_strength});
        order.push_back({IT::jungle_shard_of_strength, IT::jungle_emblem_of_strength});
    } else {
        order.push_back({IT::home_shard_of_strength, IT::home_emblem_of_strength});
    }
    try {
        const auto counts = me.item_count();
        for (const Family& f : order) {
            if (counts[f.emblem] > 0)
                continue;  // this emblem is already built
            const std::uint32_t price = f.shard.cost();
            const std::uint32_t needed = cfg::kShardsPerEmblem - std::min<std::uint32_t>(cfg::kShardsPerEmblem, counts[f.shard]);
            const std::uint32_t affordable = me.gold_count() / price;
            const std::uint32_t buy = std::min(needed, affordable);
            if (buy > 0 && me.can_buy(f.shard, buy))
                me.buy(f.shard, buy);
            if (buy < needed)
                return;  // cannot finish this emblem yet: keep saving
        }
        // All the planned emblems exist: bruiser emblems from then on (any number of shards).
        const IT bruiser = IT::bruiser_shard_of_strength;
        const std::uint32_t affordable = me.gold_count() / bruiser.cost();
        if (affordable > 0 && me.can_buy(bruiser, affordable))
            me.buy(bruiser, affordable);
    } catch (const cambc::error& error) {
        LogLine log;
        log.s("SHOP ").s(name_).s(" error ").s(error.message().c_str());
        log.flush();
    }
}

