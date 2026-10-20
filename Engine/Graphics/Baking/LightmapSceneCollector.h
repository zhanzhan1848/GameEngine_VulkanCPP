#pragma once

// Lightmap 场景收集器（2026-10-19 计划，M2）。
//
// 从渲染场景（RenderScene）收集 BakeScene：proxy 世界变换 × 资产 soup
//（content::get_rhi_mesh_asset 按 meshId 解析）+ 灯光 + 材质底色。
// 输出持有自己的数据（变换后的 world-space 拷贝）——烘焙期间场景可继续
// 渲染，收集完成后与渲染侧零共享。
//
// 语义：
//   - 无 lightmap UV 的网格 → trace-only（BVH 遮挡，不占 atlas）；
//   - 同 meshId 多实例（instancing）按 proxy 各自收集（各自矩形）；
//   - 法线 v1 缺省面法线（baker 回退）；材质 albedo 用 proxy.base_color。
//     非均匀缩放的法线处理与非底色 albedo 记为后续增强。

#include "LightmapBaker.h"

#include <string>
#include <vector>

namespace primal::graphics {

class RenderScene;

namespace baking {

struct CollectedBakeScene {
    BakeScene scene;
    // BakeMesh 契约是裸指针借用——收集器持有底层存储。
    std::vector<std::vector<float>> ownedPositions;  // xyz
    std::vector<std::vector<float>> ownedUvs;        // uv
    std::vector<std::vector<std::uint32_t>> ownedIndices;
    // §14 方案 B：材质 UV0（element static_normal_texture 布局提取）。
    std::vector<std::vector<float>> ownedMaterialUvs;
};

// 收集全部 proxy/灯源。至少一个 bakeable mesh 才算成功（与 baker 契约一致）。
// err 返回首个失败摘要（资产解析失败/零 bakeable）。
bool CollectBakeScene(const RenderScene& renderScene, CollectedBakeScene& out,
                      std::string& err);

} // namespace baking
} // namespace primal::graphics
