#include "CardSurfaceBake.h"
#include "Graphics/RHI/Core/CardSurfaceIdentity.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace primal::tools::card_surface {

namespace {

// ContentTools inherits fast-math. Inspect exponent bits so malformed NaN/Inf
// inputs cannot be accepted when the optimizer assumes floating values finite.
// Read through a non-inlined reference: a by-value/inlined classification may
// inherit fast-math assumptions from preceding arithmetic (notably NaN tint).
[[gnu::noinline]] bool finite_sample_value(const f32& value)
{
    u32 bits; std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}
[[gnu::noinline]] bool finite_sample_value(const double& value)
{
    u64 bits; std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

// ---- FNV-1a 64（与运行时读者共用同一实现约定）----
u64 fnv_step(u64 h, const u8* p, u64 n)
{
    constexpr u64 kPrime = 0x100000001b3ull;
    for (u64 i = 0; i < n; ++i) { h ^= p[i]; h *= kPrime; }
    return h;
}

// ---- f32 ↔ f16 ----
u16 f32_to_f16(f32 v)
{
    const u32 bits = [v]{ u32 b; std::memcpy(&b, &v, 4); return b; }();
    const u32 sign = (bits >> 16) & 0x8000u;
    const int32_t biased = int32_t((bits >> 23) & 0xFFu);
    const u32 mant = bits & 0x7FFFFFu;
    if (biased == 0xFF) return u16(sign | 0x7C00u | (mant ? 0x200u : 0u));  // inf/nan
    const int32_t exp = biased - 127 + 15;
    if (exp >= 31) return u16(sign | 0x7C00u);   // overflow → inf
    if (exp <= 0)  return u16(sign);             // underflow → 0（页值域远离子正规）
    return u16(sign | (u32(exp) << 10) | (mant >> 13));
}

// ---- 卡片坐标系（与 SurfaceCacheData.glsl 逐约定一致）----
// axis: 深度轴序号；dir: 深度方向符号。u/v = 其余两轴（x,y,z 顺序）。
struct CardFrame
{
    u32 axis, dir;
    u32 u_axis, v_axis;
    f32 f[3];       // 深度单位向量（含 dir 符号）
    f32 u[3], v[3]; // u/v 单位向量
};
CardFrame card_frame(u32 axis_direction)
{
    CardFrame cf{};
    cf.axis = axis_direction & 0xFFu;
    cf.dir = (axis_direction >> 8) & 0xFFu;
    cf.u_axis = (cf.axis == 0u) ? 1u : 0u;
    cf.v_axis = (cf.axis == 2u) ? 1u : 2u;
    for (u32 k = 0; k < 3; ++k) { cf.f[k] = 0.f; cf.u[k] = 0.f; cf.v[k] = 0.f; }
    cf.f[cf.axis] = (cf.dir > 0u) ? 1.f : -1.f;
    cf.u[cf.u_axis] = 1.f;
    cf.v[cf.v_axis] = 1.f;
    return cf;
}

// 运行时分辨率公式（pack_placement）：max(e[u],e[v])·2·texels_per_unit，
// 钳 [max(page,min_res), page·4]，向上对齐页倍数。texels_per_unit 必须与
// 运行时分配一致（静态页分辨率 = 卡片 atlas 分辨率），默认 64。
u32 card_resolution(const f32 extent[3], const CardFrame& cf, const BakeParams& p)
{
    const f32 proj = std::max(extent[cf.u_axis], extent[cf.v_axis]) * 2.0f;
    u32 res = u32(proj * p.texels_per_unit);
    res = std::max(res, std::max(p.page_size, p.min_resolution));
    res = std::min(res, p.page_size * 4u);
    return ((res + p.page_size - 1u) / p.page_size) * p.page_size;
}

// ---- 纹理采样：双线性 + repeat wrap + sRGB→线性 ----
void srgb_to_linear(const u8 srgb, f32& out_linear)
{
    const f32 s = f32(srgb) / 255.0f;
    out_linear = (s <= 0.04045f) ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}
f32 wrap_frac(f32 x)
{
    f32 r = std::fmod(x, 1.0f);
    return (r < 0.0f) ? r + 1.0f : r;
}
void sample_albedo(const RgbaImage& tex, f32 u, f32 v, f32 scale, const f32 tint[3], u8 out_rgba[4])
{
    f32 lin[3]{ tint[0] * scale, tint[1] * scale, tint[2] * scale };
    if (tex.valid && tex.width > 0 && tex.height > 0) {
        // 与运行时一致：纹理 V=0 为首行，capture 在顶点级做 uv.y=1-uv.y 翻转——
        // 翻转并进采样行号：row = (1 - v)·H。
        const f32 fx = wrap_frac(u) * f32(tex.width) - 0.5f;
        const f32 fy = wrap_frac(1.0f - v) * f32(tex.height) - 0.5f;
        const int32_t x0 = int32_t(std::floor(fx)), y0 = int32_t(std::floor(fy));
        const f32 tx = fx - f32(x0), ty = fy - f32(y0);
        f32 acc[3]{ 0.f, 0.f, 0.f };
        for (u32 dy = 0; dy < 2; ++dy) {
            for (u32 dx = 0; dx < 2; ++dx) {
                const int32_t px = (x0 + int32_t(dx) + int32_t(tex.width)) % int32_t(tex.width);
                const int32_t py = (y0 + int32_t(dy) + int32_t(tex.height)) % int32_t(tex.height);
                const u8* s = &tex.rgba[(u64(py) * tex.width + px) * 4u];
                const f32 w = (dx ? tx : 1.f - tx) * (dy ? ty : 1.f - ty);
                for (u32 c = 0; c < 3; ++c) {
                    f32 s_lin;
                    srgb_to_linear(s[c], s_lin);
                    acc[c] += s_lin * w;
                }
            }
        }
        for (u32 c = 0; c < 3; ++c) lin[c] = acc[c] * tint[c] * scale;
    }
    for (u32 c = 0; c < 3; ++c)
        out_rgba[c] = u8(std::clamp(lin[c], 0.0f, 1.0f) * 255.0f + 0.5f);
    out_rgba[3] = 255;
}

// ---- oct 编码（与 CardCapture.frag octEncode 一致，输出 [0,1]²）----
void oct_encode(const f32 n[3], f32 out_e[2])
{
    const f32 l1 = std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]);
    f32 rx = n[0] / l1, ry = n[1] / l1;
    if (n[2] < 0.0f) {
        // 注意保持 shader 语义：(1 - |r.yx|) 的符号取自 r.xy 翻转前各分量。
        const f32 ex = rx, ey = ry;
        rx = (1.0f - std::abs(ey)) * ((ex >= 0.0f) ? 1.0f : -1.0f);
        ry = (1.0f - std::abs(ex)) * ((ey >= 0.0f) ? 1.0f : -1.0f);
    }
    out_e[0] = rx * 0.5f + 0.5f;
    out_e[1] = ry * 0.5f + 0.5f;
}
void store_oct16(std::vector<u8>& plane, u64 texel, const f32 e[2])
{
    // rg16f 位型 = f16 两枚；半精度可精确表示 1/65535 量化值（≤2^-10 量级）
    // 的近邻，存 f16(量化 [0,1]) 与运行时 atlas 数值域一致。
    const u16 h0 = f32_to_f16(e[0]);
    const u16 h1 = f32_to_f16(e[1]);
    plane[texel * 4u + 0] = u8(h0 & 0xFF);
    plane[texel * 4u + 1] = u8(h0 >> 8);
    plane[texel * 4u + 2] = u8(h1 & 0xFF);
    plane[texel * 4u + 3] = u8(h1 >> 8);
}

u32 geom_index_at(const MeshGeometryView& geom, u32 i)
{
    if (geom.index_size == 2) { u16 v; std::memcpy(&v, geom.indices + u64(i) * 2u, 2); return v; }
    u32 v; std::memcpy(&v, geom.indices + u64(i) * 4u, 4); return v;
}

// 顶点法线解包（与 CardCapture.vert unpack_normal 一致）。
void unpack_normal16(const u8* elem_base, f32 out[3])
{
    u16 x, y;
    std::memcpy(&x, elem_base + 4, 2);
    std::memcpy(&y, elem_base + 6, 2);
    const f32 fx = f32(x) * (2.0f / 65535.0f) - 1.0f;
    const f32 fy = f32(y) * (2.0f / 65535.0f) - 1.0f;
    const f32 d = fx * fx + fy * fy;
    const f32 z = std::sqrt(std::max(0.0f, 1.0f - d));
    const u8 signs = elem_base[3];
    const f32 n_sign = ((signs & 0x02u) ? 1.0f : -1.0f);
    out[0] = fx; out[1] = fy; out[2] = z * n_sign;
}

// ---- BVH（中位分裂，叶子 ≤8 三角形；双面、带回重心）----
struct BvhNode { f32 bmin[3]; f32 bmax[3]; u32 start, count, left, right; };
struct TriRef { u32 index; f32 cx, cy, cz; };

class TriBvh
{
public:
    void build(const MeshGeometryView& geom)
    {
        geom_ = geom;
        tris_.clear();
        nodes_.clear();
        tris_.reserve(geom.index_count / 3u);
        for (u32 t = 0; t + 2 < geom.index_count; t += 3) {
            const u32 ia = index_at(t), ib = index_at(t + 1), ic = index_at(t + 2);
            if (ia >= geom.vertex_count || ib >= geom.vertex_count || ic >= geom.vertex_count) continue;
            f32 cx = 0.f, cy = 0.f, cz = 0.f;
            for (const u32 vi : { ia, ib, ic }) {
                cx += geom.positions[u64(vi) * 3u + 0] / 3.0f;
                cy += geom.positions[u64(vi) * 3u + 1] / 3.0f;
                cz += geom.positions[u64(vi) * 3u + 2] / 3.0f;
            }
            tris_.push_back({ t, cx, cy, cz });
        }
        if (tris_.empty()) return;
        nodes_.reserve(tris_.size() * 2u);
        build_range(0, u32(tris_.size()));
    }

    // 最近命中（双面）。命中 → true，out_tri = 首索引（tri_index×3），
    // out_bary = (w0,w1,w2)，out_t = 射线参数（= 卡局部 axis depth）。
    bool nearest_hit(const f32 o[3], const f32 d[3], f32 tmin, f32 tmax,
                     u32& out_tri, f32 out_bary[3], f32& out_t) const
    {
        if (nodes_.empty()) return false;
        f32 best = tmax;
        bool hit = false;
        u32 best_tri = 0;
        f32 best_bary[3]{ 0.f, 0.f, 0.f };
        u32 stack[64];
        u32 sp = 0;
        stack[sp++] = 0;
        while (sp > 0) {
            const BvhNode& n = nodes_[stack[--sp]];
            if (!slab(n, o, d, tmin, best)) continue;
            if (n.left == 0xFFFFFFFFu) {
                for (u32 i = 0; i < n.count; ++i) {
                    f32 t; f32 b[3];
                    if (ray_tri(tris_[n.start + i].index, o, d, tmin, best, t, b)) {
                        if (hit && t == best && tris_[n.start + i].index >= best_tri) continue;
                        best = t;
                        best_tri = tris_[n.start + i].index;
                        best_bary[0] = b[0]; best_bary[1] = b[1]; best_bary[2] = b[2];
                        hit = true;
                    }
                }
            } else {
                stack[sp++] = n.left;
                stack[sp++] = n.right;
            }
        }
        if (hit) {
            out_tri = best_tri;
            out_bary[0] = best_bary[0]; out_bary[1] = best_bary[1]; out_bary[2] = best_bary[2];
            out_t = best;
        }
        return hit;
    }

private:
    u32 index_at(u32 i) const
    {
        if (geom_.index_size == 2) { u16 v; std::memcpy(&v, geom_.indices + size_t(i) * 2u, 2); return v; }
        u32 v; std::memcpy(&v, geom_.indices + size_t(i) * 4u, 4); return v;
    }
    u32 build_range(u32 start, u32 count)
    {
        const u32 node_idx = u32(nodes_.size());
        nodes_.push_back({});
        BvhNode& n = nodes_[node_idx];
        n.start = start; n.count = count;
        n.left = n.right = 0xFFFFFFFFu;
        for (u32 i = start; i < start + count; ++i) expand(n, tris_[i].index);
        if (count <= 8u) return node_idx;
        f32 cmin[3]{ 1e30f, 1e30f, 1e30f }, cmax[3]{ -1e30f, -1e30f, -1e30f };
        for (u32 i = start; i < start + count; ++i) {
            cmin[0] = std::min(cmin[0], tris_[i].cx); cmax[0] = std::max(cmax[0], tris_[i].cx);
            cmin[1] = std::min(cmin[1], tris_[i].cy); cmax[1] = std::max(cmax[1], tris_[i].cy);
            cmin[2] = std::min(cmin[2], tris_[i].cz); cmax[2] = std::max(cmax[2], tris_[i].cz);
        }
        u32 axis = 0;
        if (cmax[1] - cmin[1] > cmax[axis] - cmin[axis]) axis = 1;
        if (cmax[2] - cmin[2] > cmax[axis] - cmin[axis]) axis = 2;
        const u32 mid = start + count / 2u;
        std::nth_element(tris_.begin() + start, tris_.begin() + mid, tris_.begin() + start + count,
                         [axis](const TriRef& a, const TriRef& b) {
                             return (axis == 0 ? a.cx : axis == 1 ? a.cy : a.cz) <
                                    (axis == 0 ? b.cx : axis == 1 ? b.cy : b.cz);
                         });
        n.left = build_range(start, mid - start);
        n.right = build_range(mid, start + count - mid);
        return node_idx;
    }
    void expand(BvhNode& n, u32 tri_index)
    {
        for (u32 k = 0; k < 3; ++k) {
            const u32 vi = index_at(tri_index + k);
            for (u32 c = 0; c < 3; ++c) {
                const f32 val = geom_.positions[u64(vi) * 3u + c];
                n.bmin[c] = std::min(n.bmin[c], val);
                n.bmax[c] = std::max(n.bmax[c], val);
            }
        }
    }
    static bool slab(const BvhNode& n, const f32 o[3], const f32 d[3], f32 tmin, f32 tmax)
    {
        for (u32 c = 0; c < 3; ++c) {
            const f32 inv = 1.0f / d[c];
            f32 t0 = (n.bmin[c] - o[c]) * inv;
            f32 t1 = (n.bmax[c] - o[c]) * inv;
            if (t0 > t1) std::swap(t0, t1);
            if (t0 > tmin) tmin = t0;
            if (t1 < tmax) tmax = t1;
            if (tmin > tmax) return false;
        }
        return true;
    }
    bool ray_tri(u32 tri_index, const f32 o[3], const f32 d[3], f32 tmin, f32 tmax,
                 f32& out_t, f32 out_bary[3]) const
    {
        const u32 ia = index_at(tri_index), ib = index_at(tri_index + 1), ic = index_at(tri_index + 2);
        const f32* a = &geom_.positions[u64(ia) * 3u];
        const f32* b = &geom_.positions[u64(ib) * 3u];
        const f32* c = &geom_.positions[u64(ic) * 3u];
        const f32 e1[3]{ b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const f32 e2[3]{ c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        const f32 p[3]{ d[1] * e2[2] - d[2] * e2[1],
                        d[2] * e2[0] - d[0] * e2[2],
                        d[0] * e2[1] - d[1] * e2[0] };
        const f32 det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::abs(det) < 1e-20f) return false;
        const f32 inv = 1.0f / det;
        const f32 s[3]{ o[0] - a[0], o[1] - a[1], o[2] - a[2] };
        const f32 u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
        if (u < -1e-5f || u > 1.00001f) return false;
        const f32 q[3]{ s[1] * e1[2] - s[2] * e1[1],
                        s[2] * e1[0] - s[0] * e1[2],
                        s[0] * e1[1] - s[1] * e1[0] };
        const f32 v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
        if (v < -1e-5f || u + v > 1.00001f) return false;
        const f32 t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
        if (t < tmin || t > tmax) return false;
        out_t = t;
        out_bary[0] = 1.0f - u - v;
        out_bary[1] = u;
        out_bary[2] = v;
        return true;
    }

    MeshGeometryView geom_{};
    std::vector<TriRef> tris_;
    std::vector<BvhNode> nodes_;
};

}  // namespace

u64 fnv1a64(const void* data, u64 bytes, u64 seed)
{
    return fnv_step(seed, static_cast<const u8*>(data), bytes);
}

bool bake_mesh(const MeshGeometryView& geom, const card_bake::data& cards,
               const RgbaImage& albedo, const BakeParams& params, MeshBakeResult& out)
{
    out.pages.clear();
    out.depth_limit_scale = params.depth_limit_scale;
    if (cards.cards.empty()) return false;
    if (!geom.positions || !geom.indices || (geom.index_size != 2 && geom.index_size != 4)) return false;
    if (params.page_size == 0) return false;

    // 身份摘要（§3.1）：几何 + 卡片布局各自独立指纹。索引字节用 digest
    // 专用原生视图（blob 原始宽度），与运行时 ComputeCardBakeDigests 逐字节
    // 一致——展宽副本会产生不同指纹，静态页将永远无法键控命中。
    {
        const u8* idx_bytes = geom.digest_indices ? geom.digest_indices : geom.indices;
        const u32 idx_size = geom.digest_index_size ? geom.digest_index_size : geom.index_size;
        u64 h = fnv1a64(&geom.vertex_count, sizeof(u32));
        h = fnv1a64(&geom.index_count, sizeof(u32), h);
        h = fnv1a64(geom.positions, u64(geom.vertex_count) * 12u, h);
        h = fnv1a64(idx_bytes, u64(geom.index_count) * idx_size, h);
        h = fnv1a64(geom.elements, u64(geom.vertex_count) * geom.element_stride, h);
        out.geometry_digest = h;
    }
    out.layout_digest = fnv1a64(cards.cards.data(), u64(cards.cards.size()) * sizeof(card_bake::card));
    const f32 effective_tint[3] = {params.tint[0] * params.albedo_scale, params.tint[1] * params.albedo_scale, params.tint[2] * params.albedo_scale};
    out.material_digest = graphics::rhi::card_surface_identity::Material(
        albedo.valid ? graphics::rhi::card_surface_identity::Texture(albedo.width, albedo.height, albedo.rgba.data()) : 0,
        effective_tint, params.roughness, params.metallic);

    TriBvh bvh;
    bvh.build(geom);

    const bool has_uv = (geom.element_stride >= 24u) && geom.elements;

    out.pages.reserve(cards.cards.size());
    for (u32 ci = 0; ci < cards.cards.size(); ++ci) {
        const card_bake::card& bc = cards.cards[ci];
        const CardFrame cf = card_frame(bc.axis_direction);
        const u32 res = card_resolution(bc.extent, cf, params);
        if (res == 0) continue;

        CardPage page;
        page.card_index = ci;
        page.resolution = res;
        const u64 texels = u64(res) * res;
        page.albedo.assign(texels * 4u, 0u);
        page.normal.assign(texels * 4u, 0u);
        page.depth.assign(texels, -1.0f);
        page.params.assign(texels * 4u, 0u);
        page.triangle_ids.assign(texels, 0xffffffffu);

        // capture 对齐：近平面 0.05（PRIMAL_SC_CAPTURE_NEAR 默认），远平面
        // 2·halfAxis + 0.5（SurfaceCachePass captureFar 同式）。
        const f32 t_min = graphics::rhi::card_surface_identity::CaptureNear;
        const f32 t_max = 2.0f * bc.extent[cf.axis] * params.depth_limit_scale + graphics::rhi::card_surface_identity::CaptureFarBias;

        const f32 eu = bc.extent[cf.u_axis];
        const f32 ev = bc.extent[cf.v_axis];

        u32 filled = 0;
        for (u32 py = 0; py < res; ++py) {
            for (u32 px = 0; px < res; ++px) {
                // texel 中心 → 卡平面原点（查询约定：u/v 沿 +u/+v 轴，
                // cardTexelToWorld 同式——capture 的 U 镜像只是光栅对齐机制）。
                const f32 u = (f32(px) + 0.5f) / f32(res);
                const f32 v = (f32(py) + 0.5f) / f32(res);
                const f32 u_off = (u - 0.5f) * eu * 2.0f;
                const f32 v_off = (v - 0.5f) * ev * 2.0f;
                const f32 o[3]{ bc.center[0] + cf.u[0] * u_off + cf.v[0] * v_off,
                                bc.center[1] + cf.u[1] * u_off + cf.v[1] * v_off,
                                bc.center[2] + cf.u[2] * u_off + cf.v[2] * v_off };

                u32 tri_index;
                f32 bary[3];
                f32 t;
                if (!bvh.nearest_hit(o, cf.f, t_min, t_max, tri_index, bary, t)) continue;

                // 命中表面属性 = 插值顶点法线 + 仿射 UV（正交投影下与 capture
                // 的透视矫正一致）。
                f32 acc_n[3]{ 0.f, 0.f, 0.f };
                f32 acc_uv[2]{ 0.f, 0.f };
                if (!has_uv) {
                    // 无顶点法线：几何面法线（capture 侧顶点法线缺失时的诚实回退）。
                    const u32 va = geom_index_at(geom, tri_index + 0);
                    const u32 vb = geom_index_at(geom, tri_index + 1);
                    const u32 vc = geom_index_at(geom, tri_index + 2);
                    const f32* a = &geom.positions[u64(va) * 3u];
                    const f32* b = &geom.positions[u64(vb) * 3u];
                    const f32* c = &geom.positions[u64(vc) * 3u];
                    const f32 e1[3]{ b[0]-a[0], b[1]-a[1], b[2]-a[2] };
                    const f32 e2[3]{ c[0]-a[0], c[1]-a[1], c[2]-a[2] };
                    acc_n[0] = e1[1]*e2[2]-e1[2]*e2[1];
                    acc_n[1] = e1[2]*e2[0]-e1[0]*e2[2];
                    acc_n[2] = e1[0]*e2[1]-e1[1]*e2[0];
                } else {
                    for (u32 k = 0; k < 3; ++k) {
                        const u32 vi = geom_index_at(geom, tri_index + k);
                        if (vi >= geom.vertex_count) continue;
                        const f32 w = bary[k];
                        f32 n3[3];
                        unpack_normal16(geom.elements + u64(vi) * geom.element_stride, n3);
                        for (u32 c = 0; c < 3; ++c) acc_n[c] += n3[c] * w;
                        const u8* eb = geom.elements + u64(vi) * geom.element_stride;
                        f32 uvx, uvy;
                        std::memcpy(&uvx, eb + 16, 4);
                        std::memcpy(&uvy, eb + 20, 4);
                        acc_uv[0] += uvx * w;
                        acc_uv[1] += uvy * w;
                    }
                }
                const f32 nl = std::sqrt(acc_n[0] * acc_n[0] + acc_n[1] * acc_n[1] + acc_n[2] * acc_n[2]);
                if (nl > 1e-8f) for (f32& c : acc_n) c /= nl;
                // capture 同约定（CardCapture.frag）：法线翻成朝卡眼
                //（dot(N, fwd) ≤ 0）——与三角形绕序无关，双面捕获语义一致。
                {
                    const f32 dot_nf = acc_n[0] * cf.f[0] + acc_n[1] * cf.f[1] + acc_n[2] * cf.f[2];
                    if (dot_nf > 0.0f)
                        for (f32& c : acc_n) c = -c;
                }

                const u64 texel = u64(py) * res + px;
                u8 rgba[4];
                sample_albedo(albedo, acc_uv[0], acc_uv[1], params.albedo_scale, params.tint, rgba);
                std::memcpy(&page.albedo[texel * 4u], rgba, 4);
                f32 e[2];
                oct_encode(acc_n, e);
                store_oct16(page.normal, texel, e);
                page.depth[texel] = t;
                page.triangle_ids[texel] = tri_index / 3u;
                page.params[texel * 4u + 0] = params.roughness;
                page.params[texel * 4u + 1] = params.metallic;
                page.params[texel * 4u + 2] = 0;
                page.params[texel * 4u + 3] = 0;
                ++filled;
            }
        }
        page.valid_texels = filled;
        page.hits_total = filled;
        if (params.verbosity > 0) {
            std::fprintf(stderr, "[card-surface] card %u res %u valid %u / %llu (%.1f%%)\n",
                         ci, res, filled, (unsigned long long)texels,
                         100.0 * double(filled) / double(texels));
        }
        out.pages.push_back(std::move(page));
    }
    return !out.pages.empty();
}

// ============================================================================
// SCS1 sidecar 文件 IO
// ============================================================================

bool bake_boundary_samples(const MeshGeometryView& geom, const card_bake::data& cards,
                           const RgbaImage& albedo, const BakeParams& params,
                           const std::vector<u32>& triangle_ids, f32 spacing,
                           u32 max_samples, BoundaryBakeResult& out)
{
    out = {};
    // An explicit per-owner upper bound prevents dense/huge primitives from
    // turning this auxiliary path into an unbounded allocation.
    constexpr u32 kMaxSamples = 1u << 20;
    if (!geom.positions || !geom.indices || !geom.vertex_count ||
        (geom.index_size != 2 && geom.index_size != 4) || geom.index_count % 3 ||
        (geom.element_stride && !geom.elements) || cards.cards.empty() || triangle_ids.size() > kMaxSamples ||
        !finite_sample_value(spacing) || spacing <= 0 || !max_samples || max_samples > kMaxSamples ||
        !finite_sample_value(params.albedo_scale) || params.albedo_scale < 0 ||
        (geom.digest_indices && geom.digest_index_size != 2 && geom.digest_index_size != 4) ||
        (albedo.valid && (!albedo.width || !albedo.height || albedo.width > (1u << 24) || albedo.height > (1u << 24) ||
                         u64(albedo.width) * albedo.height > albedo.rgba.size() / 4u))) return false;
    for (f32 tint : params.tint) if (!finite_sample_value(tint) ||
        !finite_sample_value(tint * params.albedo_scale)) return false;

    auto ids = triangle_ids;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    struct Primitive { u32 id, vertices[3], segments[3]; };
    std::vector<Primitive> primitives;
    u64 count = 0;
    for (u32 id : ids) {
        if (id >= geom.index_count / 3u) return false;
        Primitive primitive{}; primitive.id = id;
        for (u32 k = 0; k < 3; ++k) {
            const u32 vi = geom_index_at(geom, id * 3u + k);
            if (vi >= geom.vertex_count) return false;
            primitive.vertices[k] = vi;
            for (u32 c = 0; c < 3; ++c)
                if (!finite_sample_value(geom.positions[u64(vi) * 3u + c])) return false;
            if (geom.elements && geom.element_stride >= 24) {
                f32 uv[2]; std::memcpy(uv, geom.elements + u64(vi) * geom.element_stride + 16, 8);
                if (!finite_sample_value(uv[0]) || !finite_sample_value(uv[1])) return false;
            }
        }
        const f32* a = geom.positions + u64(primitive.vertices[0]) * 3;
        const f32* b = geom.positions + u64(primitive.vertices[1]) * 3;
        const f32* c = geom.positions + u64(primitive.vertices[2]) * 3;
        double cross[3];
        for (u32 k = 0; k < 3; ++k) {
            const u32 u = (k + 1) % 3, v = (k + 2) % 3;
            cross[k] = (double(b[u])-a[u])*(double(c[v])-a[v]) -
                       (double(b[v])-a[v])*(double(c[u])-a[u]);
        }
        const double area2 = cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2];
        if (!finite_sample_value(area2) || area2 <= 0) return false;
        for (u32 edge = 0; edge < 3; ++edge) {
            const f32* p = geom.positions + u64(primitive.vertices[edge]) * 3;
            const f32* q = geom.positions + u64(primitive.vertices[(edge+1)%3]) * 3;
            double distance2 = 0;
            for (u32 k = 0; k < 3; ++k) { const double d = double(q[k])-p[k]; distance2 += d*d; }
            const double segments = std::max(1.0, std::ceil(std::sqrt(distance2) / spacing));
            if (!finite_sample_value(segments) || segments > max_samples - count) return false;
            primitive.segments[edge] = u32(segments);
            count += primitive.segments[edge];
        }
        primitives.push_back(primitive);
    }

    BoundaryBakeResult result;
    result.triangle_count = geom.index_count / 3u;
    const u8* idx_bytes = geom.digest_indices ? geom.digest_indices : geom.indices;
    const u32 idx_size = geom.digest_index_size ? geom.digest_index_size : geom.index_size;
    u64 h = fnv1a64(&geom.vertex_count, sizeof(u32));
    h = fnv1a64(&geom.index_count, sizeof(u32), h);
    h = fnv1a64(geom.positions, u64(geom.vertex_count) * 12u, h);
    h = fnv1a64(idx_bytes, u64(geom.index_count) * idx_size, h);
    result.geometry_digest = fnv1a64(geom.elements, u64(geom.vertex_count) * geom.element_stride, h);
    result.layout_digest = fnv1a64(cards.cards.data(), u64(cards.cards.size()) * sizeof(card_bake::card));
    const f32 effective_tint[3] = {params.tint[0]*params.albedo_scale, params.tint[1]*params.albedo_scale, params.tint[2]*params.albedo_scale};
    result.material_digest = graphics::rhi::card_surface_identity::Material(
        albedo.valid ? graphics::rhi::card_surface_identity::Texture(albedo.width, albedo.height, albedo.rgba.data()) : 0,
        effective_tint, params.roughness, params.metallic);
    result.samples.reserve(count);
    for (const auto& primitive : primitives) {
        const f32* v[3];
        for (u32 k = 0; k < 3; ++k) v[k] = geom.positions + u64(primitive.vertices[k]) * 3;
        double face[3];
        for (u32 k = 0; k < 3; ++k) {
            const u32 u=(k+1)%3, w=(k+2)%3;
            face[k]=(double(v[1][u])-v[0][u])*(double(v[2][w])-v[0][w])-
                    (double(v[1][w])-v[0][w])*(double(v[2][u])-v[0][u]);
        }
        const double length=std::sqrt(face[0]*face[0]+face[1]*face[1]+face[2]*face[2]);
        for (u32 edge = 0; edge < 3; ++edge) {
            const u32 next=(edge+1)%3, n=primitive.segments[edge];
            // End corner belongs to the next edge. Shared vertices of another
            // original primitive deliberately remain separate identity samples.
            for (u32 i=0; i<n; ++i) {
                const f32 t=f32(i)/f32(n), weights[2]={1.f-t,t};
                const u32 corners[2]={edge,next};
                BoundarySample sample; sample.triangle_id=primitive.id;
                f32 uv[2]{};
                for (u32 k=0; k<3; ++k)
                    sample.position[k]=f32((1.0-double(i)/n)*v[edge][k]+(double(i)/n)*v[next][k]);
                if (geom.elements && geom.element_stride >= 24) {
                    for (u32 j=0; j<2; ++j) {
                        const u8* element=geom.elements+u64(primitive.vertices[corners[j]])*geom.element_stride;
                        f32 normal[3], texcoord[2]; unpack_normal16(element,normal);
                        std::memcpy(texcoord,element+16,8);
                        for (u32 k=0; k<3; ++k) sample.normal[k]+=weights[j]*normal[k];
                        for (u32 k=0; k<2; ++k) uv[k]+=weights[j]*texcoord[k];
                    }
                }
                const f32 nl=std::sqrt(sample.normal[0]*sample.normal[0]+sample.normal[1]*sample.normal[1]+sample.normal[2]*sample.normal[2]);
                for (u32 k=0; k<3; ++k) sample.normal[k]=nl>1e-8f ? sample.normal[k]/nl : f32(face[k]/length);
                if (!finite_sample_value(uv[0]) || !finite_sample_value(uv[1])) return false;
                sample_albedo(albedo,uv[0],uv[1],params.albedo_scale,params.tint,sample.albedo);
                sample.params[0]=params.roughness; sample.params[1]=params.metallic;
                result.samples.push_back(sample);
            }
        }
    }
    out = std::move(result);
    return true;
}

bool write_sidecar(const char* path, const ScsHeader& header,
                   const std::vector<MeshBakeResult>& meshes)
{
    if (!path || meshes.empty()) return false;
    if (header.version != kScsVersion || header.capture_near != graphics::rhi::card_surface_identity::CaptureNear ||
        header.capture_far_bias != graphics::rhi::card_surface_identity::CaptureFarBias) return false;
    for (const auto& mesh : meshes) {
        if (mesh.depth_limit_scale != header.capture_far_scale) return false;
        for (const auto& pg : mesh.pages) {
            const size_t n = size_t(pg.resolution) * pg.resolution;
            if (pg.albedo.size()!=n*4 || pg.normal.size()!=n*4 || pg.depth.size()!=n ||
                pg.params.size()!=n*4 || pg.triangle_ids.size()!=n) return false;
            for(size_t t=0;t<n;++t)
                if ((pg.albedo[t*4+3]>=128) != (pg.triangle_ids[t]!=0xffffffffu)) return false;
        }
    }

    // 布局：header | Σ(mesh record + entries) | 页平面数据。
    struct Pending
    {
        ScsMeshRecord record;
        std::vector<ScsCardPageEntry> entries;
    };
    std::vector<Pending> pending(meshes.size());
    for (size_t m = 0; m < meshes.size(); ++m) {
        Pending& p = pending[m];
        p.record.geometry_digest = meshes[m].geometry_digest;
        p.record.layout_digest = meshes[m].layout_digest;
        p.record.material_digest = meshes[m].material_digest;
        p.record.name_hash = meshes[m].name_hash;
        p.record.card_count = u32(meshes[m].pages.size());
        p.record.pages_offset = 0;
        p.record.pages_bytes = 0;
        p.entries.resize(meshes[m].pages.size());
    }
    u64 records_bytes = 0;
    for (const Pending& p : pending)
        records_bytes += sizeof(ScsMeshRecord) + u64(p.record.card_count) * sizeof(ScsCardPageEntry);
    const u64 pages_region = kScsHeaderBytes + records_bytes;

    std::vector<u8> blob = std::vector<u8>(size_t(pages_region));
    ScsHeader hdr = header;
    hdr.mesh_count = u32(meshes.size());
    hdr.flags |= 3u;  // params 平面存在
    std::memcpy(blob.data(), &hdr, sizeof(hdr));
    u64 w = kScsHeaderBytes;
    u64 page_w = pages_region;
    auto append = [&blob, &w](const void* p, u64 n) {
        if (w + n > blob.size()) blob.resize(size_t(w + n));
        std::memcpy(blob.data() + w, p, n);
        w += n;
    };
    for (size_t m = 0; m < meshes.size(); ++m) {
        Pending& p = pending[m];
        for (size_t c = 0; c < meshes[m].pages.size(); ++c) {
            const CardPage& pg = meshes[m].pages[c];
            ScsCardPageEntry& e = p.entries[c];
            e.card_index = pg.card_index;
            e.resolution = pg.resolution;
            const u64 plane = u64(pg.resolution) * pg.resolution;
            e.albedo_offset = page_w;  page_w += plane * 4u;
            e.normal_offset = page_w;  page_w += plane * 4u;
            e.depth_offset = page_w;   page_w += plane * 4u;
            e.params_offset = page_w;  page_w += plane * 4u;
            e.triangle_offset = page_w; page_w += plane * 4u;
        }
        p.record.pages_offset = p.entries.empty() ? 0u : p.entries.front().albedo_offset;
        p.record.pages_bytes = p.entries.empty() ? 0u : (page_w - p.record.pages_offset);
        append(&p.record, sizeof(p.record));
        if (!p.entries.empty())
            append(p.entries.data(), u64(p.entries.size()) * sizeof(ScsCardPageEntry));
    }
    blob.resize(size_t(page_w));
    for (size_t m = 0; m < meshes.size(); ++m) {
        for (size_t c = 0; c < meshes[m].pages.size(); ++c) {
            const CardPage& pg = meshes[m].pages[c];
            const u64 plane = u64(pg.resolution) * pg.resolution;
            const ScsCardPageEntry& e = pending[m].entries[c];
            std::memcpy(blob.data() + e.albedo_offset, pg.albedo.data(), plane * 4u);
            std::memcpy(blob.data() + e.normal_offset, pg.normal.data(), plane * 4u);
            std::memcpy(blob.data() + e.depth_offset, pg.depth.data(), plane * 4u);
            std::memcpy(blob.data() + e.params_offset, pg.params.data(), plane * 4u);
            std::memcpy(blob.data() + e.triangle_offset, pg.triangle_ids.data(), plane * 4u);
        }
    }

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const size_t wrote = std::fwrite(blob.data(), 1, blob.size(), f);
    std::fclose(f);
    return wrote == blob.size();
}

bool read_sidecar_header(const char* path, ScsHeader& out_header)
{
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    ScsHeader h;
    const size_t got = std::fread(&h, 1, sizeof(h), f);
    std::fclose(f);
    if (got != sizeof(h) || h.magic != kScsMagic || h.version != kScsVersion) return false;
    out_header = h;
    return true;
}

bool read_sidecar_records(const char* path, const ScsHeader& header,
                          std::vector<ScsMeshRecord>& out_meshes,
                          std::vector<std::vector<ScsCardPageEntry>>& out_pages,
                          std::vector<u64>* out_record_file_offsets)
{
    out_meshes.clear();
    out_pages.clear();
    if (out_record_file_offsets) out_record_file_offsets->clear();
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    bool ok = true;
    u64 pos = kScsHeaderBytes;
    for (u32 m = 0; ok && m < header.mesh_count; ++m) {
        ScsMeshRecord rec;
        ok = std::fseek(f, long(pos), SEEK_SET) == 0 &&
             std::fread(&rec, 1, sizeof(rec), f) == sizeof(rec);
        if (ok) {
            out_meshes.push_back(rec);
            if (out_record_file_offsets) out_record_file_offsets->push_back(pos);
            std::vector<ScsCardPageEntry> entries(rec.card_count);
            if (rec.card_count)
                ok = std::fread(entries.data(), sizeof(ScsCardPageEntry), rec.card_count, f) == rec.card_count;
            out_pages.push_back(std::move(entries));
            pos += sizeof(ScsMeshRecord) + u64(rec.card_count) * sizeof(ScsCardPageEntry);
        }
    }
    std::fclose(f);
    if (!ok) { out_meshes.clear(); out_pages.clear(); }
    return ok;
}

}  // namespace primal::tools::card_surface
