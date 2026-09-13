#pragma once

// PhysicsQuery.h — PFX 物理查询注入接口（架构文档 §4 松耦合行）。
// 粒子模块只认本接口；Simulation 层经 install_simulation_adapter() 注入实现。
// 静态注入点（引擎单例语义）——非线程安全，宿主初始化时安装一次。

#include "../PfxTypes.h"

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::interop {

struct raycast_hit {
    math::v3 point{};
    math::v3 normal{ 0.f, 1.f, 0.f };
    f32 distance{ 0.f };
};

using raycast_fn = bool (*)(const math::v3& from, const math::v3& dir, f32 max_dist, raycast_hit& out,
                            void* user);

class physics_query {
public:
    // 安装实现（nullptr 卸载）。
    static void install(raycast_fn fn, void* user) {
        s_fn = fn;
        s_user = user;
    }

    // 未安装实现时返回 false（粒子模块的"无场景碰撞"降级路径）。
    static bool raycast(const math::v3& from, const math::v3& dir, f32 max_dist, raycast_hit& out) {
        if (!s_fn) return false;
        return s_fn(from, dir, max_dist, out, s_user);
    }

private:
    inline static raycast_fn s_fn{ nullptr };
    inline static void* s_user{ nullptr };
};

// 把 primal::simulation::raycast 适配并安装为当前实现（interop 层职责）。
void install_simulation_adapter();

} // namespace primal::pfx::interop

#endif // !DISABLE_PARTICLE_FX
