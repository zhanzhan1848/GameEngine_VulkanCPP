#pragma once

// BodyEntityMap.h — engine body id ↔ Jolt BodyID ↔ ECS 实体的映射（M0：仅前两者 + 预留 owner）。

#include "SimulationAPI.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <unordered_map>
#include <vector>

namespace primal::simulation {

class body_entity_map
{
public:
    struct record
    {
        u64                 id{};
        JPH::BodyID         jid{};
        game_entity::entity owner{};
        body_desc::shape    shape{ body_desc::shape::box };
        math::v3            half_extents{};        // 线框 proxy 用
        bool                debug_draw_proxy{ true };
    };

    // 返回 engine body id（从 1 递增）；0 = 拒绝。
    u64 add(JPH::BodyID jid, game_entity::entity owner,
            body_desc::shape shape, const math::v3& half_extents, bool debug_draw_proxy);
    bool remove(u64 id);                    // 只删映射；Jolt body 由调用方先移除
    bool get(u64 id, JPH::BodyID& out_jid, game_entity::entity& out_owner) const;
    u32  count() const { return static_cast<u32>(_ordered_records.size()); }

    // 按 engine id 升序遍历（确定性哈希依赖稳定顺序）
    template <typename Fn>
    void for_each_ordered(Fn&& fn) const
    {
        for (const record& r : _ordered_records) {
            fn(r.id, r);
        }
    }

private:
    // _ordered_records 按 engine id 升序；_index 映射 id → 记录下标。
    // remove 用有序删除（保持确定性遍历顺序），M0/M1 body 量级下 O(n) 可接受。
    std::vector<record>                  _ordered_records;
    std::unordered_map<u64, std::size_t> _index;
    u64                                  _next_id{ 1 };
};

} // namespace primal::simulation
