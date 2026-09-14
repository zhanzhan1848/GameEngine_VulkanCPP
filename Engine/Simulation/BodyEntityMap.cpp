#include "BodyEntityMap.h"

#include <algorithm>

namespace primal::simulation {

u64 body_entity_map::add(JPH::BodyID jid, game_entity::entity owner,
                         body_desc::shape shape, const math::v3& half_extents, bool debug_draw_proxy)
{
    const u64 id = _next_id++;
    const std::size_t pos = static_cast<std::size_t>(
        std::upper_bound(_ordered_records.begin(), _ordered_records.end(), id,
                         [](u64 v, const record& r) { return v < r.id; }) -
        _ordered_records.begin());
    _ordered_records.insert(_ordered_records.begin() + static_cast<long>(pos),
                            record{ id, jid, owner, shape, half_extents, debug_draw_proxy });
    _index.emplace(id, pos);
    for (std::size_t i = pos + 1; i < _ordered_records.size(); ++i) {
        _index[_ordered_records[i].id] = i;
    }
    return id;
}

bool body_entity_map::remove(u64 id)
{
    auto it = _index.find(id);
    if (it == _index.end()) return false;

    const std::size_t pos = it->second;
    _ordered_records.erase(_ordered_records.begin() + static_cast<long>(pos));
    _index.erase(it);
    for (std::size_t i = pos; i < _ordered_records.size(); ++i) {
        _index[_ordered_records[i].id] = i;
    }
    return true;
}

bool body_entity_map::get(u64 id, JPH::BodyID& out_jid, game_entity::entity& out_owner) const
{
    auto it = _index.find(id);
    if (it == _index.end()) return false;
    const record& r = _ordered_records[it->second];
    out_jid = r.jid;
    out_owner = r.owner;
    return true;
}

} // namespace primal::simulation
