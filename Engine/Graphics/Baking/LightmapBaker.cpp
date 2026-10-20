// Lightmap 烘焙器（2026-10-19 计划，M2）。
//
// CPU 路径烘焙管线（契约见 LightmapBaker.h）：
//   0 collect  —— 校验 mesh（必须有 lightmap UV），算世界面积
//   1 pack     —— 纹素需求 → MaxRects → per-mesh 矩形 + scale/bias
//   2 raster   —— UV 空间逐三角形栅格化：每有效纹素存世界 pos/normal/albedo
//   3 build    —— 全场景世界三角形 BVH（utl::BVH）
//   4 bounce0  —— 直射（阴影射线）+ 自发光 → 0 阶出射（仅作迭代源，不落盘）
//   5 bounces  —— 每纹素 N 条余弦加权半球射线 gather 上一阶出射；
//                 ray miss = 天空辐射。k 阶出射累入最终产物（Indirect-Only）
//   6 filter   —— 5×5 联合双边（可选）
//   7 dilate   —— N 迭代 3×3 最近值扩张（矩形内）
//   8 encode   —— f16 → LMAP 落盘 + per-mesh 清单
//
// 确定性：逐纹素 RNG 种子 = hash(texel, bounce)，同输入同输出。
// 并行：JobSystem 运行时 ParallelFor，否则串行回退（单测/无引擎环境）。
// 进度回调可能来自 worker 线程——调用方自行marshal。

#include "LightmapBaker.h"
#include "LightmapAtlasPacker.h"
#include "LightmapPagePng.h"

#include "CommonHeaders.h"
#include "Graphics/Utilities/BVH.hpp"
#include "JobSystem/JobSystem.h"
#include "Engine/Common/CoreLogStream.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <filesystem>
#include <cstring>
#include <cstdio>
#include <mutex>

namespace primal::graphics::baking {

LightmapBakeSettings DefaultLightmapBakeSettings() {
    LightmapBakeSettings s{};
    s.atlas_size = 2048;
    s.samples_per_texel = 64;   // spp 基线（用户定调：路径追踪 64spp）
    s.bounces = 3;
    s.texel_density = 16.0f;
    s.denoise = 1;
    s.dilation_px = 8;
    s.output_dir[0] = '\0';
    return s;
}

namespace {

// ---------- 小工具 ----------

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

inline Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 Mul(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Len(const Vec3& a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Norm(const Vec3& a) {
    const float l = Len(a);
    return l > 1e-12f ? Scale(a, 1.0f / l) : Vec3{0, 1, 0};
}
inline Vec3 Mix(const Vec3& a, const Vec3& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

inline math::v3 ToV3(const Vec3& a) { return math::v3{a.x, a.y, a.z}; }

// f32 → f16（位级，与 LightmapModule 的 FloatToHalf 同式）。
inline u16 FloatToHalfBits(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const std::int32_t exp = static_cast<std::int32_t>((x >> 23) & 0xFFu) - 127 + 15;
    const std::uint32_t mant = x & 0x007FFFFFu;
    if (exp <= 0) return static_cast<u16>(sign);
    if (exp >= 31) return static_cast<u16>(sign | 0x7C00u);
    return static_cast<u16>(sign | (static_cast<std::uint32_t>(exp) << 10) | (mant >> 13));
}

// 确定性 RNG（xorshift32）。
struct Rng {
    std::uint32_t s;
    explicit Rng(std::uint32_t seed) : s(seed ? seed : 1u) {}
    std::uint32_t Next() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return s;
    }
    float NextF() { return (Next() & 0xFFFFFF) / 16777216.0f; }
};

inline std::uint32_t HashTexel(std::uint32_t texel, std::uint32_t salt) {
    std::uint32_t h = texel * 2654435761u;
    h ^= salt * 40503u;
    h ^= h >> 13;
    return h ? h : 1u;
}

// 余弦加权半球方向（局部 z-up）。
inline Vec3 CosineHemisphere(Rng& rng) {
    const float u1 = rng.NextF(), u2 = rng.NextF();
    const float r = std::sqrt(u1);
    const float phi = 2.0f * 3.14159265f * u2;
    Vec3 local{r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - u1))};
    // 正交基（n 任意）由调用方构建——这里返回局部系方向。
    return local;
}
inline void Onb(const Vec3& n, Vec3& t, Vec3& b) {
    const Vec3 up = std::abs(n.z) < 0.999f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    t = Norm(Cross(up, n));
    b = Cross(n, t);
}

// ---------- 每纹素栅格化数据 ----------

struct TexelSample {
    Vec3 pos;
    Vec3 normal;
    float albedo[3];
    std::uint32_t mesh_index = 0xFFFFFFFFu;
    bool valid = false;
};

// ---------- 光照求值 ----------

inline Vec3 SkyRadiance(const BakeSky& sky, const Vec3& dir) {
    if (dir.y <= 0.0f) return {0, 0, 0};
    const float t = std::min(1.0f, dir.y);
    const Vec3 c = Mix({sky.horizon[0], sky.horizon[1], sky.horizon[2]},
                       {sky.zenith[0], sky.zenith[1], sky.zenith[2]}, t);
    return Scale(c, sky.intensity);
}

// 直射辐照度（含阴影射线）。origin 已沿法线偏移。
Vec3 DirectIrradiance(const BakeScene& scene, const utl::BVH& bvh,
                      const Vec3& pos, const Vec3& n) {
    Vec3 e{0, 0, 0};
    for (const auto& light : scene.lights) {
        if (light.type == BakeLight::Type::Directional) {
            const Vec3 dir{light.direction[0], light.direction[1], light.direction[2]};
            const Vec3 L = Norm(Scale(dir, -1.0f));
            const float ndotl = Dot(n, L);
            if (ndotl <= 0.0f) continue;
            const utl::BVH::Ray ray(ToV3(pos), ToV3(L));
            if (bvh.IntersectAny(ray, 1e30f)) continue;
            e = Add(e, Scale({light.color[0], light.color[1], light.color[2]},
                             light.intensity * ndotl));
        } else {
            const Vec3 lp{light.position[0], light.position[1], light.position[2]};
            const Vec3 toL = Sub(lp, pos);
            const float d = Len(toL);
            if (d < 1e-4f || d >= light.range) continue;
            const Vec3 L = Scale(toL, 1.0f / d);
            const float ndotl = Dot(n, L);
            if (ndotl <= 0.0f) continue;
            const utl::BVH::Ray ray(ToV3(pos), ToV3(L));
            if (bvh.IntersectAny(ray, d)) continue;
            // 距离衰减：物理 1/d² × 窗口（range 边缘归零）。
            const float g = std::max(d * d, 1e-3f);
            const float w = 1.0f - (d / light.range) * (d / light.range);
            float falloff = (light.intensity / g) * std::max(0.0f, w);
            if (light.type == BakeLight::Type::Spot) {
                const Vec3 axis = Norm(Vec3{light.direction[0], light.direction[1], light.direction[2]});
                const float cosA = Dot(Scale(L, -1.0f), axis);
                const float outer = std::cos(light.cone_outer * 3.14159265f / 180.0f);
                const float inner = std::cos(light.cone_inner * 3.14159265f / 180.0f);
                const float cone = std::min(1.0f, std::max(0.0f, (cosA - outer) / std::max(1e-4f, inner - outer)));
                falloff *= cone;
            }
            e = Add(e, Scale({light.color[0], light.color[1], light.color[2]}, falloff * ndotl));
        }
    }
    return e;
}

// ---------- ParallelFor 包装（JobSystem 可用则并行，否则串行） ----------

template <typename Fn>
void ParallelBlocks(std::uint32_t count, Fn&& fn) {
    if (g_JobSystem && g_JobSystem->IsRunning()) {
        auto h = g_JobSystem->ParallelFor(count, std::forward<Fn>(fn));
        g_JobSystem->Wait(h);
    } else {
        for (std::uint32_t i = 0; i < count; ++i) fn(i);
    }
}

struct Progress {
    LightmapProgressCallback cb = nullptr;
    void* user = nullptr;
    void Report(std::uint32_t phase, std::uint32_t done, std::uint32_t total) const {
        if (cb) cb(user, phase, done, total);
    }
};

} // namespace

LightmapBakeResult BakeLightmaps(const BakeScene& scene,
                                 const LightmapBakeSettings& settings,
                                 LightmapProgressCallback cb, void* cb_user) {
    LightmapBakeResult result;
    Progress progress{cb, cb_user};

    // ---- 0 collect：校验 + 世界面积 ----
    // 语义：带 lightmap UV 的 mesh 进烘焙；缺 UV 的 mesh 仅入 BVH 做遮挡
    //（trace-only——未展开的遮挡物/场景件不需要 atlas 空间）。
    std::vector<const BakeMesh*> allMeshes;
    std::vector<std::uint32_t> bakeable;        // → allMeshes 下标
    std::vector<double> meshAreas;              // 平行于 bakeable
    for (const auto& m : scene.meshes) {
        if (!m.positions || !m.indices || m.index_count < 3 || m.index_count % 3 != 0 ||
            m.vertex_count == 0) {
            result.error = "mesh missing positions/indices: " + m.name;
            return result;
        }
        allMeshes.push_back(&m);
        if (!m.lightmap_uvs) continue;  // trace-only
        double area = 0.0;
        for (std::uint32_t i = 0; i < m.index_count; i += 3) {
            const Vec3 a{m.positions[m.indices[i] * 3], m.positions[m.indices[i] * 3 + 1],
                         m.positions[m.indices[i] * 3 + 2]};
            const Vec3 b{m.positions[m.indices[i + 1] * 3], m.positions[m.indices[i + 1] * 3 + 1],
                         m.positions[m.indices[i + 1] * 3 + 2]};
            const Vec3 c{m.positions[m.indices[i + 2] * 3], m.positions[m.indices[i + 2] * 3 + 1],
                         m.positions[m.indices[i + 2] * 3 + 2]};
            area += 0.5 * Len(Cross(Sub(b, a), Sub(c, a)));
        }
        bakeable.push_back((std::uint32_t)(allMeshes.size() - 1));
        meshAreas.push_back(area);
    }
    if (bakeable.empty()) {
        result.error = "no bakeable meshes (all trace-only)";
        return result;
    }
    progress.Report(0, (std::uint32_t)allMeshes.size(), (std::uint32_t)allMeshes.size());

    // ---- 1 pack：纹素需求 → 矩形 ----
    const std::uint32_t atlasW = settings.atlas_size, atlasH = settings.atlas_size;
    const std::uint32_t margin = settings.dilation_px * 2;  // 两侧各留一倍扩张
    std::vector<PackRequest> reqs;
    reqs.reserve(bakeable.size());
    for (size_t i = 0; i < bakeable.size(); ++i) {
        // 需求纹素数 = 面积 × 密度² × 利用率系数 1.3（chart 间隙补偿）。
        const double texels = meshAreas[i] * (double)settings.texel_density *
                              (double)settings.texel_density * 1.3;
        double side = std::sqrt(texels);
        side = std::max(4.0, std::min((double)atlasW, side));
        reqs.push_back({static_cast<std::uint32_t>(i), (std::uint32_t)side,
                        (std::uint32_t)side});
    }
    MaxRectsPacker packer(atlasW);
    const auto packed = packer.Pack(reqs, margin);
    for (const auto& p : packed) {
        if (!p.placed) {
            result.error = "atlas overflow: mesh " + allMeshes[bakeable[p.id]]->name +
                           " does not fit " + std::to_string(atlasW) + "px page";
            return result;
        }
    }

    // ---- 2 raster：UV 空间栅格化 ----
    std::vector<TexelSample> texels((size_t)atlasW * atlasH);
    // 每 bakeable mesh 的 rect（texel，未含 margin——扩张写进 margin 区）。
    struct MeshRect { std::uint32_t x, y, w, h; };
    std::vector<MeshRect> rects(bakeable.size());
    for (const auto& p : packed) {
        rects[p.id] = {p.x, p.y, p.w, p.h};
        LightmapMeshEntry e;
        e.SetName(allMeshes[bakeable[p.id]]->name);
        e.atlas_index = 0;
        e.scale_x = (float)p.w / (float)atlasW;
        e.scale_y = (float)p.h / (float)atlasH;
        e.bias_x = (float)p.x / (float)atlasW;
        e.bias_y = (float)p.y / (float)atlasH;
        e.rect_x = p.x; e.rect_y = p.y; e.rect_w = p.w; e.rect_h = p.h;
        result.entries.push_back(e);
    }

    {
        const std::uint32_t meshCount = (std::uint32_t)bakeable.size();
        ParallelBlocks(meshCount, [&](std::uint32_t mi) {
            const BakeMesh& m = *allMeshes[bakeable[mi]];
            const MeshRect& r = rects[mi];
            for (std::uint32_t t = 0; t + 2 < m.index_count; t += 3) {
                const std::uint32_t ia = m.indices[t], ib = m.indices[t + 1], ic = m.indices[t + 2];
                const Vec3 pa{m.positions[ia * 3], m.positions[ia * 3 + 1], m.positions[ia * 3 + 2]};
                const Vec3 pb{m.positions[ib * 3], m.positions[ib * 3 + 1], m.positions[ib * 3 + 2]};
                const Vec3 pc{m.positions[ic * 3], m.positions[ic * 3 + 1], m.positions[ic * 3 + 2]};
                const Vec3 faceN = Norm(Cross(Sub(pb, pa), Sub(pc, pa)));
                const float ua = m.lightmap_uvs[ia * 2], va = m.lightmap_uvs[ia * 2 + 1];
                const float ub = m.lightmap_uvs[ib * 2], vb = m.lightmap_uvs[ib * 2 + 1];
                const float uc = m.lightmap_uvs[ic * 2], vc = m.lightmap_uvs[ic * 2 + 1];
                // rect 内 texel 空间坐标（uv∈[0,1] → rect 全域）。
                auto toRect = [&](float u, float v) {
                    return std::pair<float, float>{(float)r.x + u * (float)r.w, (float)r.y + v * (float)r.h};
                };
                const auto [ax, ay] = toRect(ua, va);
                const auto [bx, by] = toRect(ub, vb);
                const auto [cx, cy] = toRect(uc, vc);
                const float minX = std::min({ax, bx, cx}), maxX = std::max({ax, bx, cx});
                const float minY = std::min({ay, by, cy}), maxY = std::max({ay, by, cy});
                const std::int32_t x0 = std::max<std::int32_t>(0, (std::int32_t)std::floor(minX) - 1);
                const std::int32_t x1 = std::min<std::int32_t>((std::int32_t)atlasW - 1, (std::int32_t)std::ceil(maxX) + 1);
                const std::int32_t y0 = std::max<std::int32_t>(0, (std::int32_t)std::floor(minY) - 1);
                const std::int32_t y1 = std::min<std::int32_t>((std::int32_t)atlasH - 1, (std::int32_t)std::ceil(maxY) + 1);
                if (x1 < x0 || y1 < y0) continue;
                const float e0 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);  // 2×面积
                if (std::abs(e0) < 1e-12f) continue;
                for (std::int32_t y = y0; y <= y1; ++y) {
                    for (std::int32_t x = x0; x <= x1; ++x) {
                        // texel 中心 + 0.5px 边缘余量（保守覆盖）。
                        const float px = (float)x + 0.5f, py = (float)y + 0.5f;
                        const float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / e0;
                        const float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / e0;
                        const float w2 = 1.0f - w0 - w1;
                        const float pad = 0.5f / std::sqrt(std::abs(e0));
                        if (w0 < -pad || w1 < -pad || w2 < -pad) continue;
                        const std::uint32_t idx = (std::uint32_t)y * atlasW + (std::uint32_t)x;
                        TexelSample& s = texels[idx];
                        if (s.valid) continue;  // 先到先得（UV 不重叠前提）
                        s.valid = true;
                        s.mesh_index = bakeable[mi];  // allMeshes 索引
                        const Vec3 p = Add(Add(Scale(pa, w0), Scale(pb, w1)), Scale(pc, w2));
                        s.pos = p;
                        // 顶点法线可用则插值，否则面法线。
                        if (m.normals) {
                            const Vec3 na{m.normals[ia * 3], m.normals[ia * 3 + 1], m.normals[ia * 3 + 2]};
                            const Vec3 nb{m.normals[ib * 3], m.normals[ib * 3 + 1], m.normals[ib * 3 + 2]};
                            const Vec3 nc{m.normals[ic * 3], m.normals[ic * 3 + 1], m.normals[ic * 3 + 2]};
                            s.normal = Norm(Add(Add(Scale(na, w0), Scale(nb, w1)), Scale(nc, w2)));
                        } else {
                            s.normal = faceN;
                        }
                        s.albedo[0] = m.albedo[0]; s.albedo[1] = m.albedo[1]; s.albedo[2] = m.albedo[2];
                    }
                }
            }
            progress.Report(1, mi + 1, meshCount);
        });
    }

    // ---- 3 build：全场景 BVH + 三角形→mesh 映射 ----
    utl::vector<math::v3> bvhVerts;
    utl::vector<u32> bvhIndices;
    // mesh = allMeshes 索引；bakeIdx = bakeable 内索引（v2 路径追踪不再区分：
    // 所有命中面都按其材质（albedo/emissive）参与路径）。
    struct TriRef { std::uint32_t mesh, v0, v1, v2, bakeIdx; };
    constexpr std::uint32_t kNoBake = 0xFFFFFFFFu;
    std::vector<TriRef> triRefs;
    std::vector<std::uint32_t> meshToBakeable(allMeshes.size(), kNoBake);
    for (size_t bi = 0; bi < bakeable.size(); ++bi) meshToBakeable[bakeable[bi]] = (std::uint32_t)bi;
    for (std::uint32_t mi = 0; mi < allMeshes.size(); ++mi) {
        const BakeMesh& m = *allMeshes[mi];
        for (std::uint32_t t = 0; t + 2 < m.index_count; t += 3) {
            const std::uint32_t ia = m.indices[t], ib = m.indices[t + 1], ic = m.indices[t + 2];
            const std::uint32_t vi = (std::uint32_t)bvhVerts.size();
            bvhVerts.push_back(math::v3{m.positions[ia * 3], m.positions[ia * 3 + 1], m.positions[ia * 3 + 2]});
            bvhVerts.push_back(math::v3{m.positions[ib * 3], m.positions[ib * 3 + 1], m.positions[ib * 3 + 2]});
            bvhVerts.push_back(math::v3{m.positions[ic * 3], m.positions[ic * 3 + 1], m.positions[ic * 3 + 2]});
            bvhIndices.push_back(vi);
            bvhIndices.push_back(vi + 1);
            bvhIndices.push_back(vi + 2);
            triRefs.push_back({mi, ia, ib, ic, meshToBakeable[mi]});
        }
    }
    utl::BVH bvh;
    bvh.Build(bvhVerts, bvhIndices);
    progress.Report(2, 1, 1);  // build 完成并入 trace 阶段上报

    // ---- 4/5：路径追踪积分（v2，2026-10-20 用户定调）----
    // 每纹素 spp 条完整路径：顶点 NEE（太阳/点/聚光含阴影射线）+ 自发光
    // 命中 + 天空 miss + 余弦半球 BSDF 采样，depth≥2 俄罗斯轮盘。产物 =
    // 表面完整出射辐亮度（直射+多弹间接），运行时独立消费、不依赖动态 GI。
    const std::uint32_t texelCount = atlasW * atlasH;
    const std::uint32_t maxDepth = std::max(1u, settings.bounces);  // 散射事件数
    const std::uint32_t spp = std::max(1u, settings.samples_per_texel);
    std::vector<Vec3> accum(texelCount);

    // 命中面属性（重心位置/法线插值 + 双面翻转）——路径任意顶点复用。
    auto hitSurface = [](const BakeMesh* m, std::uint32_t v0, std::uint32_t v1,
                         std::uint32_t v2, float u, float v, const Vec3& rayDir,
                         Vec3& hp, Vec3& hn) {
        const Vec3 pa{m->positions[v0 * 3], m->positions[v0 * 3 + 1], m->positions[v0 * 3 + 2]};
        const Vec3 pb{m->positions[v1 * 3], m->positions[v1 * 3 + 1], m->positions[v1 * 3 + 2]};
        const Vec3 pc{m->positions[v2 * 3], m->positions[v2 * 3 + 1], m->positions[v2 * 3 + 2]};
        const float w0 = 1.0f - u - v;
        hp = Add(Add(Scale(pa, w0), Scale(pb, u)), Scale(pc, v));
        if (m->normals) {
            const Vec3 n0{m->normals[v0 * 3], m->normals[v0 * 3 + 1], m->normals[v0 * 3 + 2]};
            const Vec3 n1{m->normals[v1 * 3], m->normals[v1 * 3 + 1], m->normals[v1 * 3 + 2]};
            const Vec3 n2{m->normals[v2 * 3], m->normals[v2 * 3 + 1], m->normals[v2 * 3 + 2]};
            hn = Norm(Add(Add(Scale(n0, w0), Scale(n1, u)), Scale(n2, v)));
        } else {
            hn = Norm(Cross(Sub(pb, pa), Sub(pc, pa)));
        }
        if (Dot(hn, rayDir) > 0.0f) hn = Scale(hn, -1.0f);  // 薄板/双面几何
    };

    {
        const std::uint32_t blocks = 64;
        const std::uint32_t per = (texelCount + blocks - 1) / blocks;
        ParallelBlocks(blocks, [&](std::uint32_t b) {
            const std::uint32_t s0 = b * per;
            const std::uint32_t e = std::min(texelCount, s0 + per);
            const float invPi = 1.0f / 3.14159265f;
            for (std::uint32_t i = s0; i < e; ++i) {
                const TexelSample& ts = texels[i];
                if (!ts.valid) continue;
                const BakeMesh& self = *allMeshes[ts.mesh_index];
                Vec3 sum{0, 0, 0};
                for (std::uint32_t s = 0; s < spp; ++s) {
                    Rng rng(HashTexel(i, s + 1));
                    Vec3 beta{1, 1, 1};
                    // 顶点 0 = 纹素自身：自发光即表面出射的一部分。
                    Vec3 col{self.emissive[0], self.emissive[1], self.emissive[2]};
                    Vec3 o = ts.pos, n = ts.normal;
                    Vec3 alb{ts.albedo[0], ts.albedo[1], ts.albedo[2]};
                    for (std::uint32_t depth = 0; depth <= maxDepth; ++depth) {
                        // 顶点 NEE：f·E = albedo/π × 直射辐照（含阴影射线）。
                        const Vec3 E = DirectIrradiance(scene, bvh, Add(o, Scale(n, 1e-3f)), n);
                        col = Add(col, Mul(beta, Scale(Mul(alb, E), invPi)));
                        if (depth == maxDepth) break;  // 末顶点只收 NEE 不再延伸
                        // 俄罗斯轮盘（depth≥2）：吞吐量低的路径提前终止。
                        if (depth >= 2) {
                            const float q = std::max(alb.x, std::max(alb.y, alb.z));
                            const float pCont = std::min(0.95f, std::max(0.05f, q));
                            if (rng.NextF() >= pCont) break;
                            beta = Scale(beta, 1.0f / pCont);
                        }
                        // 余弦半球 BSDF 采样；漫反射权重 f·cos/pdf = albedo。
                        beta = Mul(beta, alb);
                        Vec3 t, bi2;
                        Onb(n, t, bi2);
                        const Vec3 local = CosineHemisphere(rng);
                        const Vec3 d = Norm(Add(Add(Scale(t, local.x), Scale(bi2, local.y)),
                                                Scale(n, local.z)));
                        utl::BVH::Ray ray(ToV3(Add(o, Scale(n, 1e-3f))), ToV3(d));
                        ray.t_min = 1e-3f;
                        utl::BVH::HitInfo hit;
                        if (!bvh.Intersect(ray, hit)) {
                            col = Add(col, Mul(beta, SkyRadiance(scene.sky, d)));
                            break;
                        }
                        if (hit.triangle_index >= triRefs.size()) break;
                        const TriRef& ref = triRefs[hit.triangle_index];
                        const BakeMesh* hm = allMeshes[ref.mesh];
                        Vec3 hp, hn;
                        hitSurface(hm, ref.v0, ref.v1, ref.v2, hit.u, hit.v, d, hp, hn);
                        col = Add(col, Mul(beta, Vec3{hm->emissive[0], hm->emissive[1],
                                                      hm->emissive[2]}));
                        o = hp; n = hn;
                        alb = Vec3{hm->albedo[0], hm->albedo[1], hm->albedo[2]};
                    }
                    sum = Add(sum, col);
                }
                accum[i] = Scale(sum, 1.0f / (float)spp);
                // [DIAG] PRIMAL_LIGHTMAP_TRACE_TEXEL=<idx>：单纹素分解。
                if (const char* te = std::getenv("PRIMAL_LIGHTMAP_TRACE_TEXEL")) {
                    const std::uint32_t ti = (std::uint32_t)std::atoi(te);
                    if (i == ti && ts.valid) {
                        Vec3 e0 = DirectIrradiance(scene, bvh, Add(ts.pos, Scale(ts.normal, 1e-3f)), ts.normal);
                        std::uint32_t miss = 0, hits = 0, neeHit = 0;
                        const BakeMesh& dm = *allMeshes[ts.mesh_index];
                        for (int k = 0; k < 64; ++k) {
                            Rng rng(HashTexel(i, k + 1));
                            Vec3 t2, b2; Onb(ts.normal, t2, b2);
                            const Vec3 lc = CosineHemisphere(rng);
                            const Vec3 d2 = Norm(Add(Add(Scale(t2, lc.x), Scale(b2, lc.y)), Scale(ts.normal, lc.z)));
                            utl::BVH::Ray r2(ToV3(Add(ts.pos, Scale(ts.normal, 1e-3f))), ToV3(d2));
                            r2.t_min = 1e-3f;
                            utl::BVH::HitInfo h2;
                            if (!bvh.Intersect(r2, h2)) { miss++; continue; }
                            hits++;
                            if (h2.triangle_index < triRefs.size()) {
                                const TriRef& rf = triRefs[h2.triangle_index];
                                const BakeMesh* m2 = allMeshes[rf.mesh];
                                Vec3 hp2, hn2;
                                hitSurface(m2, rf.v0, rf.v1, rf.v2, h2.u, h2.v, d2, hp2, hn2);
                                if (DirectIrradiance(scene, bvh, Add(hp2, Scale(hn2, 1e-3f)), hn2).x > 0.001f) neeHit++;
                            }
                        }
                        PRIMAL_LOG_STREAM(Error, "Render")
                            << "[Lightmap][DIAG] texel=" << i << " pos=(" << ts.pos.x << ","
                            << ts.pos.y << "," << ts.pos.z << ") alb=(" << ts.albedo[0]
                            << "," << ts.albedo[1] << "," << ts.albedo[2] << ") meshAlb=("
                            << dm.albedo[0] << "," << dm.albedo[1] << "," << dm.albedo[2]
                            << ") E0=(" << e0.x << "," << e0.y << "," << e0.z
                            << ") accum=" << accum[i].x
                            << " sky(intensity=" << scene.sky.intensity
                            << " zen=" << scene.sky.zenith[0] << "," << scene.sky.zenith[1]
                            << "," << scene.sky.zenith[2] << ")"
                            << " 64ray: miss=" << miss
                            << " hit=" << hits << " hitNEE>0=" << neeHit << std::endl;
                    }
                }
            }
            progress.Report(2, b + 1, blocks);
        });
    }

    // ---- 6 filter：5×5 联合双边（可选） ----
    if (settings.denoise) {
        std::vector<Vec3> filtered(texelCount);
        const std::uint32_t blocks = 64;
        const std::uint32_t per = (texelCount + blocks - 1) / blocks;
        ParallelBlocks(blocks, [&](std::uint32_t b) {
            const std::uint32_t s = b * per;
            const std::uint32_t e = std::min(texelCount, s + per);
            for (std::uint32_t i = s; i < e; ++i) {
                if (!texels[i].valid) { filtered[i] = accum[i]; continue; }
                const std::int32_t x = (std::int32_t)(i % atlasW), y = (std::int32_t)(i / atlasW);
                Vec3 sum{0, 0, 0};
                float wsum = 0;
                for (std::int32_t dy = -2; dy <= 2; ++dy) {
                    for (std::int32_t dx = -2; dx <= 2; ++dx) {
                        const std::int32_t nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= (std::int32_t)atlasW || ny >= (std::int32_t)atlasH)
                            continue;
                        const std::uint32_t ni = (std::uint32_t)ny * atlasW + (std::uint32_t)nx;
                        if (!texels[ni].valid) continue;
                        const float spatial = std::exp(-(dx * dx + dy * dy) / 4.0f);
                        const float dc = std::abs(accum[ni].x - accum[i].x) +
                                         std::abs(accum[ni].y - accum[i].y) +
                                         std::abs(accum[ni].z - accum[i].z);
                        const float range = std::exp(-(dc * dc) / (2.0f * 0.2f * 0.2f));
                        const float w = spatial * range;
                        sum = Add(sum, Scale(accum[ni], w));
                        wsum += w;
                    }
                }
                filtered[i] = wsum > 1e-6f ? Scale(sum, 1.0f / wsum) : accum[i];
            }
            progress.Report(3, b + 1, blocks);
        });
        accum = std::move(filtered);
    } else {
        progress.Report(3, 1, 1);
    }

    // ---- 7 dilate：矩形内扩张 ----
    for (std::uint32_t it = 0; it < settings.dilation_px; ++it) {
        std::vector<Vec3> next = accum;
        std::vector<bool> validNow(texelCount);
        for (std::uint32_t i = 0; i < texelCount; ++i) validNow[i] = texels[i].valid;
        for (std::uint32_t y = 0; y < atlasH; ++y) {
            for (std::uint32_t x = 0; x < atlasW; ++x) {
                const std::uint32_t i = y * atlasW + x;
                if (validNow[i]) continue;
                Vec3 sum{0, 0, 0};
                std::uint32_t n = 0;
                for (std::int32_t dy = -1; dy <= 1; ++dy) {
                    for (std::int32_t dx = -1; dx <= 1; ++dx) {
                        const std::int32_t nx = (std::int32_t)x + dx, ny = (std::int32_t)y + dy;
                        if (nx < 0 || ny < 0 || nx >= (std::int32_t)atlasW || ny >= (std::int32_t)atlasH)
                            continue;
                        const std::uint32_t ni = (std::uint32_t)ny * atlasW + (std::uint32_t)nx;
                        if (!validNow[ni]) continue;
                        sum = Add(sum, accum[ni]);
                        ++n;
                    }
                }
                if (n > 0) {
                    next[i] = Scale(sum, 1.0f / (float)n);
                    texels[i].valid = true;  // 下一迭代继续向外扩
                }
            }
        }
        accum = std::move(next);
    }
    progress.Report(4, settings.dilation_px, settings.dilation_px);

    // ---- 8 encode + save ----
    LightmapContainer c;
    c.atlas_count = 1;
    LightmapPageData page;
    page.width = atlasW;
    page.height = atlasH;
    page.texels_rgba16f.resize((size_t)atlasW * atlasH * 4);
    for (std::uint32_t i = 0; i < texelCount; ++i) {
        page.texels_rgba16f[i * 4 + 0] = FloatToHalfBits(accum[i].x);
        page.texels_rgba16f[i * 4 + 1] = FloatToHalfBits(accum[i].y);
        page.texels_rgba16f[i * 4 + 2] = FloatToHalfBits(accum[i].z);
        page.texels_rgba16f[i * 4 + 3] = 0x3C00;  // A=1（未来存遮罩/权重）
    }
    c.pages.push_back(std::move(page));
    c.entries = result.entries;

    std::string dir = settings.output_dir[0] ? std::string(settings.output_dir) : std::string(".");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);  // 已存在则无操作
    const std::string path = dir + "/lightmap.lmap";
    if (!SaveLightmapContainer(path, c)) {
        result.error = "failed to write " + path;
        result.entries.clear();
        return result;
    }
    // 可视化导出（用户需求 3）：lightmap_page.png —— Reinhard+gamma2.2 的
    // LDR 视图 + per-mesh 1px 白框。失败只打日志不阻断（查看产物非数据链）。
    {
        std::vector<std::uint32_t> rectFlat;
        rectFlat.reserve(rects.size() * 4);
        for (const auto& r : rects) rectFlat.insert(rectFlat.end(), {r.x, r.y, r.w, r.h});
        // Vec3 为标准布局 {x,y,z}——reinterpret 为 float 三元组。
        static_assert(std::is_standard_layout_v<Vec3> && sizeof(Vec3) == 12,
                      "accum reinterpret to float triple requires standard-layout Vec3");
        WriteLightmapPagePng(dir + "/lightmap_page.png",
                             reinterpret_cast<const float*>(accum.data()),
                             atlasW, atlasH,
                             rectFlat.empty() ? nullptr : rectFlat.data(),
                             (std::uint32_t)rectFlat.size() / 4);
    }
    progress.Report(5, 1, 1);

    result.ok = true;
    result.output_path = path;
    result.atlas_width = atlasW;
    result.atlas_height = atlasH;
    return result;
}

} // namespace primal::graphics::baking
