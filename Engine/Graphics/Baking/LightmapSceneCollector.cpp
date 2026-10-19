// Lightmap 场景收集器实现（2026-10-19 计划，M2）。契约见头注释。

#include "LightmapSceneCollector.h"

#include "CommonHeaders.h"
#include "Content/ContentToEngine.h"
#include "Graphics/RenderScene.h"

#include <cmath>
#include <cstring>

namespace primal::graphics::baking {

bool CollectBakeScene(const RenderScene& renderScene, CollectedBakeScene& out,
                      std::string& err) {
    out = CollectedBakeScene{};
    std::uint32_t failedAssets = 0;

    for (const auto& proxy : renderScene.GetProxies()) {
        // proxy.meshId 是 hierarchy ID——两步解析（RenderSceneSnapshot.cpp:110
        // 同注）。
        rhi::RHIMeshAsset asset;
        if (!content::get_rhi_mesh_asset(content::get_rhi_mesh_id(proxy.meshId), asset)) {
            ++failedAssets;
            continue;
        }
        if (asset.num_vertices == 0 || asset.num_indices < 3 || asset.index_size == 0)
            continue;

        // 索引 → u32 视图。
        std::vector<std::uint32_t> indices(asset.num_indices);
        if (asset.index_size == 4) {
            std::memcpy(indices.data(), asset.index_buffer.data(),
                        (size_t)asset.num_indices * 4);
        } else {
            const auto* src = reinterpret_cast<const std::uint16_t*>(asset.index_buffer.data());
            for (std::uint32_t i = 0; i < asset.num_indices; ++i) indices[i] = src[i];
        }

        // 世界变换拷贝（位置）。约定与渲染一致：M × 列向量
        //（simd::float4x4，columns[col] 元素 .x/.y/.z——见
        // RenderSceneSnapshot.cpp:477/499 先例）。
        const auto& m = proxy.transform;
        auto transformPoint = [&m](float x, float y, float z) {
            return std::tuple<float, float, float>{
                m.columns[0].x * x + m.columns[1].x * y + m.columns[2].x * z + m.columns[3].x,
                m.columns[0].y * x + m.columns[1].y * y + m.columns[2].y * z + m.columns[3].y,
                m.columns[0].z * x + m.columns[1].z * y + m.columns[2].z * z + m.columns[3].z};
        };

        const auto* positions = reinterpret_cast<const float*>(asset.position_buffer.data());
        std::vector<float> worldPos((size_t)asset.num_vertices * 3);
        for (std::uint32_t v = 0; v < asset.num_vertices; ++v) {
            const auto [wx, wy, wz] = transformPoint(positions[v * 3], positions[v * 3 + 1],
                                                     positions[v * 3 + 2]);
            worldPos[v * 3] = wx; worldPos[v * 3 + 1] = wy; worldPos[v * 3 + 2] = wz;
        }

        BakeMesh mesh;
        mesh.positions = nullptr;  // 底层存储 push 后统一回填（vector move 安全）
        mesh.indices = nullptr;
        mesh.vertex_count = asset.num_vertices;
        mesh.index_count = asset.num_indices;
        mesh.albedo[0] = proxy.base_color[0];
        mesh.albedo[1] = proxy.base_color[1];
        mesh.albedo[2] = proxy.base_color[2];
        mesh.name = "mesh_" + std::to_string(proxy.meshId) + "_e" +
                    std::to_string(proxy.entityId);

        const bool hasUv = asset.lightmap_uv_buffer.size() >= (size_t)asset.num_vertices * 8;

        out.ownedPositions.push_back(std::move(worldPos));
        out.ownedIndices.push_back(std::move(indices));
        if (hasUv) {
            const auto* uvs = reinterpret_cast<const float*>(asset.lightmap_uv_buffer.data());
            out.ownedUvs.emplace_back(uvs, uvs + (size_t)asset.num_vertices * 2);
        } else {
            out.ownedUvs.emplace_back();  // trace-only：空 UV
        }

        out.scene.meshes.push_back(std::move(mesh));
        const size_t idx = out.scene.meshes.size() - 1;
        BakeMesh& stored = out.scene.meshes[idx];
        stored.positions = out.ownedPositions[idx].data();
        stored.indices = out.ownedIndices[idx].data();
        stored.lightmap_uvs = out.ownedUvs[idx].empty() ? nullptr
                                                        : out.ownedUvs[idx].data();
    }

    // 灯源映射。RenderLight 的锥角存 cos 值 → BakeLight 契约为角度。
    for (const auto& light : renderScene.GetLights()) {
        BakeLight bl;
        switch (light.type) {
            case LightType::Directional: bl.type = BakeLight::Type::Directional; break;
            case LightType::Point: bl.type = BakeLight::Type::Point; break;
            case LightType::Spot: bl.type = BakeLight::Type::Spot; break;
            default: continue;  // Area 预留未实现
        }
        bl.color[0] = light.color.x; bl.color[1] = light.color.y; bl.color[2] = light.color.z;
        bl.intensity = light.intensity;
        bl.position[0] = light.position.x; bl.position[1] = light.position.y;
        bl.position[2] = light.position.z;
        bl.direction[0] = light.direction.x; bl.direction[1] = light.direction.y;
        bl.direction[2] = light.direction.z;
        bl.range = light.range;
        bl.cone_inner = std::acos(std::min(1.0f, std::max(-1.0f, light.innerCone))) *
                        57.29578f;
        bl.cone_outer = std::acos(std::min(1.0f, std::max(-1.0f, light.outerCone))) *
                        57.29578f;
        out.scene.lights.push_back(bl);
    }

    if (out.scene.meshes.empty()) {
        err = failedAssets ? "no proxies resolved (asset lookups failed)"
                           : "scene has no render proxies";
        return false;
    }
    if (failedAssets)
        err = "";  // 部分失败不计错（收集到的仍可烘）
    return true;
}

} // namespace primal::graphics::baking
