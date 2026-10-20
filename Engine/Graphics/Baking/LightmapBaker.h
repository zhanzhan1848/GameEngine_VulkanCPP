#pragma once

// Lightmap 烘焙契约（2026-10-19 lightmap 计划，M1 冻结）。
//
// 三个冻结面：
//   1. LightmapBakeSettings —— EngineDLL C ABI 直接镜像传递（LightmapAPI.cpp
//      static_assert 锁尺寸；只增不改）。
//   2. BakeScene —— 引擎内收集器（LightmapSceneCollector，M2）产出的
//      CPU 场景表示：世界空间三角形 soup + 灯源 + 天空。
//   3. LightmapBakeResult —— per-mesh 矩形/scale-bias + 产物路径（喂
//      编辑器文档模型与运行时注册面）。
//
// 积分器实现（M2，LightmapBaker.cpp）：CPU BVH 路径追踪 + radiosity 迭代，
// Indirect-Only（产物只存 ≥1 阶反弹间接光；直射保持实时 Shadow Map）。

#include "LightmapContainer.h"

#include <cstdint>

namespace primal::graphics::baking {

// ---- ABI 镜像（EngineDLL 直接按字节传入；改动=破坏 ABI，禁止）----
struct LightmapBakeSettings {
    std::uint32_t atlas_size;        // 方形页边长（texel），默认 2048
    std::uint32_t samples_per_texel; // 路径追踪每纹素样本数（spp），默认 64
    std::uint32_t bounces;           // 间接反弹次数，默认 3
    float texel_density;             // 纹素/米，默认 16
    std::int32_t denoise;            // 1=内置双边滤波
    std::uint32_t dilation_px;       // 接缝扩张像素，默认 8
    char output_dir[256];            // UTF-8 零填充，产物 lightmap.lmap 落于此
};
static_assert(sizeof(LightmapBakeSettings) == 280, "Lightmap bake settings ABI frozen");

LightmapBakeSettings DefaultLightmapBakeSettings();

// 进度回调（phase 语义固定；同 phase 内 done 单调递增至 total）。
//   0=collect  1=rasterize  2=trace  3=filter  4=dilate  5=encode
using LightmapProgressCallback = void (*)(void* user, std::uint32_t phase,
                                          std::uint32_t done, std::uint32_t total);

// ---- 引擎内场景表示（收集器产出；指针均为非所有权借用）----
struct BakeMesh {
    const float* positions = nullptr;      // xyz × vertex_count（世界空间）
    const float* normals = nullptr;        // xyz × vertex_count
    const float* lightmap_uvs = nullptr;   // xy × vertex_count（UVLM 展开结果）
    const std::uint32_t* indices = nullptr;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;         // 3 的倍数
    float albedo[3] = {0.8f, 0.8f, 0.8f};  // v1 材质底色（方案 A per-mesh 平均）
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    std::string name;                      // 场景内 mesh 键（LMAP manifest 同名）
    // §14 方案 B：per-texel 贴图采样。albedo_texture = sRGB u8 RGB 三元组
    //（w×h，行 0 = 贴图顶行）；material_uvs = 材质 UV0（xy × vertex_count，
    // 资产 element 原值——采样按渲染同约定 y 翻转，与 PipelineDraw 对齐）。
    // 二者齐备时栅格化逐 texel 重心插值 UV0 → 双线性采样 → LUT 线性化，
    // 贴图花纹参与反弹；缺失任一则回退 albedo（方案 A）。
    const unsigned char* albedo_texture = nullptr;
    std::uint32_t albedo_tex_w = 0;
    std::uint32_t albedo_tex_h = 0;
    const float* material_uvs = nullptr;
    // 收集器回填的代理 mesh id（BakeSceneLightmaps 据此挂接烘焙期注册的
    // per-mesh 贴图 albedo；0 = 未设）。
    std::uint32_t source_mesh_id = 0;
};

struct BakeLight {
    enum class Type : std::uint32_t { Directional = 0, Point = 1, Spot = 2 };
    Type type = Type::Directional;
    float color[3] = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;      // 方向光辐照度 / 点光功率（引擎单位约定）
    float position[3] = {0, 0, 0};
    float direction[3] = {0, -1, 0};
    float range = 10.0f;         // Point/Spot
    float cone_inner = 30.0f;    // Spot（度）
    float cone_outer = 45.0f;    // Spot（度）
};

struct BakeSky {
    float zenith[3] = {0.35f, 0.45f, 0.65f};
    float horizon[3] = {0.55f, 0.60f, 0.70f};
    float intensity = 1.0f;
    // v1 模型：上方向天空梯度（n.y 混合 zenith/horizon）；地面以下无天空。
};

struct BakeScene {
    std::vector<BakeMesh> meshes;
    std::vector<BakeLight> lights;
    BakeSky sky;
};

struct LightmapBakeResult {
    bool ok = false;
    std::string error;            // !ok 时的失败摘要
    std::string output_path;      // 成功时 = <output_dir>/lightmap.lmap
    std::uint32_t atlas_width = 0;
    std::uint32_t atlas_height = 0;
    std::vector<LightmapMeshEntry> entries;  // per-mesh 矩形 + scale/bias
};

// 主入口（M2 实现）：栅格化 → BVH 积分 → 滤波 → 扩张 → LMAP 落盘。
// 回调可为空；回调在 ParallelFor 分块边界泵出（天然节流）。
LightmapBakeResult BakeLightmaps(const BakeScene& scene,
                                 const LightmapBakeSettings& settings,
                                 LightmapProgressCallback cb = nullptr,
                                 void* cb_user = nullptr);

} // namespace primal::graphics::baking
