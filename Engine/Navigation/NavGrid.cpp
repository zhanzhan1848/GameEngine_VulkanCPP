// NavGrid.cpp — 高度场导航网格：烘焙管线 / 查询 / 序列化（ai-navigation S1）。
//
// 烘焙管线（顺序固定，结果确定性）：
//   三角光栅化（XZ 列裁剪 + 平面 Y 极值取裁剪多边形顶点）→ 列内 span 排序合并
//   （重叠/间隙 < cell_height 合并，不可走标志在合并时获胜）→ 净空过滤（span 上方
//   间隙 < agent.height 判不可走）→ 半径腐蚀（8 邻域 chebyshev 距离场两遍松弛建）
//   → 连通域编号（8 邻域 + 台阶谓词）。
// 查询：最近可走面投影 → 连通域预检 → A*（span 图，惰性删除开集 + 插入序平手裁决）
//   → 视线拉直（列中心采样，陡坡折返时退化为逐点）。
// 序列化：确定性文本，只持久化可走 span（查询侧不消费不可走 span）。

#include "Navigation/NavGrid.h"
#include "Common/CoreLogStream.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <vector>

namespace primal::navigation {

namespace {

// ---- 可移植向量小工具（只依赖分量与运算符，Apple simd 与 Win 结构体通用）----
inline f32 dist2_xz(const math::v3& a, const math::v3& b)
{
    const f32 dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}
inline f32 dist3(const math::v3& a, const math::v3& b)
{
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}
inline bool finite3(const math::v3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

constexpr f32 k_nav_pi = 3.14159265358979323846f;
constexpr f32 k_top_eps = 1e-4f;         // 台阶谓词容差
constexpr u32 k_dist_max = 0xFFFFFFFEu;
constexpr f32 k_area_eps = 1e-12f;

// ---- FNV-1a 64 ----
void fnv_mix(u64& h, const void* data, size_t bytes)
{
    const u8* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
}
void fnv_f32(u64& h, f32 v) { u32 b; memcpy(&b, &v, 4); fnv_mix(h, &b, 4); }
void fnv_u32(u64& h, u32 v) { fnv_mix(h, &v, 4); }

// ---- A* 开放集（order 为插入序，保证确定性平手裁决）----
struct open_node {
    f32 f;
    u32 order;
    u32 span;
};
struct open_greater {
    bool operator()(const open_node& a, const open_node& b) const
    {
        if (a.f != b.f) return a.f > b.f;   // priority_queue 取最小 f
        return a.order > b.order;
    }
};

// ---- 8 邻域方向（固定扫描序 = 确定性）----
constexpr s32 k_dirs[8][2] = {
    { -1, 0 }, { 0, -1 }, { 1, 0 }, { 0, 1 },
    { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 },
};

// span → 所在列（span_offsets 有序，二分 upper_bound-1）
u32 column_of_span(const nav_grid& g, u32 span)
{
    u32 lo = 0, hi = g.width * g.depth;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo + 1) / 2;
        if (mid <= g.width * g.depth && g.span_offsets[mid] <= span) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

// 邻列兼容可走 span（|顶差|<=step；多候选取顶差最小，固定扫描序=确定性）
bool neighbor_span(const nav_grid& g, u32 cx, u32 cz, u32 from_span, s32 dx, s32 dz, u32& out_n)
{
    const s32 nx = (s32)cx + dx, nz = (s32)cz + dz;
    if (nx < 0 || nz < 0 || nx >= (s32)g.width || nz >= (s32)g.depth) return false;
    const u32 nc = (u32)nz * g.width + (u32)nx;
    const f32 from_top = g.spans[from_span].ymax;
    f32 best = FLT_MAX;
    u32 best_i = UINT32_MAX;
    for (u32 i = g.span_offsets[nc]; i < g.span_offsets[nc + 1]; ++i) {
        if (!g.spans[i].walkable) continue;
        const f32 d = fabsf(g.spans[i].ymax - from_top);
        if (d <= g.profile.step_height + k_top_eps && d < best) {
            best = d;
            best_i = i;
        }
    }
    if (best_i == UINT32_MAX) return false;
    out_n = best_i;
    return true;
}

// XZ 视线（Amanatides-Woo 超覆盖 + 列中心采样 y；容差=台阶高）。
bool segment_walkable(const nav_grid& g, const math::v3& a, const math::v3& b)
{
    const f32 fx = (a.x - g.bmin.x) / g.cell_size, fz = (a.z - g.bmin.z) / g.cell_size;
    const f32 gx = (b.x - g.bmin.x) / g.cell_size, gz = (b.z - g.bmin.z) / g.cell_size;
    s32 cx = std::clamp((s32)floorf(fx), 0, (s32)g.width - 1);
    s32 cz = std::clamp((s32)floorf(fz), 0, (s32)g.depth - 1);
    const s32 ex = std::clamp((s32)floorf(gx), 0, (s32)g.width - 1);
    const s32 ez = std::clamp((s32)floorf(gz), 0, (s32)g.depth - 1);
    const f32 dx = gx - fx, dz = gz - fz;
    const f32 len2 = dx * dx + dz * dz;
    const s32 sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0), sz = dz > 0 ? 1 : (dz < 0 ? -1 : 0);
    f32 t_max_x = sx != 0 ? (((sx > 0 ? cx + 1 : cx) - fx) / dx) : FLT_MAX;
    f32 t_max_z = sz != 0 ? (((sz > 0 ? cz + 1 : cz) - fz) / dz) : FLT_MAX;
    const f32 t_dx = sx != 0 ? fabsf(1.f / dx) : FLT_MAX;
    const f32 t_dz = sz != 0 ? fabsf(1.f / dz) : FLT_MAX;
    const f32 tol = std::max(g.profile.step_height, g.cell_height * 2.f);

    for (int guard = 0; guard < 1000000; ++guard) {
        if (cx < 0 || cz < 0 || cx >= (s32)g.width || cz >= (s32)g.depth) return false;
        const math::v3 center = g.cell_center((u32)cx, (u32)cz, 0.f);
        f32 t = 0.f;
        if (len2 > 1e-12f) {
            const f32 ux = b.x - a.x, uz = b.z - a.z;
            t = ((center.x - a.x) * ux + (center.z - a.z) * uz) / len2;
            t = std::clamp(t, 0.f, 1.f);
        }
        const f32 y = a.y + (b.y - a.y) * t;
        const s32 s = g.span_at((u32)cz * g.width + (u32)cx, y, tol);
        if (s < 0 || !g.spans[(u32)s].walkable) return false;
        if (cx == ex && cz == ez) break;
        if (t_max_x < t_max_z) {
            if (t_max_x > 1.f) break;
            cx += sx;
            t_max_x += t_dx;
        } else {
            if (t_max_z > 1.f) break;
            cz += sz;
            t_max_z += t_dz;
        }
    }
    return true;
}

// 最近可走 span：切比雪夫环内全扫，代价 = 水平距离 + 0.5×高差；平手取扫描序小者。
nav_error find_nearest_walkable(const nav_grid& g, const math::v3& p, f32 max_radius,
                                math::v3& out_pos, u32& out_span)
{
    if (g.empty()) return nav_error::no_nav_data;
    if (g.walkable_span_count == 0) return nav_error::agent_too_large;

    u32 cx0 = 0, cz0 = 0;
    if (g.world_to_column(p, cx0, cz0)) {
        // 列内
    } else {
        cx0 = std::clamp((u32)std::max(0, (s32)floorf((p.x - g.bmin.x) / g.cell_size)), 0u, g.width - 1);
        cz0 = std::clamp((u32)std::max(0, (s32)floorf((p.z - g.bmin.z) / g.cell_size)), 0u, g.depth - 1);
    }

    f32 best_cost = FLT_MAX;
    u32 best_span = UINT32_MAX;
    math::v3 best_pos{};
    u32 own_cx = UINT32_MAX, own_cz = UINT32_MAX;
    g.world_to_column(p, own_cx, own_cz);   // 查询点所在列（越界则保持 UINT32_MAX）
    // 层分离：只认"包含查询高度"的 span（±tol_y），避免把楼板下的目标投到屋顶上。
    const f32 tol_y = std::max(g.profile.step_height, 0.5f);
    const u32 max_r = std::min((u32)floorf(max_radius / g.cell_size), std::max(g.width, g.depth));
    for (u32 r = 0; r <= max_r; ++r) {
        const s32 x_lo = std::max(0, (s32)cx0 - (s32)r), x_hi = std::min((s32)g.width - 1, (s32)cx0 + (s32)r);
        const s32 z_lo = std::max(0, (s32)cz0 - (s32)r), z_hi = std::min((s32)g.depth - 1, (s32)cz0 + (s32)r);
        for (s32 cz = z_lo; cz <= z_hi; ++cz) {
            for (s32 cx = x_lo; cx <= x_hi; ++cx) {
                if (r > 0 && cx > x_lo && cx < x_hi && cz > z_lo && cz < z_hi) continue; // 只扫环
                const u32 c = (u32)cz * g.width + (u32)cx;
                for (u32 i = g.span_offsets[c]; i < g.span_offsets[c + 1]; ++i) {
                    if (!g.spans[i].walkable) continue;
                    if (p.y < g.spans[i].ymin - tol_y || p.y > g.spans[i].ymax + tol_y) continue;
                    const math::v3 sp = g.cell_center((u32)cx, (u32)cz, g.spans[i].ymax);
                    const f32 horiz = sqrtf(dist2_xz(p, sp));
                    if (horiz > max_radius) continue;
                    const f32 cost = horiz + 0.5f * fabsf(p.y - g.spans[i].ymax);
                    if (cost < best_cost) {
                        best_cost = cost;
                        best_span = i;
                        // 查询点就在所选列内：投影保留精确 x/z（终点=目标本身，0.1m 到达合同）
                        best_pos = (own_cx == (u32)cx && own_cz == (u32)cz)
                            ? math::v3{ p.x, g.spans[i].ymax, p.z }
                            : sp;
                    }
                }
            }
        }
    }
    if (best_span == UINT32_MAX) return nav_error::invalid_target;
    out_pos = best_pos;
    out_span = best_span;
    return nav_error::ok;
}

// ---- 序列化词法游标（空白分隔 token）----
struct cursor {
    const char* p;
    const char* end;
    bool ok{ true };

    bool token(const char** out_beg, size_t* out_len)
    {
        if (!ok) return false;
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
        if (p >= end) return false;
        const char* b = p;
        while (p < end && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') ++p;
        *out_beg = b;
        *out_len = (size_t)(p - b);
        return true;
    }
    bool expect(const char* lit)
    {
        const char* b; size_t l;
        if (!token(&b, &l) || l != strlen(lit) || strncmp(b, lit, l) != 0) {
            ok = false;
            return false;
        }
        return true;
    }
    bool get_f32(f32& v)
    {
        const char* b; size_t l;
        if (!token(&b, &l)) { ok = false; return false; }
        char buf[64];
        if (l >= sizeof(buf)) { ok = false; return false; }
        memcpy(buf, b, l);
        buf[l] = 0;
        char* e = nullptr;
        v = strtof(buf, &e);
        if (e == buf || !std::isfinite(v)) { ok = false; return false; }
        return true;
    }
    bool get_u32(u32& v)
    {
        const char* b; size_t l;
        if (!token(&b, &l)) { ok = false; return false; }
        char buf[32];
        if (l >= sizeof(buf)) { ok = false; return false; }
        memcpy(buf, b, l);
        buf[l] = 0;
        char* e = nullptr;
        const unsigned long long ull = strtoull(buf, &e, 10);
        if (e == buf || ull > 0xFFFFFFFFull) { ok = false; return false; }
        v = (u32)ull;
        return true;
    }
    bool get_u64(u64& v)
    {
        const char* b; size_t l;
        if (!token(&b, &l)) { ok = false; return false; }
        char buf[64];
        if (l >= sizeof(buf)) { ok = false; return false; }
        memcpy(buf, b, l);
        buf[l] = 0;
        char* e = nullptr;
        v = strtoull(buf, &e, 10);
        if (e == buf) { ok = false; return false; }
        return true;
    }
};

void fmt_append(std::string& s, const char* fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    s += buf;
}

} // anonymous namespace

// ---------------------------------------------------------------- 网格基础

bool nav_grid::world_to_column(const math::v3& p, u32& cx, u32& cz) const
{
    if (p.x < bmin.x || p.x > bmax.x || p.z < bmin.z || p.z > bmax.z) return false;
    const f32 fx = (p.x - bmin.x) / cell_size;
    const f32 fz = (p.z - bmin.z) / cell_size;
    s32 ix = (s32)floorf(fx), iz = (s32)floorf(fz);
    ix = std::clamp(ix, 0, (s32)width - 1);
    iz = std::clamp(iz, 0, (s32)depth - 1);
    cx = (u32)ix;
    cz = (u32)iz;
    return true;
}

s32 nav_grid::span_at(u32 column, f32 y, f32 tol) const
{
    const u32 beg = span_offsets[column], end = span_offsets[column + 1];
    s32 best = -1;
    f32 best_d = tol;
    for (u32 i = beg; i < end; ++i) {
        const f32 d = fabsf(spans[i].ymax - y);
        if (d <= best_d) {
            best_d = d;
            best = (s32)i;
        }
    }
    return best;
}

u64 hash_bake_input(const bake_input& in, const agent_profile& profile)
{
    u64 h = 1469598103934665603ull;
    fnv_u32(h, in.position_count);
    for (u32 i = 0; i < in.position_count * 3; ++i) fnv_f32(h, in.positions[i]);
    fnv_u32(h, in.index_count);
    for (u32 i = 0; i < in.index_count; ++i) fnv_u32(h, in.indices[i]);
    fnv_f32(h, in.cell_size);
    fnv_f32(h, in.cell_height);
    fnv_f32(h, profile.radius);
    fnv_f32(h, profile.height);
    fnv_f32(h, profile.max_slope_deg);
    fnv_f32(h, profile.step_height);
    return h;
}

// ---------------------------------------------------------------- 烘焙

nav_error bake(const bake_input& in, const agent_profile& profile, nav_grid& out)
{
    out = nav_grid{};

    if (!in.positions || !in.indices) return nav_error::null_input;
    if (in.position_count < 3 || in.index_count < 3 || (in.index_count % 3) != 0)
        return nav_error::null_input;
    if (!std::isfinite(in.cell_size) || in.cell_size <= 0.f) return nav_error::null_input;
    if (!std::isfinite(in.cell_height) || in.cell_height <= 0.f) return nav_error::null_input;
    if (!profile.valid()) return nav_error::null_input;
    for (u32 i = 0; i < in.index_count; ++i)
        if (in.indices[i] >= in.position_count) return nav_error::null_input;
    for (u32 i = 0; i < in.position_count; ++i) {
        const math::v3 p{ in.positions[i * 3], in.positions[i * 3 + 1], in.positions[i * 3 + 2] };
        if (!finite3(p)) return nav_error::null_input;
    }

    // 包围盒（XZ 外扩 padding；Y 保留几何原值）
    math::v3 bmin{ FLT_MAX, FLT_MAX, FLT_MAX }, bmax{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (u32 i = 0; i < in.position_count; ++i) {
        const math::v3 p{ in.positions[i * 3], in.positions[i * 3 + 1], in.positions[i * 3 + 2] };
        bmin.x = std::min(bmin.x, p.x); bmin.y = std::min(bmin.y, p.y); bmin.z = std::min(bmin.z, p.z);
        bmax.x = std::max(bmax.x, p.x); bmax.y = std::max(bmax.y, p.y); bmax.z = std::max(bmax.z, p.z);
    }
    const f32 pad = std::isfinite(in.bounds_padding) && in.bounds_padding > 0.f ? in.bounds_padding : 0.5f;
    bmin.x -= pad; bmin.z -= pad;
    bmax.x += pad; bmax.z += pad;

    out.bmin = bmin;
    out.bmax = bmax;
    out.profile = profile;
    out.cell_size = in.cell_size;
    out.cell_height = in.cell_height;
    out.width  = std::max(1u, (u32)ceilf((bmax.x - bmin.x) / in.cell_size));
    out.depth  = std::max(1u, (u32)ceilf((bmax.z - bmin.z) / in.cell_size));

    std::vector<std::vector<grid_span>> cols((size_t)out.width * out.depth);

    // ---- 1. 列中心采样 + 绕向实体化 ----
    // 竖直面在 XZ 投影零面积，表面光栅化无法看见墙体；改为每列收集所有非竖直
    // 三角在列中心的穿越点：上朝向面(ny>0)=进入其下方实体，下朝向面(ny<0)=离开。
    // 降序配对成实心 span（未闭合的进入向下钳制——开放地板的正确语义）。
    // 列中心永远不落在列边界上 → 天然无边界串扰；竖直面不产生穿越 → 无毒化。
    struct crossing {
        f32 y;
        u8  dir;     // 1 = IN（上朝向），0 = OUT（下朝向）
        bool walk;   // 该表面的坡度可走性（|ny|/area >= cos(maxSlope)）
    };
    std::vector<std::vector<crossing>> xcols((size_t)out.width * out.depth);
    const f32 walkable_cos = cosf(profile.max_slope_deg * k_nav_pi / 180.f);
    for (u32 t = 0; t < in.index_count; t += 3) {
        const math::v3 p0{ in.positions[in.indices[t] * 3],     in.positions[in.indices[t] * 3 + 1],     in.positions[in.indices[t] * 3 + 2] };
        const math::v3 p1{ in.positions[in.indices[t + 1] * 3], in.positions[in.indices[t + 1] * 3 + 1], in.positions[in.indices[t + 1] * 3 + 2] };
        const math::v3 p2{ in.positions[in.indices[t + 2] * 3], in.positions[in.indices[t + 2] * 3 + 1], in.positions[in.indices[t + 2] * 3 + 2] };

        const math::v3 e1 = p1 - p0, e2 = p2 - p0;
        const f32 nx = e1.y * e2.z - e1.z * e2.y;
        const f32 ny = e1.z * e2.x - e1.x * e2.z;
        const f32 nz = e1.x * e2.y - e1.y * e2.x;
        const f32 area2 = sqrtf(nx * nx + ny * ny + nz * nz);
        if (area2 < k_area_eps) continue;            // 退化三角
        if (fabsf(ny) < 1e-9f * area2) continue;     // 竖直面：不产生列中心穿越

        const bool walk_tri = fabsf(ny) / area2 >= walkable_cos;
        const u8 dir = ny > 0.f ? 1 : 0;

        const s32 cx0 = std::max((s32)floorf((std::min({ p0.x, p1.x, p2.x }) - bmin.x) / in.cell_size), 0);
        const s32 cx1 = std::min((s32)floorf((std::max({ p0.x, p1.x, p2.x }) - bmin.x) / in.cell_size), (s32)out.width - 1);
        const s32 cz0 = std::max((s32)floorf((std::min({ p0.z, p1.z, p2.z }) - bmin.z) / in.cell_size), 0);
        const s32 cz1 = std::min((s32)floorf((std::max({ p0.z, p1.z, p2.z }) - bmin.z) / in.cell_size), (s32)out.depth - 1);

        for (s32 cz = cz0; cz <= cz1; ++cz) {
            for (s32 cx = cx0; cx <= cx1; ++cx) {
                // 列中心点的重心坐标（边函数，符号归一兼容两种绕序；含边界容差）
                const f32 px = bmin.x + (cx + 0.5f) * in.cell_size;
                const f32 pz = bmin.z + (cz + 0.5f) * in.cell_size;
                const f32 ab = (p1.x - p0.x) * (pz - p0.z) - (p1.z - p0.z) * (px - p0.x);
                const f32 bc = (p2.x - p1.x) * (pz - p1.z) - (p2.z - p1.z) * (px - p1.x);
                const f32 ca = (p0.x - p2.x) * (pz - p2.z) - (p0.z - p2.z) * (px - p2.x);
                const f32 s = ab + bc + ca;
                if (fabsf(s) < 1e-20f) continue;
                const f32 w0 = bc / s, w1 = ca / s, w2 = ab / s;
                const f32 bary_eps = -1e-5f;
                if (w0 < bary_eps || w1 < bary_eps || w2 < bary_eps) continue;   // 中心在三角形外
                const f32 y = w0 * p0.y + w1 * p1.y + w2 * p2.y;
                xcols[(size_t)cz * out.width + cx].push_back(crossing{ y, dir, walk_tri });
            }
        }
    }

    // ---- 2. 每列配对成实心 span（y 降序；同 y 先 OUT 后 IN——盒底+地板贴合场景）----
    const u32 col_count = out.width * out.depth;
    const f32 y_floor = bmin.y - 1.f;
    for (u32 c = 0; c < col_count; ++c) {
        auto& xs = xcols[c];
        std::sort(xs.begin(), xs.end(), [](const crossing& a, const crossing& b) {
            if (a.y != b.y) return a.y > b.y;
            return a.dir < b.dir;   // OUT(0) 先于 IN(1)
        });
        bool inside = false;
        f32 top = 0.f;
        bool walk = false;
        for (size_t i = 0; i < xs.size(); ++i) {
            // 去重：同 y 同向（两三角共享对角线）
            if (i > 0 && xs[i].dir == xs[i - 1].dir && fabsf(xs[i].y - xs[i - 1].y) < 1e-6f) continue;
            if (xs[i].dir == 1) {
                if (!inside) {
                    inside = true;
                    top = xs[i].y;
                    walk = xs[i].walk;
                }
                // 已在实体内的新 IN：保守忽略（异常绕序输入）
            } else {
                if (inside) {
                    cols[c].push_back(grid_span{ xs[i].y, top, walk, 0 });
                    inside = false;
                }
            }
        }
        if (inside) {
            // 未闭合：开放地板，向下钳制
            cols[c].push_back(grid_span{ y_floor, top, walk, 0 });
        }
    }

    // ---- 3. 列内排序 + 合并（重叠/间隙 < cell_height；不可走在合并中获胜）----
    out.span_offsets.assign((size_t)col_count + 1, 0);
    for (u32 c = 0; c < col_count; ++c) {
        auto& col = cols[c];
        std::sort(col.begin(), col.end(), [](const grid_span& a, const grid_span& b) {
            return a.ymin < b.ymin;
        });
        for (size_t i = 0; i < col.size();) {
            grid_span merged = col[i];
            size_t j = i + 1;
            while (j < col.size() && col[j].ymin <= merged.ymax + in.cell_height) {
                merged.ymax = std::max(merged.ymax, col[j].ymax);
                merged.walkable = merged.walkable && col[j].walkable;
                ++j;
            }
            out.spans.push_back(merged);
            i = j;
        }
        out.span_offsets[c + 1] = (u32)out.spans.size();
    }

    // ---- 3. 净空过滤：可走 span 上方间隙 < agent.height → 不可走 ----
    for (u32 c = 0; c < col_count; ++c) {
        const u32 beg = out.span_offsets[c], end = out.span_offsets[c + 1];
        for (u32 i = beg; i + 1 < end; ++i) {
            if (out.spans[i].walkable && (out.spans[i + 1].ymin - out.spans[i].ymax) < profile.height)
                out.spans[i].walkable = false;
        }
    }

    // ---- 4. 半径腐蚀：span 级距离场两遍松弛 ----
    // 邻列贡献规则（层分离，防止"墙列屋顶可走"污染地面层距离）：
    //   有兼容可走 span（顶差≤台阶）→ 取其最小 dist；
    //   否则有不可走 span 与角色身体带 [top-step, top+height] 垂直重叠 → 障碍(0)；
    //   空列/仅不相关层 → 中性（不限制，开放边缘不腐蚀）。
    {
        const size_t n = out.spans.size();
        std::vector<u32> dist(n, 0);
        for (size_t i = 0; i < n; ++i) dist[i] = out.spans[i].walkable ? k_dist_max : 0;

        const auto neighbor_dist = [&](u32 cx, u32 cz, f32 my_top) -> u32 {
            const u32 c = cz * out.width + cx;
            u32 best = k_dist_max;
            bool has_compat_walk = false;
            for (u32 i = out.span_offsets[c]; i < out.span_offsets[c + 1]; ++i) {
                const grid_span& s2 = out.spans[i];
                if (s2.walkable && fabsf(s2.ymax - my_top) <= profile.step_height + k_top_eps) {
                    has_compat_walk = true;
                    best = std::min(best, dist[i]);
                }
            }
            if (has_compat_walk) return best;
            // 障碍：与角色身体带 [top-step, top+height] 垂直重叠、且不是本层兼容可走面的
            // 任何 span（含墙体的屋顶层 span——对地面层而言不可站立即障碍）。
            const f32 band_lo = my_top - profile.step_height, band_hi = my_top + profile.height;
            for (u32 i = out.span_offsets[c]; i < out.span_offsets[c + 1]; ++i) {
                const grid_span& s2 = out.spans[i];
                if (s2.ymin <= band_hi && s2.ymax >= band_lo) return 0;
            }
            return k_dist_max;   // 空列/仅不相关层 → 中性（开放边缘不腐蚀）
        };
        const s32 fwd_dirs[4][2] = { { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 } };
        const s32 bwd_dirs[4][2] = { { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 } };
        const auto relax_pass = [&](const s32 (*dirs)[2]) {
            for (s32 pass_row = 0; pass_row < (s32)out.depth; ++pass_row) {
                const s32 cz = (dirs == fwd_dirs) ? pass_row : (s32)out.depth - 1 - pass_row;
                for (s32 pass_col = 0; pass_col < (s32)out.width; ++pass_col) {
                    const s32 cx = (dirs == fwd_dirs) ? pass_col : (s32)out.width - 1 - pass_col;
                    const u32 c = (u32)cz * out.width + (u32)cx;
                    for (u32 i = out.span_offsets[c]; i < out.span_offsets[c + 1]; ++i) {
                        if (!out.spans[i].walkable) continue;
                        u32 d = dist[i];
                        for (int k = 0; k < 4; ++k) {
                            const s32 nx = cx + dirs[k][0], nz = cz + dirs[k][1];
                            if (nx < 0 || nz < 0 || nx >= (s32)out.width || nz >= (s32)out.depth) continue;
                            d = std::min(d, 1u + neighbor_dist((u32)nx, (u32)nz, out.spans[i].ymax));
                        }
                        dist[i] = d;
                    }
                }
            }
        };
        relax_pass(fwd_dirs);
        relax_pass(bwd_dirs);

        const u32 required = (u32)ceilf(profile.radius / in.cell_size);
        for (size_t i = 0; i < n; ++i)
            if (out.spans[i].walkable && dist[i] < required) out.spans[i].walkable = false;
    }

    // ---- 5. 连通域编号（8 邻域 + 台阶谓词；显式栈避免深递归）----
    {
        out.region_count = 0;
        std::vector<u32> stack;
        for (u32 seed = 0; seed < out.spans.size(); ++seed) {
            if (!out.spans[seed].walkable || out.spans[seed].region != 0) continue;
            const u16 region = (u16)std::min<u32>(out.region_count + 1, 0xFFFEu);
            out.region_count = std::max<u32>(out.region_count, region);
            out.spans[seed].region = region;
            stack.clear();
            stack.push_back(seed);
            while (!stack.empty()) {
                const u32 cur = stack.back();
                stack.pop_back();
                const u32 column = column_of_span(out, cur);
                const u32 cx = column % out.width, cz = column / out.width;
                for (const auto& dir : k_dirs) {
                    const s32 nx = (s32)cx + dir[0], nz = (s32)cz + dir[1];
                    if (nx < 0 || nz < 0 || nx >= (s32)out.width || nz >= (s32)out.depth) continue;
                    const u32 nc = (u32)nz * out.width + (u32)nx;
                    for (u32 i = out.span_offsets[nc]; i < out.span_offsets[nc + 1]; ++i) {
                        if (!out.spans[i].walkable || out.spans[i].region != 0) continue;
                        if (fabsf(out.spans[i].ymax - out.spans[cur].ymax) > profile.step_height + k_top_eps)
                            continue;
                        out.spans[i].region = region;
                        stack.push_back(i);
                    }
                }
            }
        }
    }

    out.walkable_span_count = 0;
    for (const auto& s : out.spans)
        if (s.walkable) ++out.walkable_span_count;
    out.source_hash = hash_bake_input(in, profile);

    PRIMAL_LOG_STREAM(Info, "Navigation")
        << "[NavGrid] bake: " << out.width << "x" << out.depth << " cols, "
        << out.spans.size() << " spans (" << out.walkable_span_count << " walkable), "
        << out.region_count << " regions, hash=" << out.source_hash << std::endl;
    return nav_error::ok;
}

// ---------------------------------------------------------------- 查询

nav_error nearest_walkable(const nav_grid& g, const math::v3& p, f32 max_radius, math::v3& out)
{
    u32 span = 0;
    return find_nearest_walkable(g, p, max_radius, out, span);
}

path_result find_path(const nav_grid& g, const math::v3& from, const math::v3& to, f32 nearest_radius)
{
    path_result res;
    if (g.empty()) {
        res.error = nav_error::no_nav_data;
        return res;
    }
    if (g.walkable_span_count == 0) {
        res.error = nav_error::agent_too_large;
        return res;
    }

    math::v3 from_pos{}, to_pos{};
    u32 from_span = 0, to_span = 0;
    if (nav_error e = find_nearest_walkable(g, from, nearest_radius, from_pos, from_span);
        e != nav_error::ok) {
        res.error = e;
        return res;
    }
    if (nav_error e = find_nearest_walkable(g, to, nearest_radius, to_pos, to_span);
        e != nav_error::ok) {
        res.error = e;
        return res;
    }
    res.region_from = g.spans[from_span].region;
    res.region_to = g.spans[to_span].region;
    if (res.region_from != res.region_to) {
        res.error = nav_error::unreachable;
        return res;
    }

    // ---- A*（span 图；惰性删除开集 + 插入序平手裁决 = 确定性）----
    const size_t n = g.spans.size();
    std::vector<f32> g_score(n, FLT_MAX);
    std::vector<f32> f_score(n, FLT_MAX);
    std::vector<u32> came(n, UINT32_MAX);
    std::priority_queue<open_node, std::vector<open_node>, open_greater> open;
    u32 order_counter = 0;

    const auto stand_point = [&](u32 span) {
        const u32 col = column_of_span(g, span);
        return g.cell_center(col % g.width, col / g.width, g.spans[span].ymax);
    };
    const auto h_cost = [&](u32 span) { return sqrtf(dist2_xz(stand_point(span), to_pos)); };

    g_score[from_span] = 0.f;
    f_score[from_span] = h_cost(from_span);
    open.push({ f_score[from_span], order_counter++, from_span });

    bool found = false;
    while (!open.empty()) {
        const open_node node = open.top();
        open.pop();
        if (node.span == to_span) {
            found = true;
            break;
        }
        if (node.f > f_score[node.span] + 1e-6f) continue;   // 过期开集项
        const u32 col = column_of_span(g, node.span);
        const u32 cx = col % g.width, cz = col / g.width;
        const math::v3 cur_pt = stand_point(node.span);
        for (const auto& dir : k_dirs) {
            u32 nb = 0;
            if (!neighbor_span(g, cx, cz, node.span, dir[0], dir[1], nb)) continue;
            const f32 tentative = g_score[node.span] + dist3(cur_pt, stand_point(nb));
            if (tentative < g_score[nb] - 1e-6f) {
                g_score[nb] = tentative;
                f_score[nb] = tentative + h_cost(nb);
                came[nb] = node.span;
                open.push({ f_score[nb], order_counter++, nb });
            }
        }
    }
    if (!found) {
        res.error = nav_error::internal;   // 同连通域却寻路失败 = 不变量破坏
        return res;
    }

    // ---- 回溯 + 视线拉直（失败时退化为逐点，路径仍有效）----
    std::vector<math::v3> raw;
    for (u32 s = to_span; s != UINT32_MAX; s = came[s]) raw.push_back(stand_point(s));
    std::reverse(raw.begin(), raw.end());

    res.waypoints.push_back(from_pos);
    math::v3 cur = from_pos;
    size_t idx = 0;
    while (idx + 1 < raw.size()) {
        size_t k = raw.size() - 1;
        while (k > idx + 1 && !segment_walkable(g, cur, raw[k])) --k;
        res.waypoints.push_back(raw[k]);
        cur = raw[k];
        idx = k;
    }
    if (raw.size() > 1) {
        res.waypoints.back() = to_pos;      // raw 末点即目标 span 站立点，替换为精确目标投影
    } else {
        res.waypoints.push_back(to_pos);    // 起终点同 span：直接补目标投影
    }
    return res;
}

// ---------------------------------------------------------------- 序列化

nav_error serialize(const nav_grid& g, std::string& out_text)
{
    if (g.empty() || g.walkable_span_count == 0) return nav_error::bad_format;

    out_text.clear();
    fmt_append(out_text, "PRIMALNAV %u\n", nav_grid::nav_version);
    fmt_append(out_text, "hash %llu\n", (unsigned long long)g.source_hash);
    fmt_append(out_text, "profile %.6f %.6f %.6f %.6f\n",
               g.profile.radius, g.profile.height, g.profile.max_slope_deg, g.profile.step_height);
    fmt_append(out_text, "grid %u %u %.6f %.6f\n", g.width, g.depth, g.cell_size, g.cell_height);
    fmt_append(out_text, "bounds %.6f %.6f %.6f %.6f %.6f %.6f\n",
               g.bmin.x, g.bmin.y, g.bmin.z, g.bmax.x, g.bmax.y, g.bmax.z);
    fmt_append(out_text, "summary %u %u\n", g.region_count, g.walkable_span_count);

    const u32 col_count = g.width * g.depth;
    fmt_append(out_text, "columns %u\n", col_count);
    for (u32 c = 0; c < col_count; ++c) {
        u32 k = 0;
        for (u32 i = g.span_offsets[c]; i < g.span_offsets[c + 1]; ++i)
            if (g.spans[i].walkable) ++k;
        fmt_append(out_text, "%u", k);
        for (u32 i = g.span_offsets[c]; i < g.span_offsets[c + 1]; ++i) {
            if (!g.spans[i].walkable) continue;
            fmt_append(out_text, " %.6f %.6f %u", g.spans[i].ymin, g.spans[i].ymax, g.spans[i].region);
        }
        fmt_append(out_text, "\n");
    }
    return nav_error::ok;
}

nav_error deserialize(const char* text, size_t len, nav_grid& out_grid)
{
    out_grid = nav_grid{};
    cursor cur{ text, text + len, true };
    if (!cur.expect("PRIMALNAV")) return nav_error::bad_format;
    u32 version = 0;
    if (!cur.get_u32(version)) return nav_error::bad_format;
    if (version != nav_grid::nav_version) return nav_error::unsupported_version;
    if (!cur.expect("hash")) return nav_error::bad_format;
    if (!cur.get_u64(out_grid.source_hash)) return nav_error::bad_format;

    if (!cur.expect("profile")) return nav_error::bad_format;
    if (!cur.get_f32(out_grid.profile.radius) || !cur.get_f32(out_grid.profile.height)
        || !cur.get_f32(out_grid.profile.max_slope_deg) || !cur.get_f32(out_grid.profile.step_height))
        return nav_error::bad_format;
    if (!cur.expect("grid")) return nav_error::bad_format;
    if (!cur.get_u32(out_grid.width) || !cur.get_u32(out_grid.depth)
        || !cur.get_f32(out_grid.cell_size) || !cur.get_f32(out_grid.cell_height))
        return nav_error::bad_format;
    if (out_grid.width == 0 || out_grid.depth == 0 || out_grid.cell_size <= 0.f
        || out_grid.cell_height <= 0.f)
        return nav_error::bad_format;

    if (!cur.expect("bounds")) return nav_error::bad_format;
    {
        // simd 向量分量是右值：先读临时再赋值
        f32 v[6];
        for (int i = 0; i < 6; ++i)
            if (!cur.get_f32(v[i])) return nav_error::bad_format;
        out_grid.bmin = math::v3{ v[0], v[1], v[2] };
        out_grid.bmax = math::v3{ v[3], v[4], v[5] };
    }

    if (!cur.expect("summary")) return nav_error::bad_format;
    if (!cur.get_u32(out_grid.region_count) || !cur.get_u32(out_grid.walkable_span_count))
        return nav_error::bad_format;

    if (!cur.expect("columns")) return nav_error::bad_format;
    u32 col_count = 0;
    if (!cur.get_u32(col_count) || col_count != out_grid.width * out_grid.depth)
        return nav_error::bad_format;

    out_grid.span_offsets.assign((size_t)col_count + 1, 0);
    u32 total_walkable = 0;
    for (u32 c = 0; c < col_count; ++c) {
        u32 k = 0;
        if (!cur.get_u32(k)) return nav_error::bad_format;
        f32 prev_ymin = -FLT_MAX;
        for (u32 j = 0; j < k; ++j) {
            grid_span s{};
            s.walkable = true;
            u32 region = 0;
            if (!cur.get_f32(s.ymin) || !cur.get_f32(s.ymax) || !cur.get_u32(region))
                return nav_error::bad_format;
            if (s.ymax < s.ymin || s.ymin < prev_ymin || region == 0
                || region > out_grid.region_count || region > 0xFFFFu)
                return nav_error::bad_format;
            prev_ymin = s.ymin;
            s.region = (u16)region;
            out_grid.spans.push_back(s);
            ++total_walkable;
        }
        out_grid.span_offsets[c + 1] = (u32)out_grid.spans.size();
    }
    if (total_walkable != out_grid.walkable_span_count) return nav_error::bad_format;
    return nav_error::ok;
}

} // namespace primal::navigation
