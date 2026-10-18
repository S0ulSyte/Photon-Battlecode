// champion_util.cpp - see champion_util.h.
#include "champion_util.h"

// See champion_util.h. No engine call.
int slot_of(cambc::champion_type t) {
    using T = cambc::champion_type;
    switch (t) {
    case T::issac: return 0;
    case T::cassi: return 1;
    case T::vendetta: return 2;
    case T::ryth: return 3;
    case T::marcella: return 4;
    case T::varg: return 5;
    default: return -1;
    }
}

// See champion_util.h. No engine call.
std::uint32_t id_of(const cambc::champion& c) {
    return static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(c));
}

// What: the whole live record of a champion from ONE engine call. Why: the SDK answers health(), position(), strength(), focus(), max_health(),
// can_move(), can_attack(), is_recalling()... each with this same engine call (~100 fuel each), so asking for three or four of them in a row paid for
// the same record three or four times. How: the SDK's own low-level call with the SDK's own error check; the values are exactly what the separate
// getters would return at this moment. Fuel: about 100.
cambc::detail::champion_info snapshot_of(const cambc::champion& c) {
    cambc::detail::champion_info record{};
    cambc::detail::check(cambc::detail::sdk_champion(static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(c)), &record));
    return record;
}

// See champion_util.h. No engine call.
bool has_flag(const cambc::detail::champion_info& record, cambc::champion_state flag) {
    return cambc::champion_state::from_bits_retain(record.state).contains(flag);
}

#if V2_CHANGE_SHARED_ALLIES
// CHANGE 13. The champion type of each draft slot (0 Issac, 1 Cassi, 2 Vendetta, 3 Ryth, 4 Marcella, 5 Varg).
cambc::champion_type type_of_slot(int slot) {
    using T = cambc::champion_type;
    static const T types[6] = {T::issac, T::cassi, T::vendetta, T::ryth, T::marcella, T::varg};
    return types[slot];
}

// What: design stats of the champion in a slot. Why: a shared-memory ally has no live stats we can read, so FIGHT CHECK uses the design (base) health,
// strength, focus and range. DEFAULT D25: this underestimates allies that bought strength shards. How: read once per slot and cached (a design query
// costs fuel). Fuel: ~300 the first time, about 5 afterwards.
const DesignStats& design_stats_of_slot(int slot) {
    static DesignStats cache[6];
    DesignStats& d = cache[slot];
    if (!d.known) {
        try {
            const auto st = cambc::champion(type_of_slot(slot)).base_stats();
            d.health = st.base_health;
            d.strength = st.base_strength;
            d.focus = st.base_focus;
            d.range = st.base_attack_range;
        } catch (const std::exception&) {
            d.health = 700;  // unknown design: a middling champion
            d.strength = 40;
            d.focus = 70;
            d.range = 1;
        }
        d.known = true;
    }
    return d;
}
#endif
