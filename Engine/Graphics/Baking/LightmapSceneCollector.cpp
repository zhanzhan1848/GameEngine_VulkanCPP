// Lightmap 场景收集器实现（2026-10-19 计划，M2）。契约见头注释。

#include "LightmapSceneCollector.h"

#include "CommonHeaders.h"
#include "Content/ContentToEngine.h"
#include "Graphics/EntityIdentityRegistry.h"
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
        // 资产解析走 content geometry id（proxy.contentMeshId，由
        // SyncEntitiesToRenderScene 从 SceneDataMeshInfo::meshEntityId 回填）。
        // editor 管线路径的 proxy.meshId 是 mesh 资源绑定的 ECS 实体 id，进
        // content 两步解析必败（2026-11-05 排查：RenderMesh::entityId_ ≠
        // geometry_hierarchies id）；contentMeshId 缺席（离线路径未回填）时
        // 回退旧两步解析（离线路径 meshId 即 content id，语义不变）。
        const id::id_type geometryId =
            proxy.contentMeshId != id::invalid_id ? proxy.contentMeshId : proxy.meshId;
        rhi::RHIMeshAsset asset;
        if (!content::get_rhi_mesh_asset(content::get_rhi_mesh_id(geometryId), asset)) {
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
        //（math::elem(m, col, row) 逐列取分量——见
        // RenderSceneSnapshot.cpp 提取路径先例）。
        const auto& m = proxy.transform;
        auto transformPoint = [&m](float x, float y, float z) {
            return std::tuple<float, float, float>{
                math::elem(m, 0, 0) * x + math::elem(m, 1, 0) * y + math::elem(m, 2, 0) * z + math::elem(m, 3, 0),
                math::elem(m, 0, 1) * x + math::elem(m, 1, 1) * y + math::elem(m, 2, 1) * z + math::elem(m, 3, 1),
                math::elem(m, 0, 2) * x + math::elem(m, 1, 2) * y + math::elem(m, 2, 2) * z + math::elem(m, 3, 2)};
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
        // 实例键（2026-11-05 稳定键轮）：注册表命中 → "e"+entityGuid 稳定键
        //（跨会话成立，写进 LMAP name）；未命中 → 老 runtime-id 命名（零回归）。
        // mesh 键位用 content geometry id（资产稳定，且与 LightmapModule 侧
        // 同键；editor 管线路径的 proxy.meshId 是 ECS 实体 id，仅会话内有意义）。
        // 与 LightmapModule::UpdateFrame 共用 ResolveLightmapInstanceKey，
        // 两侧镜像一致（EntityIdentityRegistry.h 单一实现）。
        mesh.name = ResolveLightmapInstanceKey((u32)proxy.entityId, geometryId);

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
        stored.source_mesh_id = (std::uint32_t)geometryId;
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
    // §17 观感轮（2026-10-21）：BakeSky 默认值已与运行时解析天空
    // EvaluateSkyBase 逐位对齐（色温 + HDR 档，见 LightmapBaker.h §17 注释）
    // ——v2 时代为远平面暗带引入的 ×2.0 校准（§15）依据已变：当时烘焙
    // 天空峰值 ~0.65 需 ×2 补暗，现在默认即为运行时 GI-miss 同域数值，
    // 无缩放。PRIMAL_LIGHTMAP_SKYINT 保留为微调旋钮（默认 1.0 = 恒等）。
    {
        float skyInt = 1.0f;
        if (const char* e = std::getenv("PRIMAL_LIGHTMAP_SKYINT")) {
            const float v = std::atof(e);
            if (v > 0.0f && v <= 100.0f) skyInt = v;
        }
        if (skyInt != 1.0f) {
            out.scene.sky.intensity *= skyInt;
        }
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
