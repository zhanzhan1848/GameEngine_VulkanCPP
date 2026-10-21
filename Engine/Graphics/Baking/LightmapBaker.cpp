// Lightmap 烘焙器（2026-10-19 计划，M2）。
//
// CPU 路径烘焙管线（契约见 LightmapBaker.h）：
//   0 collect  —— 校验 mesh（必须有 lightmap UV），算世界面积
//   1 pack     —— 纹素需求 → MaxRects → per-mesh 矩形 + scale/bias
//   2 raster   —— UV 空间逐三角形栅格化：每有效纹素存世界 pos/normal/albedo
//   3 build    —— 全场景世界三角形 BVH（utl::BVH）
//   4/5 path   —— 每纹素 N 条完整路径（v3 间接辐照度语义，2026-10-21
//                 架构轮）：首跳余弦半球射线——纹素顶点自身不做直射 NEE、
//                 不乘 albedo、不计自发射（直射与 albedo 调制留实时链）；
//                 天空 miss / 命中面自发光 / ≥1 弹顶点 NEE 累入间接辐照度
//                 E_ind。合成端 final = deferred直射 + albedo·E_ind/π·gain。
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
    // 2026-10-21 精度轮：2048→4096（用户"纹理精度太低"；密度阶梯同步
    // 扩 32/48，纹素量 ×4、烘焙耗时 ~×4——见 §15 取舍记录）。
    s.atlas_size = 4096;
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

// ---------- §14 方案 B：贴图 albedo 采样 ----------

// sRGB u8 → 线性 f32 LUT（256 项，pow 2.2——与方案 A 的逐像素线性化同式）。
struct AlbedoLut {
    float v[256];
    AlbedoLut() {
        for (int i = 0; i < 256; ++i) v[i] = std::pow((float)i / 255.0f, 2.2f);
    }
};
inline const AlbedoLut& AlbedoLutTable() {
    static const AlbedoLut lut;
    return lut;
}

// 双线性采样 sRGB u8 RGB 贴图 → 线性 albedo。u/v 为资产 element 原值；
// v 按渲染同约定翻转（PipelineDraw: uv.y = 1-uv.y），行 0 = 贴图顶行。
inline void SampleAlbedoBilinear(const unsigned char* tex, std::uint32_t w,
                                 std::uint32_t h, float u, float v,
                                 float out[3]) {
    if (!tex || w == 0 || h == 0) return;
    const float fy = 1.0f - v;
    const float fx = u * (float)w - 0.5f;
    const float gy = fy * (float)h - 0.5f;
    const auto& lut = AlbedoLutTable();
    const auto fetch = [&](int x, int y, float rgb[3]) {
        x = std::clamp(x, 0, (int)w - 1);
        y = std::clamp(y, 0, (int)h - 1);
        const unsigned char* p = tex + ((size_t)y * w + (size_t)x) * 3;
        rgb[0] = lut.v[p[0]];
        rgb[1] = lut.v[p[1]];
        rgb[2] = lut.v[p[2]];
    };
    float x0 = std::floor(fx), y0 = std::floor(gy);
    float tx = fx - x0, ty = gy - y0;
    if (tx < 0) { tx = 0; }
    if (ty < 0) { ty = 0; }
    float c00[3], c10[3], c01[3], c11[3];
    fetch((int)x0, (int)y0, c00);
    fetch((int)x0 + 1, (int)y0, c10);
    fetch((int)x0, (int)y0 + 1, c01);
    fetch((int)x0 + 1, (int)y0 + 1, c11);
    for (int k = 0; k < 3; ++k) {
        const float a = c00[k] * (1 - tx) + c10[k] * tx;
        const float b = c01[k] * (1 - tx) + c11[k] * tx;
        out[k] = a * (1 - ty) + b * ty;
    }
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
    // §17：与运行时 EvaluateSkyBase 同曲线——pow(up,0.45) 混合；地平以下
    // 返回地平色（运行时 clamp(up,0,1) 语义：向下射线带回暖地平 proxy）。
    // [2026-10-21 §19 ③ 廊顶异常亮取证/修复] PRIMAL_LIGHTMAP_SKYBELOW：
    //   未设/1 = §17 行为（地平以下返回地平色——面朝下表面的向下逃逸
    //   bounce ray 全按 ~4.4 计，E_ind 含虚假下半球分量）；
    //   0     = 物理零（向下逃逸 = 无贡献，§17 前行为）。
    //   取证 A/B：同图集直读 E 分布对比；若廊顶/天花板 E 回物理量级即坐实。
    static const bool s_belowZero = [] {
        const char* e = std::getenv("PRIMAL_LIGHTMAP_SKYBELOW");
        return e && e[0] == '0' && e[1] == '\0';
    }();
    if (s_belowZero && dir.y < 0.0f) return Vec3{0, 0, 0};
    const float t = std::pow(std::min(1.0f, std::max(0.0f, dir.y)), 0.45f);
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

    // §18 UV bbox 收紧：每 bakeable mesh 的 UVLM 值域统计（仅索引引用
    // 顶点——未引用顶点可能带垃圾 UV）。实测（SponzaLM，393 mesh）：
    // xatlas 类展开把每 mesh 归一化到整块 [0,1]²（bboxArea 和 ≈ 0.995/
    // mesh），但岛屿只填充自身 bbox 的 ~66%（p25/p50/p75 = 0.57/0.65/
    // 0.75）——"分配了但没有任何三角形光栅化到"的座位由两部分构成：
    //   a) 装箱按世界面积×1.3 系数假满铺 [0,1]² → rect 内 1-fill≈34% 空区；
    //   b) 密度阶梯过粗（16 装下 32 装不下）→ 页面 71.7% 从未分配
    //      （74.7% 空区的主项，§16/§17 遗留）。
    // 修复：rect = 岛屿 bbox × 精确密度（k = density·sqrt(A/island)，长宽
    // 比 = bbox 长宽比——保 aspect 且两轴密度一致），uv' = (uv-min)/size
    // 归一后映射 rect；LMAP scale/bias 写同一仿射（运行时 UV pre-pass 自动
    // 一致）。岛屿间 bbox 内隙由洪泛填充（A=0.5 padding 语义，dilate 后）。
    struct UvStats {
        float minU = 0.0f, minV = 0.0f, maxU = 0.0f, maxV = 0.0f;
        std::uint32_t oor = 0;      // 索引顶点引用中 u/v ∉ [0,1] 的次数
        double islandArea = 0.0;    // 三角形 UV 有向面积绝对值和
        bool usable = false;        // 岛屿面积/两轴值域均非退化
    };
    std::vector<UvStats> uvStats(bakeable.size());
    const bool uvAuditOn = [] {
        const char* a = std::getenv("PRIMAL_LIGHTMAP_UV_AUDIT");
        return a && std::atoi(a) != 0;
    }();
    for (size_t i = 0; i < bakeable.size(); ++i) {
        const BakeMesh& m = *allMeshes[bakeable[i]];
        UvStats& st = uvStats[i];
        st.minU = st.minV = 1e30f;
        st.maxU = st.maxV = -1e30f;
        for (std::uint32_t t = 0; t + 2 < m.index_count; t += 3) {
            const std::uint32_t iv[3] = {m.indices[t], m.indices[t + 1],
                                         m.indices[t + 2]};
            float us[3], vs[3];
            for (int k = 0; k < 3; ++k) {
                us[k] = m.lightmap_uvs[iv[k] * 2];
                vs[k] = m.lightmap_uvs[iv[k] * 2 + 1];
                st.minU = std::min(st.minU, us[k]);
                st.maxU = std::max(st.maxU, us[k]);
                st.minV = std::min(st.minV, vs[k]);
                st.maxV = std::max(st.maxV, vs[k]);
                if (us[k] < 0.0f || us[k] > 1.0f || vs[k] < 0.0f || vs[k] > 1.0f)
                    ++st.oor;
            }
            const float eu = (us[1] - us[0]) * (vs[2] - vs[0]) -
                             (vs[1] - vs[0]) * (us[2] - us[0]);
            st.islandArea += 0.5 * std::abs((double)eu);
        }
        st.usable = st.islandArea > 1e-12 &&
                    (st.maxU - st.minU) > 1e-9f && (st.maxV - st.minV) > 1e-9f;
        if (uvAuditOn) {
            const std::uint32_t triCount = m.index_count / 3;
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[Lightmap][uv-audit] name=" << m.name
                << " tris=" << triCount
                << " uvbbox=[" << st.minU << "," << st.minV << "," << st.maxU
                << "," << st.maxV << "]"
                << " sx=" << (st.maxU - st.minU) << " sy=" << (st.maxV - st.minV)
                << " bboxArea=" << ((double)(st.maxU - st.minU) * (st.maxV - st.minV))
                << " islandArea=" << st.islandArea
                << " fill=" << (st.islandArea > 0.0
                    ? st.islandArea / std::max(1e-30,
                          (double)(st.maxU - st.minU) * (st.maxV - st.minV))
                    : 0.0)
                << " oor=" << st.oor << "/" << (triCount * 3) << std::endl;
        }
    }

    // ---- 1 pack：纹素需求 → 矩形 ----
    // §18：rect 尺寸按岛屿 bbox × 精确密度——k = density·sqrt(A/island)
    //（texel/UV 单位，两轴一致：长宽比 = bbox 长宽比，保 aspect），栅格化
    // 命中数 = island·k² = A·density²（表面密度精确达标，与 unwrap 缩放
    // 无关）。旧式（世界面积×1.3 假满铺 [0,1]²）仅作退化 UV 回退。
    const std::uint32_t atlasW = settings.atlas_size, atlasH = settings.atlas_size;
    const std::uint32_t margin = settings.dilation_px * 2;  // 两侧各留一倍扩张
    // 每 mesh 的 UV→rect 仿射参数：uv_norm = (uv-min)/size（identity 回退
    // min=0,size=1——与旧映射逐位一致）。
    struct UvXform { float minU, sx, minV, sy; };
    std::vector<UvXform> uvX(bakeable.size(), UvXform{0.0f, 1.0f, 0.0f, 1.0f});
    std::vector<PackRequest> reqs;
    reqs.reserve(bakeable.size());
    for (size_t i = 0; i < bakeable.size(); ++i) {
        double w = 0.0, h = 0.0;
        const UvStats& st = uvStats[i];
        if (st.usable) {
            const double k = (double)settings.texel_density *
                             std::sqrt(meshAreas[i] / st.islandArea);
            w = (double)(st.maxU - st.minU) * k;
            h = (double)(st.maxV - st.minV) * k;
            uvX[i] = {st.minU, st.maxU - st.minU, st.minV, st.maxV - st.minV};
        } else {
            // 退化 UV（零面积岛屿/零值域）：旧整块方形请求 + 恒等映射。
            const double texels = meshAreas[i] * (double)settings.texel_density *
                                  (double)settings.texel_density * 1.3;
            w = h = std::sqrt(texels);
        }
        const auto clampSide = [&](double s) {
            return std::max(4.0, std::min((double)atlasW, std::floor(s)));
        };
        reqs.push_back({static_cast<std::uint32_t>(i),
                        (std::uint32_t)clampSide(w), (std::uint32_t)clampSide(h)});
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
        // uv_atlas = ((uv-min)/size)·(rect/page) + rect/page
        //            = uv·scale + bias（§18 bbox 收紧；运行时 UV pre-pass
        //            读同一 scale/bias——烘焙/运行自动一致）。identity
        //            xform（退化回退）时与旧式逐位相同。
        const UvXform& xf = uvX[p.id];
        e.scale_x = (float)p.w / (xf.sx * (float)atlasW);
        e.scale_y = (float)p.h / (xf.sy * (float)atlasH);
        e.bias_x = (float)p.x / (float)atlasW - xf.minU * e.scale_x;
        e.bias_y = (float)p.y / (float)atlasH - xf.minV * e.scale_y;
        e.rect_x = p.x; e.rect_y = p.y; e.rect_w = p.w; e.rect_h = p.h;
        result.entries.push_back(e);
    }

    {
        const std::uint32_t meshCount = (std::uint32_t)bakeable.size();
        // §18 审计：每 mesh 栅格化命中数（各 mesh 独占槽位，无竞争）。
        const bool uvAuditOn = [] {
            const char* a = std::getenv("PRIMAL_LIGHTMAP_UV_AUDIT");
            return a && std::atoi(a) != 0;
        }();
        std::vector<std::uint64_t> rasterHits(uvAuditOn ? bakeable.size() : 0, 0);
        ParallelBlocks(meshCount, [&](std::uint32_t mi) {
            std::uint64_t localHits = 0;
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
                // rect 内 texel 空间坐标（uv 归一到岛屿 bbox 后 → rect 全域；
                // §18——与 LMAP scale/bias 同一仿射，烘焙/运行一致）。
                const UvXform& xf = uvX[mi];
                auto toRect = [&](float u, float v) {
                    const float un = (u - xf.minU) / xf.sx;
                    const float vn = (v - xf.minV) / xf.sy;
                    return std::pair<float, float>{(float)r.x + un * (float)r.w,
                                                   (float)r.y + vn * (float)r.h};
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
                        ++localHits;
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
                        // §14 方案 B：贴图齐备时逐 texel 重心插值 UV0 → 双线性
                        // 采样。v3 间接辐照度语义下纹素自身 albedo 不再调制
                        // 存储值（运行时 GBuffer albedo 负责）；此处保留采样
                        // 结果供 DIAG 取证/后续扩展，无贴图回退 per-mesh 平均。
                        if (m.albedo_texture && m.material_uvs) {
                            const float mu = m.material_uvs[ia * 2] * w0 +
                                             m.material_uvs[ib * 2] * w1 +
                                             m.material_uvs[ic * 2] * w2;
                            const float mv = m.material_uvs[ia * 2 + 1] * w0 +
                                             m.material_uvs[ib * 2 + 1] * w1 +
                                             m.material_uvs[ic * 2 + 1] * w2;
                            float alb[3] = {m.albedo[0], m.albedo[1], m.albedo[2]};
                            SampleAlbedoBilinear(m.albedo_texture, m.albedo_tex_w,
                                                 m.albedo_tex_h, mu, mv, alb);
                            s.albedo[0] = alb[0]; s.albedo[1] = alb[1]; s.albedo[2] = alb[2];
                        } else {
                            s.albedo[0] = m.albedo[0]; s.albedo[1] = m.albedo[1]; s.albedo[2] = m.albedo[2];
                        }
                    }
                }
            }
            if (!rasterHits.empty()) rasterHits[mi] = localHits;
            progress.Report(1, mi + 1, meshCount);
        });
        if (uvAuditOn) {
            double sumRect = 0.0, sumHits = 0.0, sumBbox = 0.0, sumIsland = 0.0;
            for (size_t i = 0; i < bakeable.size(); ++i) {
                const MeshRect& r = rects[i];
                const double rectArea = (double)r.w * r.h;
                sumRect += rectArea;
                sumHits += (double)rasterHits[i];
                const UvStats& st = uvStats[i];
                const double bboxArea =
                    (double)(st.maxU - st.minU) * (st.maxV - st.minV);
                sumBbox += bboxArea;
                sumIsland += st.islandArea;
                PRIMAL_LOG_STREAM(Info, "Render")
                    << "[Lightmap][uv-audit] name=" << allMeshes[bakeable[i]]->name
                    << " rect=" << r.w << "x" << r.h << "@" << r.x << "," << r.y
                    << " rectArea=" << rectArea
                    << " hits=" << rasterHits[i]
                    << " cov=" << (rectArea > 0.0 ? (double)rasterHits[i] / rectArea : 0.0)
                    << std::endl;
            }
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[Lightmap][uv-audit] SUMMARY density=" << settings.texel_density
                << " pack_util=" << packer.Utilization()
                << " meshes=" << bakeable.size()
                << " sum_rect=" << sumRect
                << " sum_hits=" << sumHits
                << " page=" << atlasW << "x" << atlasH
                << " page_raster_frac=" << (sumHits / ((double)atlasW * atlasH))
                << " sum_bbox=" << sumBbox << " sum_island=" << sumIsland
                << " mean_fill=" << (sumBbox > 0.0 ? sumIsland / sumBbox : 0.0)
                << std::endl;
            // 早退：审计只到 raster 为止——返回错误让密度阶梯继续/终止，
            // 不做 BVH/trace（省 ~20 分钟积分）。
            result.error = "[uv-audit] early exit after raster (trace skipped)";
            return result;
        }
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

    // ---- 4/5：路径追踪积分（v3 间接辐照度语义，2026-10-21 架构轮）----
    // 产物 = 纹素间接辐照度 E_ind：入射到纹素表面的一切非解析直射辐照——
    // ≥1 弹路径 + 天空（含首跳直视天空）。估计器推导：
    //   E_ind = ∫ L_ind(ω)·cosθ dω；余弦半球采样 pdf = cosθ/π
    //   ⇒ 无偏估计 E_ind = π · mean_s(L_ind)。
    // 单样本 L_ind 的路径贡献（纹素顶点 = depth 0，自身不折叠 albedo、
    // 不做 NEE、不计自发射——直射由实时 deferred 渲染、albedo 调制由
    // composite 端 GBuffer albedo 完成，故烘焙值与纹素自身反照率无关）：
    //   首跳 miss         → L_ind += Sky(d)              （天空直照）
    //   首跳/后继命中     → L_ind += Le(hit)             （面发光体）
    //   ≥1 弹顶点 NEE     → L_ind += alb_hit·E_direct/π  （解析光反弹）
    //   后继 BSDF 散射    → throughput ×= alb_hit（余弦采样 f·cos/pdf = alb）
    // depth≥2 俄罗斯轮盘照旧。合成端：final = deferred直射 + albedo·E_ind/π·gain。
    const std::uint32_t texelCount = atlasW * atlasH;
    const std::uint32_t maxDepth = std::max(1u, settings.bounces);  // 散射事件数
    const std::uint32_t spp = std::max(1u, settings.samples_per_texel);
    std::vector<Vec3> accum(texelCount);

    // [2026-10-21 §20 取证] PRIMAL_LIGHTMAP_SKYSPLIT=1：并行累加"天空分量"
    // 页（一切深度 miss 天空贡献 × 同 beta；NEE/emissive 不入）——一次烘焙
    // 同时产出 total/sky 两页，差分即反弹分量（同 RNG 同路径，零差分噪声）。
    // 产物 <output_dir>/lightmap_sky.lmap（A 通道语义同主页）。
    const bool skySplit = [] {
        const char* e = std::getenv("PRIMAL_LIGHTMAP_SKYSPLIT");
        return e && e[0] == '1';
    }();
    std::vector<Vec3> skyAccum(skySplit ? texelCount : 0);
    // [2026-10-21 §20 泄漏审计] PRIMAL_LIGHTMAP_MISSDUMP=x0,y0,z0,x1,y1,z1：
    // AABB 内 texel 的首跳 sky-miss 统计行（miss 率 + 首条 miss 方向）写
    // <output_dir>/missrays.txt——Python 反查"射线从哪条缝出去/是否真可见天"。
    // 每行：texel px py pos(3) normal(3) missRate firstMissDir(3)。
    struct MissAudit {
        bool on = false;
        float bmin[3], bmax[3];
    } missAudit{};
    if (const char* a = std::getenv("PRIMAL_LIGHTMAP_MISSDUMP")) {
        if (std::sscanf(a, "%f,%f,%f,%f,%f,%f", &missAudit.bmin[0], &missAudit.bmin[1],
                        &missAudit.bmin[2], &missAudit.bmax[0], &missAudit.bmax[1],
                        &missAudit.bmax[2]) == 6) {
            missAudit.on = true;
        }
    }
    std::mutex missMutex;
    std::string missRows;

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
            std::string localMiss;
            char line[512];
            for (std::uint32_t i = s0; i < e; ++i) {
                const TexelSample& ts = texels[i];
                if (!ts.valid) continue;
                Vec3 sum{0, 0, 0};
                Vec3 skySum{0, 0, 0};
                std::uint32_t miss0 = 0;             // 首跳 sky-miss 计数
                Vec3 firstMissDir{0, 0, 0};
                const bool auditTexel = missAudit.on &&
                    ts.pos.x >= missAudit.bmin[0] && ts.pos.x <= missAudit.bmax[0] &&
                    ts.pos.y >= missAudit.bmin[1] && ts.pos.y <= missAudit.bmax[1] &&
                    ts.pos.z >= missAudit.bmin[2] && ts.pos.z <= missAudit.bmax[2];
                for (std::uint32_t s = 0; s < spp; ++s) {
                    Rng rng(HashTexel(i, s + 1));
                    Vec3 beta{1, 1, 1};
                    // v3：纹素顶点无自发射计入（发射面由实时链/emissive 渲染；
                    // 烘焙只回答"到达纹素的间接辐照度"）。
                    Vec3 col{0, 0, 0};
                    Vec3 skyCol{0, 0, 0};
                    Vec3 o = ts.pos, n = ts.normal;
                    Vec3 alb{ts.albedo[0], ts.albedo[1], ts.albedo[2]};
                    for (std::uint32_t depth = 0; depth <= maxDepth; ++depth) {
                        // 顶点 NEE：仅 depth≥1——depth 0 是纹素自身，其直射
                        // 辐照由实时 deferred 链渲染（v2 的顶点 0 NEE 项移除）。
                        if (depth > 0) {
                            const Vec3 E = DirectIrradiance(scene, bvh, Add(o, Scale(n, 1e-3f)), n);
                            col = Add(col, Mul(beta, Scale(Mul(alb, E), invPi)));
                        }
                        if (depth == maxDepth) break;  // 末顶点只收 NEE 不再延伸
                        // 俄罗斯轮盘（depth≥2）：吞吐量低的路径提前终止。
                        if (depth >= 2) {
                            const float q = std::max(alb.x, std::max(alb.y, alb.z));
                            const float pCont = std::min(0.95f, std::max(0.05f, q));
                            if (rng.NextF() >= pCont) break;
                            beta = Scale(beta, 1.0f / pCont);
                        }
                        // 余弦半球 BSDF 采样；漫反射权重 f·cos/pdf = albedo。
                        // depth==0（纹素自身）不折叠 albedo：存 E 而非
                        // albedo·E/π——反照率调制延后到 composite 端由
                        // GBuffer albedo（真实材质纹理）逐像素完成。
                        if (depth > 0) beta = Mul(beta, alb);
                        Vec3 t, bi2;
                        Onb(n, t, bi2);
                        const Vec3 local = CosineHemisphere(rng);
                        const Vec3 d = Norm(Add(Add(Scale(t, local.x), Scale(bi2, local.y)),
                                                Scale(n, local.z)));
                        utl::BVH::Ray ray(ToV3(Add(o, Scale(n, 1e-3f))), ToV3(d));
                        ray.t_min = 1e-3f;
                        utl::BVH::HitInfo hit;
                        if (!bvh.Intersect(ray, hit)) {
                            const Vec3 skyC = SkyRadiance(scene.sky, d);
                            col = Add(col, Mul(beta, skyC));
                            skyCol = Add(skyCol, Mul(beta, skyC));
                            if (depth == 0) {
                                ++miss0;
                                if (miss0 == 1) firstMissDir = d;
                            }
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
                    skySum = Add(skySum, skyCol);
                }
                // E_ind = π · mean(L_ind)（估计器推导见本阶段头注释）。
                accum[i] = Scale(sum, 3.14159265f / (float)spp);
                if (skySplit) skyAccum[i] = Scale(skySum, 3.14159265f / (float)spp);
                if (auditTexel) {
                    // 首跳 miss 率 + 首条 miss 方向（AABB 内 texel 泄漏审计行）。
                    std::snprintf(line, sizeof(line),
                        "%u %u %.3f %.3f %.3f %.3f %.3f %.3f %.4f %.3f %.3f %.3f\n",
                        i % atlasW, i / atlasW, ts.pos.x, ts.pos.y, ts.pos.z,
                        ts.normal.x, ts.normal.y, ts.normal.z,
                        (float)miss0 / (float)spp, firstMissDir.x, firstMissDir.y,
                        firstMissDir.z);
                    localMiss += line;
                    if (localMiss.size() > (1u << 20)) {
                        std::lock_guard<std::mutex> lk(missMutex);
                        missRows += localMiss;
                        localMiss.clear();
                    }
                }
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
                if (!localMiss.empty()) {
                    std::lock_guard<std::mutex> lk(missMutex);
                    missRows += localMiss;
                }
            });
        }

    // ---- 6 filter：5×5 联合双边（可选） ----
    if (settings.denoise) {
        std::vector<Vec3> filtered(texelCount);
        std::vector<Vec3> skyFiltered(skySplit ? texelCount : 0);
        const std::uint32_t blocks = 64;
        const std::uint32_t per = (texelCount + blocks - 1) / blocks;
        ParallelBlocks(blocks, [&](std::uint32_t b) {
            const std::uint32_t s = b * per;
            const std::uint32_t e = std::min(texelCount, s + per);
            for (std::uint32_t i = s; i < e; ++i) {
                if (!texels[i].valid) { filtered[i] = accum[i];
                    if (skySplit) skyFiltered[i] = skyAccum[i]; continue; }
                const std::int32_t x = (std::int32_t)(i % atlasW), y = (std::int32_t)(i / atlasW);
                Vec3 sum{0, 0, 0}, skySum2{0, 0, 0};
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
                        if (skySplit) skySum2 = Add(skySum2, Scale(skyAccum[ni], w));
                        wsum += w;
                    }
                }
                filtered[i] = wsum > 1e-6f ? Scale(sum, 1.0f / wsum) : accum[i];
                if (skySplit)
                    skyFiltered[i] = wsum > 1e-6f ? Scale(skySum2, 1.0f / wsum) : skyAccum[i];
            }
            progress.Report(3, b + 1, blocks);
        });
        accum = std::move(filtered);
        if (skySplit) skyAccum = std::move(skyFiltered);
    } else {
        progress.Report(3, 1, 1);
    }

    // ---- 7 dilate：矩形内扩张 ----
    // v3 有效性通道（§15.4 z=-8 整屏黑修复）：扩张会消费 valid 位，先快照
    // 栅格化原始命中。编码时 A = 1.0（栅格化）/ 0.5（dilation 填充值）/
    // 0.0（从未命中——chart 内未栅格化空区/UVLM 越界逃逸）；composite 对
    // A<阈值不加间接（退化为纯直射底图，绝不黑）。
    std::vector<std::uint8_t> rasterized(texelCount);
    for (std::uint32_t i = 0; i < texelCount; ++i) rasterized[i] = texels[i].valid ? 1u : 0u;
    for (std::uint32_t it = 0; it < settings.dilation_px; ++it) {
        std::vector<Vec3> next = accum;
        std::vector<Vec3> skyNext = skySplit ? skyAccum : std::vector<Vec3>{};
        std::vector<bool> validNow(texelCount);
        for (std::uint32_t i = 0; i < texelCount; ++i) validNow[i] = texels[i].valid;
        for (std::uint32_t y = 0; y < atlasH; ++y) {
            for (std::uint32_t x = 0; x < atlasW; ++x) {
                const std::uint32_t i = y * atlasW + x;
                if (validNow[i]) continue;
                Vec3 sum{0, 0, 0}, skySum2{0, 0, 0};
                std::uint32_t n = 0;
                for (std::int32_t dy = -1; dy <= 1; ++dy) {
                    for (std::int32_t dx = -1; dx <= 1; ++dx) {
                        const std::int32_t nx = (std::int32_t)x + dx, ny = (std::int32_t)y + dy;
                        if (nx < 0 || ny < 0 || nx >= (std::int32_t)atlasW || ny >= (std::int32_t)atlasH)
                            continue;
                        const std::uint32_t ni = (std::uint32_t)ny * atlasW + (std::uint32_t)nx;
                        if (!validNow[ni]) continue;
                        sum = Add(sum, accum[ni]);
                        if (skySplit) skySum2 = Add(skySum2, skyAccum[ni]);
                        ++n;
                    }
                }
                if (n > 0) {
                    next[i] = Scale(sum, 1.0f / (float)n);
                    if (skySplit) skyNext[i] = Scale(skySum2, 1.0f / (float)n);
                    texels[i].valid = true;  // 下一迭代继续向外扩
                }
            }
        }
        accum = std::move(next);
        if (skySplit) skyAccum = std::move(skyNext);
    }
    progress.Report(4, settings.dilation_px, settings.dilation_px);

    // §18 洪泛填充：rect 内仍未覆盖的纹素（岛屿间 bbox 间隙——"分配了
    // 但没有任何三角形光栅化到"座位的 rect 内部分）以 dilation 同语义
    //（A=0.5 padding）多源 BFS 填满：每纹素取已有效 8 邻域均值，源/目标
    // 均限 rect 内（不跨 mesh 泄漏）。marker 采样落间隙时得到近旁表面
    // 扩散的间接估计而非纯直射透传（§15.4/§16 z=-8 位姿暗的主修项）。
    {
        std::vector<std::uint8_t> inRect((size_t)texelCount, 0);
        for (const auto& r : rects) {
            const std::uint32_t y1 = std::min(atlasH, r.y + r.h);
            const std::uint32_t x1 = std::min(atlasW, r.x + r.w);
            for (std::uint32_t y = r.y; y < y1; ++y)
                for (std::uint32_t x = r.x; x < x1; ++x)
                    inRect[(size_t)y * atlasW + x] = 1;
        }
        std::vector<std::uint8_t> queued((size_t)texelCount, 0);
        std::vector<std::uint32_t> queue;
        queue.reserve(texelCount / 4);
        // 种子：rect 内无效、且 8 邻域存在 rect 内有效纹素（扫描序确定）。
        for (std::uint32_t i = 0; i < texelCount; ++i) {
            if (!inRect[i] || texels[i].valid) continue;
            const std::int32_t x = (std::int32_t)(i % atlasW), y = (std::int32_t)(i / atlasW);
            bool seed = false;
            for (std::int32_t dy = -1; dy <= 1 && !seed; ++dy)
                for (std::int32_t dx = -1; dx <= 1 && !seed; ++dx) {
                    if (!dx && !dy) continue;
                    const std::int32_t nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= (std::int32_t)atlasW ||
                        ny >= (std::int32_t)atlasH)
                        continue;
                    const std::uint32_t ni = (std::uint32_t)ny * atlasW + (std::uint32_t)nx;
                    if (inRect[ni] && texels[ni].valid) seed = true;
                }
            if (seed) { queued[i] = 1; queue.push_back(i); }
        }
        std::uint64_t filled = 0;
        for (size_t head = 0; head < queue.size(); ++head) {
            const std::uint32_t i = queue[head];
            const std::int32_t x = (std::int32_t)(i % atlasW), y = (std::int32_t)(i / atlasW);
            Vec3 sum{0, 0, 0}, skySum2{0, 0, 0};
            std::uint32_t n = 0;
            for (std::int32_t dy = -1; dy <= 1; ++dy)
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    const std::int32_t nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= (std::int32_t)atlasW ||
                        ny >= (std::int32_t)atlasH)
                        continue;
                    const std::uint32_t ni = (std::uint32_t)ny * atlasW + (std::uint32_t)nx;
                    if (!inRect[ni]) continue;
                    // 先入队再跳过无效源：入队条件含 !valid，若放在有效性
                    // continue 之后是死代码（首轮实测洪泛只填了种子环 77K）。
                    if (!queued[ni] && !texels[ni].valid) {
                        queued[ni] = 1;
                        queue.push_back(ni);
                    }
                    if (!texels[ni].valid) continue;
                    sum = Add(sum, accum[ni]);
                    if (skySplit) skySum2 = Add(skySum2, skyAccum[ni]);
                    ++n;
                }
            if (n > 0) {
                accum[i] = Scale(sum, 1.0f / (float)n);
                if (skySplit) skyAccum[i] = Scale(skySum2, 1.0f / (float)n);
                texels[i].valid = true;
                ++filled;
            }
        }
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[Lightmap][bake] flood fill: " << filled << " texels (A=0.5)"
            << std::endl;
    }

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
        // A = 有效性：1.0 栅格化 / 0.5 dilation 填充 / 0.0 空（见 dilate 注）。
        page.texels_rgba16f[i * 4 + 3] =
            rasterized[i] ? 0x3C00 : (texels[i].valid ? 0x3800 : 0x0000);
    }
    c.pages.push_back(std::move(page));
    c.entries = result.entries;

    // [§20 取证] 天空分量页（SKYSPLIT）：与主页同 filter/dilate/flood 管线、
    // 同 A 有效性——差分 total−sky = NEE/emissive 反弹分量（同 RNG 零噪声）。
    {
        std::string skyDir = settings.output_dir[0]
                                 ? std::string(settings.output_dir) : std::string(".");
        if (skySplit) {
            LightmapContainer sc;
            sc.atlas_count = 1;
            LightmapPageData spage;
            spage.width = atlasW;
            spage.height = atlasH;
            spage.texels_rgba16f.resize((size_t)atlasW * atlasH * 4);
            for (std::uint32_t i = 0; i < texelCount; ++i) {
                spage.texels_rgba16f[i * 4 + 0] = FloatToHalfBits(skyAccum[i].x);
                spage.texels_rgba16f[i * 4 + 1] = FloatToHalfBits(skyAccum[i].y);
                spage.texels_rgba16f[i * 4 + 2] = FloatToHalfBits(skyAccum[i].z);
                // 注意：主页 page 已被 move 走（上面 push_back(std::move(page))）
                // ——A 位按同一式重算（rasterized 快照 + flood 后 valid），禁止
                // 引用已 move 对象（首版即此 SIGSEGV：空 vector 下标访问）。
                spage.texels_rgba16f[i * 4 + 3] =
                    rasterized[i] ? 0x3C00 : (texels[i].valid ? 0x3800 : 0x0000);
            }
            sc.pages.push_back(std::move(spage));
            sc.entries = result.entries;
            const std::string skyPath = skyDir + "/lightmap_sky.lmap";
            if (SaveLightmapContainer(skyPath, sc)) {
                PRIMAL_LOG_STREAM(Info, "Render")
                    << "[Lightmap][bake] sky split page: " << skyPath << std::endl;
            }
        }
        // [§20 泄漏审计] miss 射线统计行落盘（首跳 miss 率 + 首条方向）。
        if (missAudit.on) {
            const std::string mpath = skyDir + "/missrays.txt";
            if (FILE* mf = std::fopen(mpath.c_str(), "wb")) {
                std::fwrite(missRows.data(), 1, missRows.size(), mf);
                std::fclose(mf);
                PRIMAL_LOG_STREAM(Info, "Render")
                    << "[Lightmap][bake] missrays: ~" << missRows.size() / 80
                    << " rows -> " << mpath << std::endl;
            }
        }
    }

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
