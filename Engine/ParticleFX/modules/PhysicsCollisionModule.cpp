// PhysicsCollisionModule.cpp — PhysicsCollision（update 阶段）：粒子 vs 物理世界碰撞。
// 经 physics_query 门面（interop/PhysicsQuery.h，仅依赖 PfxTypes）射线探测；
// 宿主未安装适配器（物理未初始化）时 raycast 返回 false → 模块静默无操作。
//
// 探测语义（SDFCollision 同族的位置修正式响应，非扫掠积分）：从当前位置沿速度
// 方向 cast（距离 = radius + |v|·dt）；命中 → 位置回退到 hit.point + normal·radius，
// 法向速度反射 ×(1+bounce)、切向 ×(1-friction)；kill_on_hit 命中即死。
// 静止粒子（速度≈0）不做贴附去穿透——v1 只处理运动中的穿透。

#include "../core/ModuleRegistry.h"
#include "../core/EmitterProgram.h"     // exposed_param 完整类型（param 绑定解析用）
#include "../interop/PhysicsQuery.h"

#ifndef DISABLE_PARTICLE_FX

#include <cmath>

namespace primal::pfx {

namespace {

// ---- 局部解析辅助（与 BuiltinModules 的 param_ref 解析同款语义，自包含） --------

bool param_ref_field(const json::value& o, const char* k, const param_ref& def, param_ref& out,
                     std::vector<exposed_param>* exposed, std::string& err) {
    const json::value* v = o.find(k);
    if (!v) {
        out = def;
        return true;
    }
    if (v->is_number()) {
        out = param_ref{ u32_invalid_id, v->as_f32() };
        return true;
    }
    if (v->is_object() && exposed) {
        const json::value* pname = v->find("param");
        if (pname && pname->is_string()) {
            const u32 h = hash_name(pname->as_string().c_str());
            for (u32 i = 0; i < exposed->size(); ++i) {
                if ((*exposed)[i].name_hash == h) {
                    out = param_ref{ i, def.value };
                    if (const json::value* d = v->find("default"); d && d->is_number())
                        out.value = d->as_f32();
                    return true;
                }
            }
            err = "unknown exposed param '" + pname->as_string() + "'";
            return false;
        }
    }
    err = std::string("'") + k + "' must be a number or param binding";
    return false;
}

// ---- 模块 ---------------------------------------------------------------------

struct physics_collision_params {
    param_ref radius{ u32_invalid_id, 0.05f };     // 粒子碰撞半径（球代理）
    param_ref bounce{ u32_invalid_id, 0.3f };      // 法向反弹系数（0 = 贴附）
    param_ref friction{ u32_invalid_id, 0.1f };    // 切向速度衰减（0 = 无摩擦）
    u8 kill_on_hit{ 0 };
};

bool physics_collision_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (physics_collision_params*)out;
    auto* exposed = compile_env_exposed_get();
    if (!param_ref_field(o, "radius", param_ref{ u32_invalid_id, 0.05f }, p->radius, exposed, err))
        return false;
    if (!param_ref_field(o, "bounce", param_ref{ u32_invalid_id, 0.3f }, p->bounce, exposed, err))
        return false;
    if (!param_ref_field(o, "friction", param_ref{ u32_invalid_id, 0.1f }, p->friction, exposed, err))
        return false;
    p->kill_on_hit = 0;
    if (const json::value* v = o.find("kill_on_hit"); v) {
        if (!v->is_bool()) {
            err = "'kill_on_hit' must be a bool";
            return false;
        }
        p->kill_on_hit = v->as_bool() ? 1u : 0u;
    }
    return true;
}

void physics_collision_exec(sim_context& ctx, const void* params) {
    auto* p = (const physics_collision_params*)params;

    math::v4* pos4 = ctx.store->v4_channel(attr::position) + ctx.row;
    math::v4* vel4 = ctx.store->v4_channel(attr::velocity) + ctx.row;
    const math::v3 from{ pos4->x, pos4->y, pos4->z };
    const f32 speed = std::sqrt(vel4->x * vel4->x + vel4->y * vel4->y + vel4->z * vel4->z);
    if (speed <= 1e-8f) return;   // 静止：无探测方向

    // 碰撞半径跟随粒子缩放：radius 参数按"缩放 1 时的半径"填写，
    // 粒子经 SetScale/ScaleOverLife 缩放后有效半径同步缩放，
    // 与 mesh 实例渲染（几何半径 × 粒子缩放）保持一致。
    f32 scale = 1.f;
    if (const math::v4* scl4 = ctx.store->v4_channel(attr::scale_rotation))
        scale = scl4[ctx.row].x > 1e-6f ? scl4[ctx.row].x : 1.f;
    const f32 radius = resolve_param(p->radius, ctx) * scale;
    const f32 cast = radius + speed * ctx.dt;
    const f32 inv = 1.f / speed;
    const math::v3 dir{ vel4->x * inv, vel4->y * inv, vel4->z * inv };

    interop::raycast_hit hit;
    if (!interop::physics_query::raycast(from, dir, cast, hit)) return;

    if (p->kill_on_hit) {
        ctx.store->free(ctx.handle);
        return;
    }

    math::v3 n = hit.normal;
    f32 nl = n.x * n.x + n.y * n.y + n.z * n.z;
    if (nl <= 1e-12f) return;   // 非法法线：放弃本次响应（保确定性，不动粒子）
    nl = 1.f / std::sqrt(nl);
    n = math::v3{ n.x * nl, n.y * nl, n.z * nl };

    const math::v3 v{ vel4->x, vel4->y, vel4->z };
    const f32 vn = v.x * n.x + v.y * n.y + v.z * n.z;
    const f32 bounce = resolve_param(p->bounce, ctx);
    const f32 friction = resolve_param(p->friction, ctx);
    // 反射法向 + 衰减切向：v' = v - (1+e)·vn·n；v'' = v' - friction·(v' - vn'·n)
    math::v3 vp{ v.x - (1.f + bounce) * vn * n.x, v.y - (1.f + bounce) * vn * n.y,
                 v.z - (1.f + bounce) * vn * n.z };
    const f32 vn2 = vp.x * n.x + vp.y * n.y + vp.z * n.z;
    vp = math::v3{ vp.x - friction * (vp.x - vn2 * n.x), vp.y - friction * (vp.y - vn2 * n.y),
                   vp.z - friction * (vp.z - vn2 * n.z) };

    pos4->x = hit.point.x + n.x * radius;
    pos4->y = hit.point.y + n.y * radius;
    pos4->z = hit.point.z + n.z * radius;
    vel4->x = vp.x;
    vel4->y = vp.y;
    vel4->z = vp.z;
}

constexpr param_hint physics_collision_hints[] = {
    { "radius", "param_ref", "particle collision sphere radius AT SCALE 1 (auto-scales with particle; builtin cube/sphere visual radius = 0.05)", "0.05" },
    { "bounce", "param_ref", "normal restitution (0=stick)", "0.3" },
    { "friction", "param_ref", "tangential damping per hit", "0.1" },
    { "kill_on_hit", "bool", "kill particle on first hit", "false" },
};

} // namespace

void register_physics_collision_module() {
    module_registry& r = module_registry::get();
    module_def def;
    def.name = "PhysicsCollision";
    def.stage = stage_kind::update;
    def.params_size = sizeof(physics_collision_params);
    def.init_defaults = [](void* out) { new (out) physics_collision_params(); };
    def.row_exec = physics_collision_exec;
    def.parse = physics_collision_parse;
    def.hints = physics_collision_hints;
    def.hint_count = 4;
    r.register_module(def);
}

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
