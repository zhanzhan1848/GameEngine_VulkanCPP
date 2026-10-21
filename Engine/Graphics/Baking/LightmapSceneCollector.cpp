// Lightmap 场景收集器实现（2026-10-19 计划，M2）。契约见头注释。

#include "LightmapSceneCollector.h"

#include "CommonHeaders.h"
#include "Content/ContentToEngine.h"
#include "Graphics/RenderScene.h"
#include "Engine/Common/CoreLogStream.h"  // PRIMAL_LOG_STREAM

#include <cmath>
#include <cstdlib>
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

        // §6.7 色渗透取证：前 10 个 mesh 的烘焙 albedo 一览（注入链可见
        // 性——红帷幕/彩墙 vs 石墙应显著异色；全白≈贴图平均未注入）。
        if (out.scene.meshes.size() < 10) {
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[Lightmap][collect] albedo[" << out.scene.meshes.size()
                << "] " << mesh.name << " (" << mesh.albedo[0] << ", "
                << mesh.albedo[1] << ", " << mesh.albedo[2] << ")";
        }

        const bool hasUv = asset.lightmap_uv_buffer.size() >= (size_t)asset.num_vertices * 8;

        // §14 方案 B：材质 UV0 提取（element static_normal_texture 24B 布局，
        // uv = 每顶点末 8B f32 对——与 GPUDrivenDraw PipelineDraw 顶点拉取
        // 同源同偏移；烘焙采样按渲染同约定 y 翻转）。其他 elements_type 无
        // 材质 UV → material_uvs 空，baker 回退 per-mesh 平均 albedo。
        if (asset.elements_type == 0x3 /*static_normal_texture*/ &&
            asset.element_buffer.size() >= (size_t)asset.num_vertices * 24) {
            auto& muvs = out.ownedMaterialUvs.emplace_back();
            muvs.resize((size_t)asset.num_vertices * 2);
            const std::uint8_t* eb = asset.element_buffer.data();
            for (std::uint32_t v = 0; v < asset.num_vertices; ++v) {
                std::memcpy(&muvs[(size_t)v * 2 + 0], eb + (size_t)v * 24 + 16, 4);
                std::memcpy(&muvs[(size_t)v * 2 + 1], eb + (size_t)v * 24 + 20, 4);
            }
        } else {
            out.ownedMaterialUvs.emplace_back();  // 占位——与 mesh 下标对齐
        }

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
        stored.material_uvs = out.ownedMaterialUvs[idx].empty() ? nullptr
                                                                : out.ownedMaterialUvs[idx].data();
        stored.source_mesh_id = (std::uint32_t)proxy.meshId;
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

    PRIMAL_LOG_STREAM(Info, "Render")
        << "[Lightmap][collect] meshes=" << out.scene.meshes.size()
        << " lights=" << out.scene.lights.size();
    // §15 远平面暗带根因修复（2026-10-21）：烘焙天空是暗程序梯度
    //（BakeSky 默认峰值 ~0.65），而运行时天空为 HDR 环境贴图——天空主导
    // 的远场区域（atrium 深处/背阴拱）烘焙值系统性偏暗，上屏即"贴着远
    // 平面移动的黑影"。天空辐射整体校准 ×2.0（远场 ON 17.6→~28 vs OFF
    // 25.6/255，GAIN=5 不变；太阳 NEE 走 RenderLight 单位不受影响）。
    // PRIMAL_LIGHTMAP_SKYINT 可调（1.0 = 修复前行为）；HDR env 方向采样
    // 入烘焙为后续项（见 §15 遗留）。
    {
        float skyInt = 2.0f;
        if (const char* e = std::getenv("PRIMAL_LIGHTMAP_SKYINT")) {
            const float v = std::atof(e);
            if (v > 0.0f && v <= 100.0f) skyInt = v;
        }
        out.scene.sky.intensity *= skyInt;
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[Lightmap][collect] sky intensity calibrated x" << skyInt
            << " -> " << out.scene.sky.intensity << std::endl;
    }
    if (!out.scene.lights.empty()) {
        const auto& l0 = out.scene.lights[0];
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[Lightmap][collect] light0 type=" << (int)l0.type
            << " intensity=" << l0.intensity
            << " dir=(" << l0.direction[0] << "," << l0.direction[1]
            << "," << l0.direction[2] << ")";
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
