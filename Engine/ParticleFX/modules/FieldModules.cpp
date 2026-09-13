// FieldModules.cpp — N4 场与边界模块（架构文档 §3.4/§7-N4，POP 碰撞/平流对应物）。
//
//   KillVolume     —— 解析体边界（AABB/球/平面；kill/bounce/slide/clamp）
//   CurlNoiseForce —— curl 噪声力场（确定性 value-noise 势场）
//   AdvectByField  —— 按向量场平流（.pgrid 速度场，pyro 缓存驱动粒子烟）
//   SDFCollision   —— SDF 场碰撞（.pgrid 符号距离场，bounce/slide/kill）

#include "../core/ModuleRegistry.h"
#include "../core/EmitterProgram.h"

#ifndef DISABLE_PARTICLE_FX

#include <cmath>
#include <cstdio>
#include <cstring>

namespace primal::pfx {

// 编译期场名册（定义见文件后部；模块 parse 在匿名命名空间内使用）
std::vector<std::string>& field_names_get();

namespace {

constexpr f32 kPiN{ 3.14159265358979f };
inline f32 lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

// ---- 确定性 3D value noise（整数哈希格点 + 三线性）--------------------------

inline u32 hash3(s32 x, s32 y, s32 z, u32 seed) {
    u32 h = (u32)(x) * 374761393u + (u32)(y) * 668265263u + (u32)(z) * 2654435761u + seed * 40503u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

inline f32 hash01(u32 h) { return (f32)(h & 0x00FFFFFFu) * (1.0f / 16777216.0f); }

f32 value_noise3(f32 x, f32 y, f32 z, u32 seed) {
    const f32 xf = std::floor(x), yf = std::floor(y), zf = std::floor(z);
    const s32 ix = (s32)xf, iy = (s32)yf, iz = (s32)zf;
    const f32 tx = x - xf, ty = y - yf, tz = z - zf;
    const f32 sx = tx * tx * (3.f - 2.f * tx);
    const f32 sy = ty * ty * (3.f - 2.f * ty);
    const f32 sz = tz * tz * (3.f - 2.f * tz);
    f32 c[2][2][2];
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b)
            for (int cc = 0; cc < 2; ++cc)
                c[a][b][cc] = hash01(hash3(ix + a, iy + b, iz + cc, seed));
    const f32 c00 = lerpf(c[0][0][0], c[1][0][0], sx);
    const f32 c01 = lerpf(c[0][0][1], c[1][0][1], sx);
    const f32 c10 = lerpf(c[0][1][0], c[1][1][0], sx);
    const f32 c11 = lerpf(c[0][1][1], c[1][1][1], sx);
    return lerpf(lerpf(c00, c10, sy), lerpf(c01, c11, sy), sz);
}

// ---- 解析体共通 ---------------------------------------------------------------

struct shape_spec {
    u8 shape{ 0 };            // 0 aabb 1 sphere 2 plane
    u8 response{ 0 };         // 0 kill 1 bounce 2 slide 3 clamp_inside
    f32 restitution{ 0.3f };
    math::v3 center{ 0.f, 0.f, 0.f };
    math::v3 half{ 1.f, 1.f, 1.f };
    f32 radius{ 1.f };
    math::v3 plane_normal{ 0.f, 1.f, 0.f };
};

// 求内表面法线（指向体外）与穿透深度；不在体内返回 false。
bool inside_volume(const shape_spec& s, const math::v3& p, math::v3& out_normal, f32& out_depth) {
    if (s.shape == 0) {   // AABB
        const f32 dx = s.half.x - std::fabs(p.x - s.center.x);
        const f32 dy = s.half.y - std::fabs(p.y - s.center.y);
        const f32 dz = s.half.z - std::fabs(p.z - s.center.z);
        if (dx <= 0.f || dy <= 0.f || dz <= 0.f) return false;
        if (dx <= dy && dx <= dz) {
            out_normal = math::v3{ p.x >= s.center.x ? 1.f : -1.f, 0.f, 0.f };
            out_depth = dx;
        } else if (dy <= dz) {
            out_normal = math::v3{ 0.f, p.y >= s.center.y ? 1.f : -1.f, 0.f };
            out_depth = dy;
        } else {
            out_normal = math::v3{ 0.f, 0.f, p.z >= s.center.z ? 1.f : -1.f };
            out_depth = dz;
        }
        return true;
    }
    if (s.shape == 1) {   // sphere
        const math::v3 d{ p.x - s.center.x, p.y - s.center.y, p.z - s.center.z };
        const f32 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len >= s.radius) return false;
        if (len < 1e-6f) {
            out_normal = math::v3{ 0.f, 1.f, 0.f };
            out_depth = s.radius;
        } else {
            out_normal = math::v3{ d.x / len, d.y / len, d.z / len };
            out_depth = s.radius - len;
        }
        return true;
    }
    // plane：法线负侧 = 体内部
    const f32 dist = (p.x - s.center.x) * s.plane_normal.x + (p.y - s.center.y) * s.plane_normal.y +
                     (p.z - s.center.z) * s.plane_normal.z;
    if (dist >= 0.f) return false;
    out_normal = s.plane_normal;
    out_depth = -dist;
    return true;
}

void apply_response(const shape_spec& s, math::v3& pos, math::v3& vel, const math::v3& normal,
                    f32 depth) {
    switch (s.response) {
        case 0:   // kill 由调用方处理
        case 3:   // clamp_inside：贴边 + 去内向速度
        case 2: { // slide
            const f32 vn = vel.x * normal.x + vel.y * normal.y + vel.z * normal.z;
            if (vn < 0.f) {
                vel.x -= vn * normal.x;
                vel.y -= vn * normal.y;
                vel.z -= vn * normal.z;
            }
            pos.x += normal.x * (depth + 1e-4f);
            pos.y += normal.y * (depth + 1e-4f);
            pos.z += normal.z * (depth + 1e-4f);
            break;
        }
        case 1: { // bounce：反射 + restitution
            const f32 vn = vel.x * normal.x + vel.y * normal.y + vel.z * normal.z;
            const f32 e = s.restitution;
            vel.x -= (1.f + e) * vn * normal.x;
            vel.y -= (1.f + e) * vn * normal.y;
            vel.z -= (1.f + e) * vn * normal.z;
            pos.x += normal.x * (depth + 1e-4f);
            pos.y += normal.y * (depth + 1e-4f);
            pos.z += normal.z * (depth + 1e-4f);
            break;
        }
        default:
            break;
    }
}

// ---- KillVolume ---------------------------------------------------------------

struct kill_volume_params {
    shape_spec spec;
};

bool parse_shape(const json::value& o, shape_spec& s, std::string& err) {
    static const char* const shapes[] = { "aabb", "sphere", "plane" };
    static const char* const responses[] = { "kill", "bounce", "slide", "clamp" };
    const json::value* v = o.find("shape");
    if (v && v->is_string()) {
        bool found = false;
        for (u32 i = 0; i < 3; ++i)
            if (v->as_string() == shapes[i]) {
                s.shape = (u8)i;
                found = true;
            }
        if (!found) {
            err = "unknown shape";
            return false;
        }
    }
    const json::value* r = o.find("response");
    if (r && r->is_string()) {
        bool found = false;
        for (u32 i = 0; i < 4; ++i)
            if (r->as_string() == responses[i]) {
                s.response = (u8)i;
                found = true;
            }
        if (!found) {
            err = "unknown response";
            return false;
        }
    }
    const json::value* c = o.find("center");
    if (c && c->is_array() && c->array_size() == 3)
        s.center = math::v3{ c->array_at(0)->as_f32(), c->array_at(1)->as_f32(), c->array_at(2)->as_f32() };
    const json::value* h = o.find("half");
    if (h && h->is_array() && h->array_size() == 3)
        s.half = math::v3{ h->array_at(0)->as_f32(), h->array_at(1)->as_f32(), h->array_at(2)->as_f32() };
    if (const json::value* rad = o.find("radius"); rad && rad->is_number()) s.radius = rad->as_f32();
    const json::value* n = o.find("normal");
    if (n && n->is_array() && n->array_size() == 3)
        s.plane_normal = math::v3{ n->array_at(0)->as_f32(), n->array_at(1)->as_f32(),
                                   n->array_at(2)->as_f32() };
    if (const json::value* e = o.find("restitution"); e && e->is_number()) s.restitution = e->as_f32();
    return true;
}

bool kill_volume_parse(const json::value& o, void* out, std::string& err) {
    return parse_shape(o, *(shape_spec*)out, err);
}

void kill_volume_exec(sim_context& ctx, const void* params) {
    auto* p = (const kill_volume_params*)params;
    math::v4* pos4 = ctx.store->v4_channel(attr::position) + ctx.row;
    math::v4* vel4 = ctx.store->v4_channel(attr::velocity) + ctx.row;
    const math::v3 p3{ pos4->x, pos4->y, pos4->z };
    math::v3 n;
    f32 depth;
    if (!inside_volume(p->spec, p3, n, depth)) return;
    if (p->spec.response == 0) {
        ctx.store->free(ctx.handle);   // kill：death 事件经 store
        return;
    }
    math::v3 vel{ vel4->x, vel4->y, vel4->z };
    math::v3 pos{ pos4->x, pos4->y, pos4->z };
    apply_response(p->spec, pos, vel, n, depth);
    pos4->x = pos.x;
    pos4->y = pos.y;
    pos4->z = pos.z;
    vel4->x = vel.x;
    vel4->y = vel.y;
    vel4->z = vel.z;
}

constexpr param_hint kill_volume_hints[] = {
    { "shape", "enum", "aabb|sphere|plane" },
    { "response", "enum", "kill|bounce|slide|clamp" },
    { "center", "v3", "" },
    { "half", "v3", "aabb half extents" },
    { "radius", "f32", "" },
    { "normal", "v3", "plane normal" },
    { "restitution", "f32", "bounce energy keep" },
};

// ---- CurlNoiseForce -------------------------------------------------------------

struct curl_noise_params {
    u32 seed{ 0 };
    param_ref frequency{ u32_invalid_id, 1.f };
    param_ref strength{ u32_invalid_id, 1.f };
};

bool curl_noise_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (curl_noise_params*)out;
    if (const json::value* s = o.find("seed"); s && s->is_number()) p->seed = s->as_u32();
    const json::value* freq = o.find("frequency");
    if (freq) {
        if (!freq->is_number()) {
            err = "'frequency' must be a number";
            return false;
        }
        p->frequency = param_ref{ u32_invalid_id, freq->as_f32() };
    }
    const json::value* st = o.find("strength");
    if (st) {
        if (!st->is_number()) {
            err = "'strength' must be a number";
            return false;
        }
        p->strength = param_ref{ u32_invalid_id, st->as_f32() };
    }
    return true;
}

void curl_noise_exec(sim_context& ctx, const void* params) {
    auto* p = (const curl_noise_params*)params;
    const math::v4* pos4 = ctx.store->v4_channel(attr::position) + ctx.row;
    math::v4* vel4 = ctx.store->v4_channel(attr::velocity) + ctx.row;
    const f32 freq = resolve_param(p->frequency, ctx);
    const f32 strength = resolve_param(p->strength, ctx);
    if (strength == 0.f || freq <= 0.f) return;

    const f32 x = pos4->x * freq, y = pos4->y * freq, z = pos4->z * freq;
    const f32 e = 0.1f;
    // 势场 ψ = (n, n+off1, n+off2)；curl = ∇×ψ（中心差分）
    auto psi1 = [&](f32 a, f32 b, f32 c) { return value_noise3(a, b, c, p->seed); };
    auto psi2 = [&](f32 a, f32 b, f32 c) { return value_noise3(a + 31.7f, b - 5.3f, c + 12.9f, p->seed); };
    auto psi3 = [&](f32 a, f32 b, f32 c) { return value_noise3(a - 74.1f, b + 22.2f, c + 3.7f, p->seed); };

    const f32 d3 = (psi1(x, y + e, z) - psi1(x, y - e, z) - psi1(x, y, z + e) + psi1(x, y, z - e)) / (2.f * e);
    const f32 d2 = (psi1(x, y + e, z) - psi1(x, y - e, z) - psi1(x, y + e, z) + psi1(x, y - e, z)) / (2.f * e);
    (void)d2;
    // curl = ( ∂ψ3/∂y - ∂ψ2/∂z, ∂ψ1/∂z - ∂ψ3/∂x, ∂ψ2/∂x - ∂ψ1/∂y )
    const f32 c_x = (psi3(x, y + e, z) - psi3(x, y - e, z) - psi2(x, y, z + e) + psi2(x, y, z - e)) / (2.f * e);
    const f32 c_y = (psi1(x, y, z + e) - psi1(x, y, z - e) - psi3(x + e, y, z) + psi3(x - e, y, z)) / (2.f * e);
    const f32 c_z = (psi2(x + e, y, z) - psi2(x - e, y, z) - psi1(x, y + e, z) + psi1(x, y - e, z)) / (2.f * e);
    (void)d3;

    vel4->x += c_x * strength * ctx.dt;
    vel4->y += c_y * strength * ctx.dt;
    vel4->z += c_z * strength * ctx.dt;
}

constexpr param_hint curl_noise_hints[] = {
    { "frequency", "f32", "noise spatial frequency" },
    { "strength", "f32", "force scale" },
    { "seed", "u32", "deterministic noise seed" },
};

// ---- AdvectByField / SDFCollision ----------------------------------------------

bool parse_field_ref(const json::value& o, u32& out_idx, std::string& err) {
    const json::value* v = o.find("field");
    if (!v || !v->is_string()) {
        err = "missing 'field' name";
        return false;
    }
    const std::vector<std::string>& names = field_names_get();
    for (u32 i = 0; i < names.size(); ++i) {
        if (names[i] == v->as_string()) {
            out_idx = i;
            return true;
        }
    }
    err = "unknown field '" + v->as_string() + "'";
    return false;
}

struct advect_params {
    u32 field_idx{ 0 };
    f32 strength{ 1.f };
    u8 set_velocity{ 0 };   // 0 = 叠加力，1 = 直接覆盖速度
};

bool advect_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (advect_params*)out;
    if (const json::value* s = o.find("strength"); s && s->is_number()) p->strength = s->as_f32();
    if (const json::value* m = o.find("mode"); m && m->is_string())
        p->set_velocity = m->as_string() == "set" ? 1u : 0u;
    return parse_field_ref(o, p->field_idx, err);
}

void advect_exec(sim_context& ctx, const void* params) {
    auto* p = (const advect_params*)params;
    if (!ctx.fields || p->field_idx >= ctx.fields->size()) return;
    const volume_grid* grid = (*ctx.fields)[p->field_idx].get();
    if (!grid || grid->get_kind() != volume_grid::kind::vector3) return;
    const math::v4* pos4 = ctx.store->v4_channel(attr::position) + ctx.row;
    math::v4* vel4 = ctx.store->v4_channel(attr::velocity) + ctx.row;
    const math::v3 sample_pt{ pos4->x, pos4->y, pos4->z };
    const math::v3 v = grid->sample_vector(sample_pt);
    if (p->set_velocity) {
            vel4->x = v.x * p->strength;
        vel4->y = v.y * p->strength;
        vel4->z = v.z * p->strength;
    } else {
        vel4->x += v.x * p->strength * ctx.dt;
        vel4->y += v.y * p->strength * ctx.dt;
        vel4->z += v.z * p->strength * ctx.dt;
    }
}

constexpr param_hint advect_hints[] = {
    { "field", "string", "vector field name" },
    { "strength", "f32", "" },
    { "mode", "enum", "force|set" },
};

struct sdf_collision_params {
    u32 field_idx{ 0 };
    u8 response{ 1 };        // 0 kill 1 bounce 2 slide
    f32 restitution{ 0.3f };
    f32 pushout{ 1e-3f };
};

bool sdf_collision_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (sdf_collision_params*)out;
    static const char* const responses[] = { "kill", "bounce", "slide" };
    const json::value* r = o.find("response");
    if (r && r->is_string()) {
        bool found = false;
        for (u32 i = 0; i < 3; ++i)
            if (r->as_string() == responses[i]) {
                p->response = (u8)i;
                found = true;
            }
        if (!found) {
            err = "unknown response";
            return false;
        }
    }
    if (const json::value* e = o.find("restitution"); e && e->is_number()) p->restitution = e->as_f32();
    return parse_field_ref(o, p->field_idx, err);
}

void sdf_collision_exec(sim_context& ctx, const void* params) {
    auto* p = (const sdf_collision_params*)params;
    if (!ctx.fields || p->field_idx >= ctx.fields->size()) return;
    const volume_grid* grid = (*ctx.fields)[p->field_idx].get();
    if (!grid || grid->get_kind() != volume_grid::kind::scalar) return;

    math::v4* pos4 = ctx.store->v4_channel(attr::position) + ctx.row;
    math::v4* vel4 = ctx.store->v4_channel(attr::velocity) + ctx.row;
    const math::v3 p3{ pos4->x, pos4->y, pos4->z };
    const f32 d = grid->sample_scalar(p3);
    if (d > 0.f) return;   // 体外

    if (p->response == 0) {
        ctx.store->free(ctx.handle);
        return;
    }
    const math::v3 grad = grid->sample_gradient(p3);
    f32 len = std::sqrt(grad.x * grad.x + grad.y * grad.y + grad.z * grad.z);
    math::v3 n{ 0.f, 1.f, 0.f };
    if (len > 1e-8f) n = math::v3{ grad.x / len, grad.y / len, grad.z / len };

    const f32 vn = vel4->x * n.x + vel4->y * n.y + vel4->z * n.z;
    if (p->response == 1) {   // bounce
        const f32 e = p->restitution;
        vel4->x -= (1.f + e) * vn * n.x;
        vel4->y -= (1.f + e) * vn * n.y;
        vel4->z -= (1.f + e) * vn * n.z;
    } else if (vn < 0.f) {    // slide：去内向分量
        vel4->x -= vn * n.x;
        vel4->y -= vn * n.y;
        vel4->z -= vn * n.z;
    }
    // 推出体外：沿 -SDF 梯度方向推 |d|+pad
    pos4->x -= n.x * (d + p->pushout);
    pos4->y -= n.y * (d + p->pushout);
    pos4->z -= n.z * (d + p->pushout);
}

constexpr param_hint sdf_collision_hints[] = {
    { "field", "string", "scalar SDF field name" },
    { "response", "enum", "kill|bounce|slide" },
    { "restitution", "f32", "" },
};

} // namespace

// ---- 注册与场绑定 ---------------------------------------------------------------

// 编译期场名册（编译器设置；模块 parse 阶段绑定 field 名 → 下标）
std::vector<std::string>& field_names_get() {
    static std::vector<std::string> names;
    return names;
}

void set_compile_env_fields(const std::vector<std::string>* names) {
    field_names_get().clear();
    if (names) field_names_get().insert(field_names_get().end(), names->begin(), names->end());
}

namespace {

// 编译器在场绑定阶段调用：把 field 名解析进参数块（AdvectByField/SDFCollision 共用布局前缀）
bool bind_field(void* params) {
    u32* idx = (u32*)params;
    const std::vector<std::string>& names = field_names_get();
    // 绑定阶段已写入名字 —— 这里按名册顺序核对下标合法
    return *idx < names.size();
}

} // namespace

void register_field_modules() {
    module_registry& r = module_registry::get();
    {
        module_def def;
        def.name = "KillVolume";
        def.stage = stage_kind::update;
        def.params_size = sizeof(kill_volume_params);
        def.init_defaults = [](void* out) { new (out) kill_volume_params(); };
        def.parse = kill_volume_parse;
        def.row_exec = kill_volume_exec;
        def.hints = kill_volume_hints;
        def.hint_count = 7;
        r.register_module(def);
    }
    {
        module_def def;
        def.name = "CurlNoiseForce";
        def.stage = stage_kind::update;
        def.params_size = sizeof(curl_noise_params);
        def.init_defaults = [](void* out) { new (out) curl_noise_params(); };
        def.parse = curl_noise_parse;
        def.row_exec = curl_noise_exec;
        def.hints = curl_noise_hints;
        def.hint_count = 3;
        r.register_module(def);
    }
    {
        module_def def;
        def.name = "AdvectByField";
        def.stage = stage_kind::update;
        def.params_size = sizeof(advect_params);
        def.init_defaults = [](void* out) { new (out) advect_params(); };
        def.parse = advect_parse;
        def.row_exec = advect_exec;
        def.hints = advect_hints;
        def.hint_count = 3;
        r.register_module(def);
    }
    {
        module_def def;
        def.name = "SDFCollision";
        def.stage = stage_kind::update;
        def.params_size = sizeof(sdf_collision_params);
        def.init_defaults = [](void* out) { new (out) sdf_collision_params(); };
        def.parse = sdf_collision_parse;
        def.row_exec = sdf_collision_exec;
        def.hints = sdf_collision_hints;
        def.hint_count = 3;
        r.register_module(def);
    }
}

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
