// BuiltinModules.cpp — PFX 内置模块第一梯队（架构文档 §7-N1，对标 Niagara 标准模块子集）。
//
// spawn: SpawnRate · SpawnBurst
// init:  ShapeLocation · AddVelocity · SetLifetime · SetColor · SetScale
// update: GravityForce · DragForce · KillAge
//
// 模块间契约：sim_context::last_normal —— ShapeLocation 写、AddVelocity(normal) 读。
// 参数 POD 与 params blob 一一对应（parse 直接写入）；全部解析无异常、带字段报错。

#include "../core/ModuleRegistry.h"
#include "../core/EmitterProgram.h"

#ifndef DISABLE_PARTICLE_FX

#include <cmath>
#include <cstring>

namespace primal::pfx {

namespace {

// ---- 小工具 ------------------------------------------------------------------

constexpr f32 kPi{ 3.14159265358979f };

f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

bool get_f32_opt(const json::value& o, const char* k, f32 def, f32& out, std::string& err) {
    const json::value* v = o.find(k);
    if (!v) {
        out = def;
        return true;
    }
    if (!v->is_number()) {
        err = std::string("'") + k + "' must be a number";
        return false;
    }
    out = v->as_f32();
    return true;
}

bool get_u8_enum(const json::value& o, const char* k, const char* const* names, u32 count, u8 def,
                 u8& out, std::string& err) {
    const json::value* v = o.find(k);
    if (!v) {
        out = def;
        return true;
    }
    if (!v->is_string()) {
        err = std::string("'") + k + "' must be a string";
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        if (v->as_string() == names[i]) {
            out = (u8)i;
            return true;
        }
    }
    err = std::string("'") + k + "': unknown value '" + v->as_string() + "'";
    return false;
}

math::v3 random_unit_vector(pcg32& rng) {
    // 球面均匀：Marsaglia 拒绝法（最多 8 次重试，固定上限保确定性）
    for (u32 i = 0; i < 8; ++i) {
        const f32 x = rng.next_f32() * 2.f - 1.f;
        const f32 y = rng.next_f32() * 2.f - 1.f;
        const f32 z = rng.next_f32() * 2.f - 1.f;
        const f32 d = x * x + y * y + z * z;
        if (d >= 0.01f && d <= 1.0f) {
            const f32 inv = 1.0f / std::sqrt(d);
            return math::v3{ x * inv, y * inv, z * inv };
        }
    }
    return math::v3{ 0.f, 1.f, 0.f };
}

bool get_param_ref_field(const json::value& o, const char* k, const param_ref& def, param_ref& out,
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
                    if (const json::value* d = v->find("default"); d && d->is_number()) out.value = d->as_f32();
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

// 编译期 parse 上下文：暴露参数名册（模块绑定需要；直接 spawn/无绑定场景可为 null）
struct parse_env {
    std::vector<exposed_param>* exposed{ nullptr };
};

parse_env& parse_env_get() {
    static parse_env env;
    return env;
}

} // namespace

void set_compile_env_exposed(std::vector<exposed_param>* exposed) { parse_env_get().exposed = exposed; }

namespace {

// ---- 模块参数 POD 与实现 -------------------------------------------------------

// SpawnRate --------------------------------------------------------------
struct spawn_rate_params {
    param_ref rate{ u32_invalid_id, 100.f };
};

bool spawn_rate_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (spawn_rate_params*)out;
    return get_param_ref_field(o, "rate", param_ref{ u32_invalid_id, 100.f }, p->rate,
                               parse_env_get().exposed, err);
}

u32 spawn_rate_exec(sim_context& ctx, const void* params) {
    const auto* p = (const spawn_rate_params*)params;
    const f32 rate = resolve_param(p->rate, ctx) * ctx.lod_factor;
    ctx.spawn_accum[0] += rate * ctx.dt;
    const u32 n = (u32)ctx.spawn_accum[0];
    ctx.spawn_accum[0] -= (f32)n;
    return n;
}

constexpr param_hint spawn_rate_hints[] = { { "rate", "param_ref", "particles per second", "100" } };

// SpawnBurst -------------------------------------------------------------
struct spawn_burst_params {
    param_ref period{ u32_invalid_id, 2.f };
    param_ref count{ u32_invalid_id, 50.f };
};
struct spawn_burst_state {
    f32 timer{ 0.f };
    bool fired_once{ false };
};

bool spawn_burst_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (spawn_burst_params*)out;
    if (!get_param_ref_field(o, "period", param_ref{ u32_invalid_id, 2.f }, p->period,
                             parse_env_get().exposed, err))
        return false;
    return get_param_ref_field(o, "count", param_ref{ u32_invalid_id, 50.f }, p->count,
                               parse_env_get().exposed, err);
}

u32 spawn_burst_exec(sim_context& ctx, const void* params) {
    auto* p = (const spawn_burst_params*)params;
    auto* st = (spawn_burst_state*)ctx.state;
    if (!st->fired_once) {
        st->fired_once = true;
        return (u32)(resolve_param(p->count, ctx) * ctx.lod_factor);
    }
    st->timer += ctx.dt;
    const f32 period = resolve_param(p->period, ctx);
    if (period <= 0.f) return 0;
    if (st->timer >= period) {
        st->timer -= period;
        return (u32)(resolve_param(p->count, ctx) * ctx.lod_factor);
    }
    return 0;
}

constexpr param_hint spawn_burst_hints[] = {
    { "period", "param_ref", "seconds between bursts", "2" },
    { "count", "param_ref", "particles per burst", "50" },
};

// ShapeLocation ----------------------------------------------------------
struct shape_location_params {
    u8 shape{ 0 };           // 0 point 1 sphere_surface 2 sphere_volume 3 box 4 cone 5 cylinder
    f32 angle_deg{ 25.f };   // cone 半角（度）
    f32 radius{ 1.f };       // sphere/cylinder/cone 底半径
    f32 height{ 1.f };       // cone 高 / cylinder 高
    f32 half[3]{ 0.5f, 0.5f, 0.5f };
};

bool shape_location_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (shape_location_params*)out;
    static const char* const shape_names[] = { "point", "sphere_surface", "sphere_volume", "box", "cone",
                                               "cylinder" };
    if (!get_u8_enum(o, "shape", shape_names, 6, p->shape, p->shape, err)) return false;
    if (!get_f32_opt(o, "radius", 1.f, p->radius, err)) return false;
    if (!get_f32_opt(o, "height", 1.f, p->height, err)) return false;
    if (!get_f32_opt(o, "angle", 25.f, p->angle_deg, err)) return false;
    if (const json::value* v = o.find("half"); v) {
        if (!v->is_array() || v->array_size() != 3) {
            err = "'half' must be [x,y,z]";
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            const json::value* e = v->array_at(i);
            if (!e->is_number()) {
                err = "'half' elements must be numbers";
                return false;
            }
            p->half[i] = e->as_f32();
        }
    }
    return true;
}

void shape_location_exec(sim_context& ctx, const void* params) {
    auto* p = (const shape_location_params*)params;
    pcg32& rng = *ctx.rng;
    math::v4* pos = ctx.store->v4_channel(attr::position) + ctx.row;
    math::v3 p3{ 0.f, 0.f, 0.f };
    math::v3 n{ 0.f, 1.f, 0.f };

    switch (p->shape) {
        case 1: {   // sphere_surface
            n = random_unit_vector(rng);
            p3 = n * p->radius;
            break;
        }
        case 2: {   // sphere_volume（体积均匀：r ∝ cbrt(u)）
            n = random_unit_vector(rng);
            const f32 r = p->radius * std::cbrt(rng.next_f32());
            p3 = n * r;
            break;
        }
        case 3: {   // box
            p3 = math::v3{ lerp(-p->half[0], p->half[0], rng.next_f32()),
                           lerp(-p->half[1], p->half[1], rng.next_f32()),
                           lerp(-p->half[2], p->half[2], rng.next_f32()) };
            break;
        }
        case 4: {   // cone：顶点在原点、朝 +Y 展开
            const f32 t = rng.next_f32();
            const f32 tan_a = std::tan(p->angle_deg * kPi / 180.f);
            const f32 r = t * p->height * tan_a;
            const f32 theta = rng.next_f32() * 2.f * kPi;
            p3 = math::v3{ r * std::cos(theta), t * p->height, r * std::sin(theta) };
            n = math::v3{ std::cos(theta), tan_a, std::sin(theta) };
            break;
        }
        case 5: {   // cylinder：绕 +Y 的体积
            const f32 theta = rng.next_f32() * 2.f * kPi;
            const f32 r = p->radius * std::sqrt(rng.next_f32());
            p3 = math::v3{ r * std::cos(theta), lerp(-p->height * 0.5f, p->height * 0.5f, rng.next_f32()),
                           r * std::sin(theta) };
            n = math::v3{ std::cos(theta), 0.f, std::sin(theta) };
            break;
        }
        case 0:
        default:    // point
            break;
    }

    pos->x = p3.x;
    pos->y = p3.y;
    pos->z = p3.z;
    ctx.last_normal = n;
}

constexpr param_hint shape_location_hints[] = {
    { "shape", "enum", "point|sphere_surface|sphere_volume|box|cone|cylinder", "\"point\"" },
    { "radius", "f32", "sphere/cylinder/cone radius", "1" },
    { "height", "f32", "cone/cylinder height", "1" },
    { "half", "v3", "box half extents", "[0.5,0.5,0.5]" },
    { "angle", "f32", "cone half angle in degrees", "25" },
};

// AddVelocity ------------------------------------------------------------
struct add_velocity_params {
    u8 mode{ 0 };        // 0 fixed 1 shape_normal 2 random_sphere
    f32 dir[3]{ 0.f, 1.f, 0.f };
    param_ref speed_min{ u32_invalid_id, 1.f };
    param_ref speed_max{ u32_invalid_id, 2.f };
    f32 inherit{ 0.f };
};

bool add_velocity_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (add_velocity_params*)out;
    static const char* const mode_names[] = { "fixed", "shape_normal", "random_sphere" };
    if (!get_u8_enum(o, "mode", mode_names, 3, p->mode, p->mode, err)) return false;
    if (const json::value* v = o.find("direction"); v) {
        if (!v->is_array() || v->array_size() != 3) {
            err = "'direction' must be [x,y,z]";
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            const json::value* e = v->array_at(i);
            if (!e->is_number()) {
                err = "'direction' elements must be numbers";
                return false;
            }
            p->dir[i] = e->as_f32();
        }
    }
    if (!get_param_ref_field(o, "speed_min", param_ref{ u32_invalid_id, 1.f }, p->speed_min,
                             parse_env_get().exposed, err))
        return false;
    if (!get_param_ref_field(o, "speed_max", param_ref{ u32_invalid_id, 2.f }, p->speed_max,
                             parse_env_get().exposed, err))
        return false;
    return get_f32_opt(o, "inherit_source_velocity", 0.f, p->inherit, err);
}

void add_velocity_exec(sim_context& ctx, const void* params) {
    auto* p = (const add_velocity_params*)params;
    math::v3 dir;
    switch (p->mode) {
        case 1: dir = ctx.last_normal; break;
        case 2: dir = random_unit_vector(*ctx.rng); break;
        case 0:
        default:
            dir = math::v3{ p->dir[0], p->dir[1], p->dir[2] };
            break;
    }
    const f32 len2 = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
    if (len2 > 1e-8f) {
        const f32 inv = 1.0f / std::sqrt(len2);
        dir = math::v3{ dir.x * inv, dir.y * inv, dir.z * inv };
    }
    const f32 speed = lerp(resolve_param(p->speed_min, ctx), resolve_param(p->speed_max, ctx),
                           ctx.rng->next_f32());
    math::v4* vel = ctx.store->v4_channel(attr::velocity) + ctx.row;
    vel->x += dir.x * speed + ctx.source_velocity.x * p->inherit;
    vel->y += dir.y * speed + ctx.source_velocity.y * p->inherit;
    vel->z += dir.z * speed + ctx.source_velocity.z * p->inherit;
}

constexpr param_hint add_velocity_hints[] = {
    { "mode", "enum", "fixed|shape_normal|random_sphere", "\"fixed\"" },
    { "direction", "v3", "fixed mode direction", "[0,1,0]" },
    { "speed_min", "param_ref", "", "1" },
    { "speed_max", "param_ref", "", "2" },
    { "inherit_source_velocity", "f32", "0..1 fraction of source velocity", "0" },
};

// SetLifetime ------------------------------------------------------------
struct set_lifetime_params {
    param_ref min{ u32_invalid_id, 1.f };
    param_ref max{ u32_invalid_id, 1.f };
};

bool set_lifetime_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (set_lifetime_params*)out;
    if (!get_param_ref_field(o, "min", param_ref{ u32_invalid_id, 1.f }, p->min, parse_env_get().exposed,
                             err))
        return false;
    return get_param_ref_field(o, "max", param_ref{ u32_invalid_id, 1.f }, p->max,
                               parse_env_get().exposed, err);
}

void set_lifetime_exec(sim_context& ctx, const void* params) {
    auto* p = (const set_lifetime_params*)params;
    math::v4* vel = ctx.store->v4_channel(attr::velocity) + ctx.row;
    vel->w = lerp(resolve_param(p->min, ctx), resolve_param(p->max, ctx), ctx.rng->next_f32());
}

constexpr param_hint set_lifetime_hints[] = {
    { "min", "param_ref", "seconds", "1" },
    { "max", "param_ref", "seconds", "1" },
};

// SetColor ---------------------------------------------------------------
struct set_color_params {
    f32 c0[4]{ 1.f, 1.f, 1.f, 1.f };
    f32 c1[4]{ 1.f, 1.f, 1.f, 1.f };
    u8 use_range{ 0 };
};

bool parse_color(const json::value* v, f32 out[4], std::string& err, const char* key) {
    if (!v) {
        out[0] = out[1] = out[2] = out[3] = 1.f;
        return true;
    }
    if (!v->is_array() || v->array_size() != 4) {
        err = std::string("'") + key + "' must be [r,g,b,a]";
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        const json::value* e = v->array_at(i);
        if (!e->is_number()) {
            err = std::string("'") + key + "' elements must be numbers";
            return false;
        }
        out[i] = e->as_f32();
    }
    return true;
}

bool set_color_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (set_color_params*)out;
    if (!parse_color(o.find("color"), p->c0, err, "color")) return false;
    if (!parse_color(o.find("color_end"), p->c1, err, "color_end")) return false;
    if (o.find("color_end")) p->use_range = 1;
    return true;
}

void set_color_exec(sim_context& ctx, const void* params) {
    auto* p = (const set_color_params*)params;
    math::v4* col = ctx.store->v4_channel(attr::color) + ctx.row;
    if (p->use_range) {
        const f32 t = ctx.rng->next_f32();
        col->x = lerp(p->c0[0], p->c1[0], t);
        col->y = lerp(p->c0[1], p->c1[1], t);
        col->z = lerp(p->c0[2], p->c1[2], t);
        col->w = lerp(p->c0[3], p->c1[3], t);
    } else {
        col->x = p->c0[0];
        col->y = p->c0[1];
        col->z = p->c0[2];
        col->w = p->c0[3];
    }
}

constexpr param_hint set_color_hints[] = {
    { "color", "v4", "[r,g,b,a] start", "[1,1,1,1]" },
    // color_end 不给 default：存在即启用 range 模式（presence-sensitive）
    { "color_end", "v4", "optional end (random lerp)" },
};

// SetScale ---------------------------------------------------------------
struct set_scale_params {
    f32 s0[2]{ 1.f, 1.f };
    f32 s1[2]{ 1.f, 1.f };
    u8 use_range{ 0 };
};

bool set_scale_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (set_scale_params*)out;
    const char* keys[2] = { "scale", "scale_end" };
    for (int c = 0; c < 2; ++c) {
        const json::value* v = o.find(keys[c]);
        if (!v) continue;
        if (!v->is_array() || v->array_size() != 2) {
            err = std::string("'") + keys[c] + "' must be [w,h]";
            return false;
        }
        for (int i = 0; i < 2; ++i) {
            const json::value* e = v->array_at(i);
            if (!e->is_number()) {
                err = std::string("'") + keys[c] + "' elements must be numbers";
                return false;
            }
            (c == 0 ? p->s0 : p->s1)[i] = e->as_f32();
        }
        if (c == 1) p->use_range = 1;
    }
    return true;
}

void set_scale_exec(sim_context& ctx, const void* params) {
    auto* p = (const set_scale_params*)params;
    math::v4* scl = ctx.store->v4_channel(attr::scale_rotation) + ctx.row;
    if (p->use_range) {
        const f32 t = ctx.rng->next_f32();
        scl->x = lerp(p->s0[0], p->s1[0], t);
        scl->y = lerp(p->s0[1], p->s1[1], t);
    } else {
        scl->x = p->s0[0];
        scl->y = p->s0[1];
    }
}

constexpr param_hint set_scale_hints[] = {
    { "scale", "v2", "[w,h]", "[1,1]" },
    // scale_end 不给 default：存在即启用 range 模式（presence-sensitive）
    { "scale_end", "v2", "optional end (random lerp)" },
};

// GravityForce -----------------------------------------------------------
struct gravity_params {
    f32 g[3]{ 0.f, -9.81f, 0.f };
};

bool gravity_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (gravity_params*)out;
    const json::value* v = o.find("gravity");
    if (!v) return true;   // 默认 -Y 9.81
    if (!v->is_array() || v->array_size() != 3) {
        err = "'gravity' must be [x,y,z]";
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        const json::value* e = v->array_at(i);
        if (!e->is_number()) {
            err = "'gravity' elements must be numbers";
            return false;
        }
        p->g[i] = e->as_f32();
    }
    return true;
}

void gravity_exec(sim_context& ctx, const void* params) {
    auto* p = (const gravity_params*)params;
    math::v4* vel = ctx.store->v4_channel(attr::velocity) + ctx.row;
    vel->x += p->g[0] * ctx.dt;
    vel->y += p->g[1] * ctx.dt;
    vel->z += p->g[2] * ctx.dt;
}

constexpr param_hint gravity_hints[] = { { "gravity", "v3", "acceleration m/s^2 (default -Y 9.81)", "[0,-9.81,0]" } };

// DragForce --------------------------------------------------------------
struct drag_params {
    param_ref coefficient{ u32_invalid_id, 0.f };
};

bool drag_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (drag_params*)out;
    return get_param_ref_field(o, "coefficient", param_ref{ u32_invalid_id, 0.f }, p->coefficient,
                               parse_env_get().exposed, err);
}

void drag_exec(sim_context& ctx, const void* params) {
    auto* p = (const drag_params*)params;
    const f32 c = resolve_param(p->coefficient, ctx);
    if (c <= 0.f) return;
    const f32 damp = c * ctx.dt >= 1.f ? 0.f : 1.f - c * ctx.dt;
    math::v4* vel = ctx.store->v4_channel(attr::velocity) + ctx.row;
    vel->x *= damp;
    vel->y *= damp;
    vel->z *= damp;
}

constexpr param_hint drag_hints[] = { { "coefficient", "param_ref", "linear drag 1/s", "0" } };

// ---- over-life 曲线数据结构（POD，键数上限 8，线性插值 + 端点钳制）----------

struct float_curve_pod {
    u32 n{ 0 };
    f32 t[8];
    f32 v[8];
    f32 eval(f32 u) const {
        if (n == 0) return 0.f;
        if (u <= t[0]) return v[0];
        if (u >= t[n - 1]) return v[n - 1];
        for (u32 i = 0; i + 1 < n; ++i) {
            if (u >= t[i] && u <= t[i + 1]) {
                const f32 k = (t[i + 1] - t[i]) > 1e-6f ? (u - t[i]) / (t[i + 1] - t[i]) : 0.f;
                return v[i] + (v[i + 1] - v[i]) * k;
            }
        }
        return v[n - 1];
    }
};

struct rgba_curve_pod {
    u32 n{ 0 };
    f32 t[8];
    f32 c[8][4];
    math::v4 eval(f32 u) const {
        if (n == 0) return math::v4{ 1.f, 1.f, 1.f, 1.f };
        if (u <= t[0]) return math::v4{ c[0][0], c[0][1], c[0][2], c[0][3] };
        if (u >= t[n - 1]) return math::v4{ c[n-1][0], c[n-1][1], c[n-1][2], c[n-1][3] };
        for (u32 i = 0; i + 1 < n; ++i) {
            if (u >= t[i] && u <= t[i + 1]) {
                const f32 k = (t[i + 1] - t[i]) > 1e-6f ? (u - t[i]) / (t[i + 1] - t[i]) : 0.f;
                return math::v4{ c[i][0] + (c[i+1][0] - c[i][0]) * k,
                                 c[i][1] + (c[i+1][1] - c[i][1]) * k,
                                 c[i][2] + (c[i+1][2] - c[i][2]) * k,
                                 c[i][3] + (c[i+1][3] - c[i][3]) * k };
            }
        }
        return math::v4{ 1.f, 1.f, 1.f, 1.f };
    }
};

struct vec3_curve_pod {
    u32 n{ 0 };
    f32 t[8];
    f32 v[8][3];
    math::v3 eval(f32 u) const {
        if (n == 0) return math::v3{ 0.f, 0.f, 0.f };
        if (u <= t[0]) return math::v3{ v[0][0], v[0][1], v[0][2] };
        if (u >= t[n - 1]) return math::v3{ v[n-1][0], v[n-1][1], v[n-1][2] };
        for (u32 i = 0; i + 1 < n; ++i) {
            if (u >= t[i] && u <= t[i + 1]) {
                const f32 k = (t[i + 1] - t[i]) > 1e-6f ? (u - t[i]) / (t[i + 1] - t[i]) : 0.f;
                return math::v3{ v[i][0] + (v[i+1][0] - v[i][0]) * k,
                                 v[i][1] + (v[i+1][1] - v[i][1]) * k,
                                 v[i][2] + (v[i+1][2] - v[i][2]) * k };
            }
        }
        return math::v3{ 0.f, 0.f, 0.f };
    }
};

bool parse_json_number(const json::value& v, f32& out) {
    if (!v.is_number()) return false;
    out = v.as_f32();
    return true;
}

// "curve": [[t, v], ...]
bool parse_float_curve(const json::value& o, const char* key, float_curve_pod& out, std::string& err) {
    const json::value* arr = o.find(key);
    if (!arr || !arr->is_array()) {
        err = std::string("missing '") + key + "' curve array";
        return false;
    }
    if (arr->array_size() < 2 || arr->array_size() > 8) {
        err = std::string("'") + key + "' needs 2..8 keys";
        return false;
    }
    out.n = (u32)arr->array_size();
    for (u32 i = 0; i < out.n; ++i) {
        const json::value* pair = arr->array_at(i);
        if (!pair->is_array() || pair->array_size() != 2 ||
            !parse_json_number(*pair->array_at(0), out.t[i]) ||
            !parse_json_number(*pair->array_at(1), out.v[i])) {
            err = std::string("'") + key + "' keys must be [t, v] number pairs";
            return false;
        }
    }
    for (u32 i = 1; i < out.n; ++i) {
        if (out.t[i] <= out.t[i - 1]) { err = std::string("'") + key + "' t must be strictly increasing"; return false; }
    }
    return true;
}

// "gradient": [[t, r, g, b, a], ...]
bool parse_rgba_curve(const json::value& o, const char* key, rgba_curve_pod& out, std::string& err) {
    const json::value* arr = o.find(key);
    if (!arr || !arr->is_array()) {
        err = std::string("missing '") + key + "' gradient array";
        return false;
    }
    if (arr->array_size() < 2 || arr->array_size() > 8) {
        err = std::string("'") + key + "' needs 2..8 keys";
        return false;
    }
    out.n = (u32)arr->array_size();
    for (u32 i = 0; i < out.n; ++i) {
        const json::value* pair = arr->array_at(i);
        if (!pair->is_array() || pair->array_size() < 4 || pair->array_size() > 5) {
            err = std::string("'") + key + "' keys must be [t, r, g, b] or [t, r, g, b, a]";
            return false;
        }
        if (!parse_json_number(*pair->array_at(0), out.t[i])) {
            err = std::string("'") + key + "' t must be a number";
            return false;
        }
        for (int c = 0; c < 4; ++c) {
            if (!parse_json_number(*pair->array_at(1 + c), out.c[i][c])) {
                err = std::string("'") + key + "' channels must be numbers";
                return false;
            }
        }
        if (pair->array_size() == 5 && !parse_json_number(*pair->array_at(4), out.c[i][3])) {
            return false;
        }
        if (pair->array_size() == 4) out.c[i][3] = 1.f;
    }
    for (u32 i = 1; i < out.n; ++i) {
        if (out.t[i] <= out.t[i - 1]) { err = std::string("'") + key + "' t must be strictly increasing"; return false; }
    }
    return true;
}

// "curve": [[t, x, y, z], ...]
bool parse_vec3_curve(const json::value& o, const char* key, vec3_curve_pod& out, std::string& err) {
    const json::value* arr = o.find(key);
    if (!arr || !arr->is_array()) {
        err = std::string("missing '") + key + "' curve array";
        return false;
    }
    if (arr->array_size() < 2 || arr->array_size() > 8) {
        err = std::string("'") + key + "' needs 2..8 keys";
        return false;
    }
    out.n = (u32)arr->array_size();
    for (u32 i = 0; i < out.n; ++i) {
        const json::value* pair = arr->array_at(i);
        if (!pair->is_array() || pair->array_size() != 4) {
            err = std::string("'") + key + "' keys must be [t, x, y, z]";
            return false;
        }
        if (!parse_json_number(*pair->array_at(0), out.t[i])) {
            err = std::string("'") + key + "' t must be a number";
            return false;
        }
        for (int c = 0; c < 3; ++c) {
            if (!parse_json_number(*pair->array_at(1 + c), out.v[i][c])) {
                err = std::string("'") + key + "' components must be numbers";
                return false;
            }
        }
    }
    for (u32 i = 1; i < out.n; ++i) {
        if (out.t[i] <= out.t[i - 1]) { err = std::string("'") + key + "' t must be strictly increasing"; return false; }
    }
    return true;
}

f32 curve_age_norm(sim_context& ctx) {
    const math::v4* pos = ctx.store->v4_channel(attr::position) + ctx.row;
    const math::v4* vel = ctx.store->v4_channel(attr::velocity) + ctx.row;
    const f32 lifetime = vel->w > 0.f ? vel->w : 1.f;
    f32 u = pos->w / lifetime;
    return u < 0.f ? 0.f : (u > 1.f ? 1.f : u);
}

// ---- ScaleOverLife（update）：scale.xy = curve(t) --------------------------------

struct scale_over_life_params {
    float_curve_pod curve;
};

bool scale_over_life_parse(const json::value& o, void* out, std::string& err) {
    return parse_float_curve(o, "curve", *(float_curve_pod*)out, err);
}

void scale_over_life_exec(sim_context& ctx, const void* params) {
    auto* p = (const scale_over_life_params*)params;
    const f32 s = p->curve.eval(curve_age_norm(ctx));
    math::v4* scl = ctx.store->v4_channel(attr::scale_rotation) + ctx.row;
    scl->x = s;
    scl->y = s;
}

constexpr param_hint scale_over_life_hints[] = {
    { "curve", "curve", "[[t, v]...] uniform scale over normalized age" },
};

// ---- ColorOverLife（update）：color = gradient(t) -------------------------------

struct color_over_life_params {
    rgba_curve_pod gradient;
};

bool color_over_life_parse(const json::value& o, void* out, std::string& err) {
    return parse_rgba_curve(o, "gradient", *(rgba_curve_pod*)out, err);
}

void color_over_life_exec(sim_context& ctx, const void* params) {
    auto* p = (const color_over_life_params*)params;
    const math::v4 c = p->gradient.eval(curve_age_norm(ctx));
    math::v4* col = ctx.store->v4_channel(attr::color) + ctx.row;
    col->x = c.x; col->y = c.y; col->z = c.z; col->w = c.w;
}

constexpr param_hint color_over_life_hints[] = {
    { "gradient", "curve", "[[t, r, g, b, a]...]" },
};

// ---- AlphaOverLife（update）：color.a = curve(t) --------------------------------

struct alpha_over_life_params {
    float_curve_pod curve;
};

bool alpha_over_life_parse(const json::value& o, void* out, std::string& err) {
    return parse_float_curve(o, "curve", *(float_curve_pod*)out, err);
}

void alpha_over_life_exec(sim_context& ctx, const void* params) {
    auto* p = (const alpha_over_life_params*)params;
    math::v4* col = ctx.store->v4_channel(attr::color) + ctx.row;
    col->w = p->curve.eval(curve_age_norm(ctx));
}

constexpr param_hint alpha_over_life_hints[] = {
    { "curve", "curve", "[[t, alpha]...]" },
};

// ---- RotationOverLife（update）：scale_rotation.z = curve(t) ---------------------

struct rotation_over_life_params {
    float_curve_pod curve;
};

bool rotation_over_life_parse(const json::value& o, void* out, std::string& err) {
    return parse_float_curve(o, "curve", *(float_curve_pod*)out, err);
}

void rotation_over_life_exec(sim_context& ctx, const void* params) {
    auto* p = (const rotation_over_life_params*)params;
    math::v4* scl = ctx.store->v4_channel(attr::scale_rotation) + ctx.row;
    scl->z = p->curve.eval(curve_age_norm(ctx));
}

constexpr param_hint rotation_over_life_hints[] = {
    { "curve", "curve", "[[t, radians]...]" },
};

// ---- ForceOverLife（update）：velocity += curve(t) * dt -------------------------

struct force_over_life_params {
    vec3_curve_pod curve;
};

bool force_over_life_parse(const json::value& o, void* out, std::string& err) {
    return parse_vec3_curve(o, "curve", *(vec3_curve_pod*)out, err);
}

void force_over_life_exec(sim_context& ctx, const void* params) {
    auto* p = (const force_over_life_params*)params;
    const math::v3 f = p->curve.eval(curve_age_norm(ctx));
    math::v4* vel = ctx.store->v4_channel(attr::velocity) + ctx.row;
    vel->x += f.x * ctx.dt;
    vel->y += f.y * ctx.dt;
    vel->z += f.z * ctx.dt;
}

constexpr param_hint force_over_life_hints[] = {
    { "curve", "curve", "[[t, x, y, z]...] acceleration m/s^2" },
};

// ---- AnimateUv（update）：uv_params.x = 图集帧推进 ------------------------------

struct animate_uv_params {
    f32 frame_rate{ 10.f };   // 帧每秒
    u32 frames{ 4 };          // 图集总帧数
    u8 loop{ 1 };
};

bool animate_uv_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (animate_uv_params*)out;
    if (!get_f32_opt(o, "frame_rate", 10.f, p->frame_rate, err)) return false;
    const json::value* v = o.find("frames");
    if (v && v->is_number()) p->frames = v->as_u32();
    const json::value* lp = o.find("loop");
    if (lp && lp->is_bool()) p->loop = lp->as_bool() ? 1u : 0u;
    if (p->frames == 0) p->frames = 1;
    return true;
}

void animate_uv_exec(sim_context& ctx, const void* params) {
    auto* p = (const animate_uv_params*)params;
    const math::v4* pos = ctx.store->v4_channel(attr::position) + ctx.row;
    const math::v4* vel = ctx.store->v4_channel(attr::velocity) + ctx.row;
    const f32 lifetime = vel->w > 0.f ? vel->w : 1.f;
    f32 age = pos->w;
    if (p->loop) age = std::fmod(age, lifetime);
    f32 frame = age * p->frame_rate;
    if (frame >= (f32)p->frames) frame = (f32)(p->frames - 1);
    math::v4* uv = ctx.store->v4_channel(attr::uv_params) + ctx.row;
    uv->x = frame;
    uv->y = p->frame_rate > 0.f ? std::fmod(age * p->frame_rate, 1.f) : 0.f;
}

constexpr param_hint animate_uv_hints[] = {
    { "frame_rate", "f32", "atlas frames per second", "10" },
    { "frames", "u32", "total atlas frames", "4" },
    { "loop", "bool", "loop over emitter lifetime", "true" },
};

// IntegrateVelocity ------------------------------------------------------// IntegrateVelocity ------------------------------------------------------
// 半隐式欧拉位置积分：p += v·dt。显式模块——用户控制其在堆栈中的位置
// （力 → 积分 → 碰撞）。未放置则粒子不受速度位移（纯"力场标记"语义）。
struct integrate_velocity_params {
    u8 reserved{ 0 };
};

void integrate_velocity_exec(sim_context& ctx, const void* /*params*/) {
    math::v4* pos = ctx.store->v4_channel(attr::position) + ctx.row;
    math::v4* vel = ctx.store->v4_channel(attr::velocity) + ctx.row;
    pos->x += vel->x * ctx.dt;
    pos->y += vel->y * ctx.dt;
    pos->z += vel->z * ctx.dt;
}

// KillAge ----------------------------------------------------------------
struct kill_age_params {
    param_ref normalized_max{ u32_invalid_id, 1.f };
};

bool kill_age_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (kill_age_params*)out;
    return get_param_ref_field(o, "normalized_max", param_ref{ u32_invalid_id, 1.f }, p->normalized_max,
                               parse_env_get().exposed, err);
}

void kill_age_exec(sim_context& ctx, const void* params) {
    auto* p = (const kill_age_params*)params;
    const math::v4* pos = ctx.store->v4_channel(attr::position) + ctx.row;
    const math::v4* vel = ctx.store->v4_channel(attr::velocity) + ctx.row;
    const f32 lifetime = vel->w > 0.f ? vel->w : 1.f;
    if (pos->w / lifetime >= resolve_param(p->normalized_max, ctx)) {
        ctx.store->free(ctx.handle);   // death 事件经 store 发出
    }
}

constexpr param_hint kill_age_hints[] = { { "normalized_max", "param_ref", "kill at this lifetime fraction", "1" } };

// PointForce（update）：点引力/斥力（POP Force / Niagara Point Force 对齐）
// strength 符号决定吸/斥；radius > 0 时线性衰减到 0。
struct point_force_params {
    f32 center[3]{ 0.f, 0.f, 0.f };
    param_ref strength{ u32_invalid_id, 10.f };
    param_ref radius{ u32_invalid_id, 0.f };
};

bool point_force_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (point_force_params*)out;
    if (const json::value* v = o.find("center"); v && v->is_array()) {
        if (v->array_size() != 3) { err = "'center' must be [x,y,z]"; return false; }
        for (int i = 0; i < 3; ++i) {
            const json::value* e = v->array_at(i);
            if (!e->is_number()) { err = "'center' elements must be numbers"; return false; }
            p->center[i] = e->as_f32();
        }
    }
    if (!get_param_ref_field(o, "strength", param_ref{ u32_invalid_id, 10.f }, p->strength,
                             parse_env_get().exposed, err)) return false;
    if (!get_param_ref_field(o, "radius", param_ref{ u32_invalid_id, 0.f }, p->radius,
                             parse_env_get().exposed, err)) return false;
    return true;
}

void point_force_exec(sim_context& ctx, const void* params) {
    auto* p = (const point_force_params*)params;
    const math::v4* pos4 = ctx.store->v4_channel(attr::position) + ctx.row;
    const f32 dx = p->center[0] - pos4->x;
    const f32 dy = p->center[1] - pos4->y;
    const f32 dz = p->center[2] - pos4->z;
    const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 1e-4f) return;

    const f32 radius = resolve_param(p->radius, ctx);
    f32 falloff = 1.f;
    if (radius > 0.f) {
        falloff = 1.f - dist / radius;
        if (falloff <= 0.f) return;
    }
    const f32 s = resolve_param(p->strength, ctx) * falloff * ctx.dt / dist;
    math::v4* vel = ctx.store->v4_channel(attr::velocity) + ctx.row;
    vel->x += dx * s;
    vel->y += dy * s;
    vel->z += dz * s;
}

constexpr param_hint point_force_hints[] = {
    { "center", "v3", "attraction/repulsion point", "[0,0,0]" },
    { "strength", "param_ref", "+ attract / - repel (m/s^2 at center)", "10" },
    { "radius", "param_ref", "linear falloff radius; 0 = infinite", "0" },
};

// VortexForce（update）：绕轴涡旋（POP Vortex 对齐）。切向 swirl + 径向内聚。
struct vortex_force_params {
    f32 center[3]{ 0.f, 0.f, 0.f };
    f32 axis[3]{ 0.f, 1.f, 0.f };
    param_ref strength{ u32_invalid_id, 5.f };   // 切向速度增量 (m/s^2)
    param_ref inward{ u32_invalid_id, 0.f };     // 径向内聚 (m/s^2)
};

bool vortex_force_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (vortex_force_params*)out;
    auto parse_v3 = [&](const char* key, f32* dst) -> bool {
        if (const json::value* v = o.find(key); v && v->is_array()) {
            if (v->array_size() != 3) { err = std::string("'") + key + "' must be [x,y,z]"; return false; }
            for (int i = 0; i < 3; ++i) {
                const json::value* e = v->array_at(i);
                if (!e->is_number()) { err = "axis/center elements must be numbers"; return false; }
                dst[i] = e->as_f32();
            }
        }
        return true;
    };
    if (!parse_v3("center", p->center)) return false;
    if (!parse_v3("axis", p->axis)) return false;
    const f32 len = std::sqrt(p->axis[0] * p->axis[0] + p->axis[1] * p->axis[1] + p->axis[2] * p->axis[2]);
    if (len > 1e-6f) {
        p->axis[0] /= len; p->axis[1] /= len; p->axis[2] /= len;
    }
    if (!get_param_ref_field(o, "strength", param_ref{ u32_invalid_id, 5.f }, p->strength,
                             parse_env_get().exposed, err)) return false;
    if (!get_param_ref_field(o, "inward", param_ref{ u32_invalid_id, 0.f }, p->inward,
                             parse_env_get().exposed, err)) return false;
    return true;
}

void vortex_force_exec(sim_context& ctx, const void* params) {
    auto* p = (const vortex_force_params*)params;
    const math::v4* pos4 = ctx.store->v4_channel(attr::position) + ctx.row;
    const f32 rx = pos4->x - p->center[0];
    const f32 ry = pos4->y - p->center[1];
    const f32 rz = pos4->z - p->center[2];
    const f32 rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rlen < 1e-4f) return;

    // 切向 = axis × r（归一化）；径向内聚 = -r̂
    const f32 tx = p->axis[1] * rz - p->axis[2] * ry;
    const f32 ty = p->axis[2] * rx - p->axis[0] * rz;
    const f32 tz = p->axis[0] * ry - p->axis[1] * rx;
    const f32 tlen = std::sqrt(tx * tx + ty * ty + tz * tz);
    if (tlen < 1e-6f) return;

    const f32 swirl = resolve_param(p->strength, ctx);
    const f32 inward = resolve_param(p->inward, ctx);
    if (swirl == 0.f && inward == 0.f) return;

    const f32 inv_r = 1.f / rlen;
    const f32 inv_t = 1.f / tlen;
    math::v4* vel = ctx.store->v4_channel(attr::velocity) + ctx.row;
    vel->x += (tx * inv_t * swirl - rx * inv_r * inward) * ctx.dt;
    vel->y += (ty * inv_t * swirl - ry * inv_r * inward) * ctx.dt;
    vel->z += (tz * inv_t * swirl - rz * inv_r * inward) * ctx.dt;
}

constexpr param_hint vortex_force_hints[] = {
    { "center", "v3", "vortex center", "[0,0,0]" },
    { "axis", "v3", "swirl axis (normalized internally)", "[0,1,0]" },
    { "strength", "param_ref", "tangential acceleration (m/s^2)", "5" },
    { "inward", "param_ref", "radial pull toward axis (m/s^2)", "0" },
};

// BoundsWrap（update）：越界回绕（POP Limit/Wrap 对齐）。逐轴环绕 min..max。
struct bounds_wrap_params {
    f32 min[3]{ -10.f, -10.f, -10.f };
    f32 max[3]{ 10.f, 10.f, 10.f };
};

bool bounds_wrap_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (bounds_wrap_params*)out;
    auto parse_v3 = [&](const char* key, f32* dst) -> bool {
        if (const json::value* v = o.find(key); v && v->is_array()) {
            if (v->array_size() != 3) { err = std::string("'") + key + "' must be [x,y,z]"; return false; }
            for (int i = 0; i < 3; ++i) {
                const json::value* e = v->array_at(i);
                if (!e->is_number()) { err = "min/max elements must be numbers"; return false; }
                dst[i] = e->as_f32();
            }
        }
        return true;
    };
    return parse_v3("min", p->min) && parse_v3("max", p->max);
}

void bounds_wrap_exec(sim_context& ctx, const void* params) {
    auto* p = (const bounds_wrap_params*)params;
    math::v4* pos4 = ctx.store->v4_channel(attr::position) + ctx.row;
    auto wrap = [](f32 v, f32 lo, f32 hi) {
        const f32 size = hi - lo;
        if (size <= 0.f) return v;
        if (v < lo) return v + size * (f32)((int)((lo - v) / size) + 1);
        if (v > hi) return v - size * (f32)((int)((v - hi) / size) + 1);
        return v;
    };
    pos4->x = wrap(pos4->x, p->min[0], p->max[0]);
    pos4->y = wrap(pos4->y, p->min[1], p->max[1]);
    pos4->z = wrap(pos4->z, p->min[2], p->max[2]);
}

constexpr param_hint bounds_wrap_hints[] = {
    { "min", "v3", "wrap bounds min", "[-10,-10,-10]" },
    { "max", "v3", "wrap bounds max", "[10,10,10]" },
};

// KillSpeed（update）：速度低于阈值即回收（落定灰尘/烟尘收尾，POP Kill 对齐）
struct kill_speed_params {
    param_ref threshold{ u32_invalid_id, 0.1f };   // m/s
};

bool kill_speed_parse(const json::value& o, void* out, std::string& err) {
    auto* p = (kill_speed_params*)out;
    return get_param_ref_field(o, "threshold", param_ref{ u32_invalid_id, 0.1f }, p->threshold,
                               parse_env_get().exposed, err);
}

void kill_speed_exec(sim_context& ctx, const void* params) {
    auto* p = (const kill_speed_params*)params;
    const math::v4* vel = ctx.store->v4_channel(attr::velocity) + ctx.row;
    const f32 speed2 = vel->x * vel->x + vel->y * vel->y + vel->z * vel->z;
    const f32 t = resolve_param(p->threshold, ctx);
    if (speed2 < t * t) ctx.store->free(ctx.handle);   // death 事件经 store 发出
}

constexpr param_hint kill_speed_hints[] = {
    { "threshold", "param_ref", "kill when speed drops below (m/s)", "0.1" },
};

// ---- 注册 --------------------------------------------------------------------

template <typename T>
void reg(module_registry& r, const char* name, stage_kind stage, param_parse_fn parse, void* exec,
         const param_hint* hints, u32 hint_count, size_t state_size = 0) {
    module_def def;
    def.name = name;
    def.stage = stage;
    def.params_size = sizeof(T);
    def.state_size = state_size;
    def.init_defaults = [](void* out) { new (out) T(); };
    def.parse = parse;
    def.hints = hints;
    def.hint_count = hint_count;
    if (stage == stage_kind::spawn) def.spawn_exec = (spawn_exec_fn)exec;
    else def.row_exec = (row_exec_fn)exec;
    r.register_module(def);
}

} // namespace

void register_builtin_modules() {
    static bool done = false;
    if (done) return;
    done = true;

    register_field_modules();   // N4 场与边界模块

    module_registry& r = module_registry::get();

    reg<spawn_rate_params>(r, "SpawnRate", stage_kind::spawn, spawn_rate_parse, (void*)spawn_rate_exec,
                           spawn_rate_hints, 1);
    reg<spawn_burst_params>(r, "SpawnBurst", stage_kind::spawn, spawn_burst_parse, (void*)spawn_burst_exec,
                            spawn_burst_hints, 2, sizeof(spawn_burst_state));
    reg<shape_location_params>(r, "ShapeLocation", stage_kind::init, shape_location_parse,
                               (void*)shape_location_exec, shape_location_hints, 5);
    reg<add_velocity_params>(r, "AddVelocity", stage_kind::init, add_velocity_parse,
                             (void*)add_velocity_exec, add_velocity_hints, 5);
    reg<set_lifetime_params>(r, "SetLifetime", stage_kind::init, set_lifetime_parse,
                             (void*)set_lifetime_exec, set_lifetime_hints, 2);
    reg<set_color_params>(r, "SetColor", stage_kind::init, set_color_parse, (void*)set_color_exec,
                          set_color_hints, 2);
    reg<set_scale_params>(r, "SetScale", stage_kind::init, set_scale_parse, (void*)set_scale_exec,
                          set_scale_hints, 2);
    reg<gravity_params>(r, "GravityForce", stage_kind::update, gravity_parse, (void*)gravity_exec,
                        gravity_hints, 1);
    reg<drag_params>(r, "DragForce", stage_kind::update, drag_parse, (void*)drag_exec, drag_hints, 1);
    {
        module_def def;
        def.name = "IntegrateVelocity";
        def.stage = stage_kind::update;
        def.params_size = sizeof(integrate_velocity_params);
        def.init_defaults = [](void* out) { new (out) integrate_velocity_params(); };
        def.row_exec = integrate_velocity_exec;
        r.register_module(def);
    }
    reg<kill_age_params>(r, "KillAge", stage_kind::update, kill_age_parse, (void*)kill_age_exec,
                         kill_age_hints, 1);
    reg<scale_over_life_params>(r, "ScaleOverLife", stage_kind::update, scale_over_life_parse,
                                (void*)scale_over_life_exec, scale_over_life_hints, 1);
    reg<color_over_life_params>(r, "ColorOverLife", stage_kind::update, color_over_life_parse,
                                (void*)color_over_life_exec, color_over_life_hints, 1);
    reg<alpha_over_life_params>(r, "AlphaOverLife", stage_kind::update, alpha_over_life_parse,
                                (void*)alpha_over_life_exec, alpha_over_life_hints, 1);
    reg<rotation_over_life_params>(r, "RotationOverLife", stage_kind::update, rotation_over_life_parse,
                                   (void*)rotation_over_life_exec, rotation_over_life_hints, 1);
    reg<force_over_life_params>(r, "ForceOverLife", stage_kind::update, force_over_life_parse,
                                (void*)force_over_life_exec, force_over_life_hints, 1);
    reg<animate_uv_params>(r, "AnimateUv", stage_kind::update, animate_uv_parse,
                           (void*)animate_uv_exec, animate_uv_hints, 3);
    reg<point_force_params>(r, "PointForce", stage_kind::update, point_force_parse,
                            (void*)point_force_exec, point_force_hints, 3);
    reg<vortex_force_params>(r, "VortexForce", stage_kind::update, vortex_force_parse,
                             (void*)vortex_force_exec, vortex_force_hints, 4);
    reg<bounds_wrap_params>(r, "BoundsWrap", stage_kind::update, bounds_wrap_parse,
                            (void*)bounds_wrap_exec, bounds_wrap_hints, 2);
    reg<kill_speed_params>(r, "KillSpeed", stage_kind::update, kill_speed_parse,
                           (void*)kill_speed_exec, kill_speed_hints, 1);
}

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
