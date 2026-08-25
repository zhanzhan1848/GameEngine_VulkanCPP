// include 列表对所有编译器统一。此前 _MSC_VER 分支只保留了文件早期版本的 8 个
// include，后来新增的 PipelineLight/Transform/std::cerr 等代码只更新了 __clang__
// 分支——MSVC 编译时 PipelineLightDesc、transform::component_cache、std::cerr
// 全部未声明（report-14 的 C2039/C2065 级联）。GCC 此前无任何分支可用，一并修复。
#include "Common.h"
#include "CommonHeaders.h"
#include "Graphics/RenderPipeline/RenderPipeline.h"
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Graphics/RenderPipeline/StreamingMesh.h"
#include "Graphics/RenderPipeline/PipelineQualityConfig.h"
#include "Graphics/RenderPipeline/GIEditorPanel.h"   // M6/T6.1 GI 面板 API（轻量头）
#include "Graphics/RenderPipeline/GIVolumeGizmo.h"   // M6/T6.2 GI volume gizmo（轻量头）
#include "Graphics/Lumen/LumenTypes.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include "PipelineLightDesc.h"
#include "Components/Transform.h"
#include "EngineAPI/Light.h"
#include "Components/Light.h"
#include "Components/Entity.h"
#include "EngineAPI/GameEntity.h"
#include "EngineAPI/GameEntity_impl.h"
#include "Graphics/Camera/RenderViewBuilder.h"
#include "Graphics/Scene/LightSyncSystem.h"
#include "Graphics/RHI/Core/RHITypes.h"  // for rhi::handles::INVALID_SYNC
#include "Graphics/RenderTexture.h"      // for RenderTexture::GetDesc/GetHandle
#include "Graphics/Renderer.h"           // for graphics::render_surface definition
#include "EngineAPIInternal.h"           // for engine_dll::GetRenderTarget/GetSurface
// include 顺序敏感：StaticProbeBaker.h 经 BVH.hpp 打开 primal::graphics::utl，
// 遮蔽后续头文件对 primal::utl（vector）的名字查找——必须放在上面所有 Engine
// 头之后。其完整类型仅为下方 GIBakeStaticProbes 的 ProbeBakingScene/Params。
#include "Graphics/Lumen/StaticProbe/StaticProbeBaker.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "Engine.lib")
#endif

using namespace primal;
using namespace primal::graphics;

namespace {

// Forward declaration via extern — definition is later in this TU's
// anonymous namespace. GetStdPipeline prefers g_owned_pipeline (set by
// CreateStandardRenderPipeline) over RenderPipeline::Get() to avoid an
// ODR split between the dylib's `s_instance = this` write and its
// inline-Get() read when the test binary also statically links libEngine.a.
extern StandardRenderPipeline* g_owned_pipeline;

StandardRenderPipeline* GetStdPipeline() {
    if (g_owned_pipeline) return g_owned_pipeline;
    auto* p = RenderPipeline::Get();
    return p ? static_cast<StandardRenderPipeline*>(p) : nullptr;
}

// Build quaternion that rotates +Z to `fwd`. Handles degenerate input.
// Used to translate a PipelineLightDesc direction vector into the Transform
// component rotation that the engine's forward-render / deferred lighting
// expects (lights face down +Z by convention in this engine).
primal::math::v4 quat_from_forward(primal::math::v3 fwd) {
    // Clamp tiny vectors to a default to avoid NaN.
    float len_sq = fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z;
    if (len_sq < 1e-10f) {
        return primal::math::v4{0.f, 0.f, 0.f, 1.f};  // identity
    }
    float inv = 1.f / std::sqrt(len_sq);
    fwd.x *= inv; fwd.y *= inv; fwd.z *= inv;

    primal::math::v3 axis{0.f, 0.f, 1.f};
    float d = axis.x * fwd.x + axis.y * fwd.y + axis.z * fwd.z;
    if (d > 0.99999f) return primal::math::v4{0.f, 0.f, 0.f, 1.f};       // identity
    if (d < -0.99999f) return primal::math::v4{0.f, 1.f, 0.f, 0.f};      // 180° around Y

    primal::math::v3 xyz{
        axis.y * fwd.z - axis.z * fwd.y,
        axis.z * fwd.x - axis.x * fwd.z,
        axis.x * fwd.y - axis.y * fwd.x
    };
    float w = 1.f + d;
    // Normalize
    float n = std::sqrt(xyz.x * xyz.x + xyz.y * xyz.y + xyz.z * xyz.z + w * w);
    return primal::math::v4{xyz.x / n, xyz.y / n, xyz.z / n, w / n};
}

// Apply position + optional rotation (from forward direction) to the light
// entity's Transform component via the public component_cache + update() API.
// The transform_id is derived from entity_id (same underlying value).
void apply_light_transform(id::id_type entity_id,
                           const primal::math::v3& position,
                           const primal::math::v3* fwd_optional) {
    transform::component_cache cache{};
    cache.id = transform::transform_id{ entity_id };
    cache.flags = transform::component_flags::position;
    cache.position = position;
    if (fwd_optional) {
        cache.flags |= transform::component_flags::rotation;
        cache.rotation = quat_from_forward(*fwd_optional);
    }
    transform::update(&cache, 1);
}

// Validate all numeric inputs on PipelineLightDesc are finite (rejects NaN/Inf
// which would flow into the engine unguarded and, under -ffast-math, could
// silently corrupt downstream comparisons).
bool light_desc_floats_ok(const PipelineLightDesc* d) {
    using std::isfinite;
    for (int i = 0; i < 3; ++i) {
        if (!isfinite(d->color[i])) return false;
        if (!isfinite(d->position[i])) return false;
        if (!isfinite(d->direction[i])) return false;
    }
    if (!isfinite(d->intensity)) return false;
    if (!isfinite(d->range)) return false;
    if (!isfinite(d->umbra)) return false;
    if (!isfinite(d->penumbra)) return false;
    return true;
}
}

extern "C" {

// ============================================================================
// Pass Toggles
// ============================================================================

EDITOR_INTERFACE void SetRenderPassEnabled(u32 pass_id, u32 enabled) {
    auto* p = GetStdPipeline();
    if (!p) return;
    p->SetPassEnabled(static_cast<RenderPassID>(pass_id), enabled != 0);
}

EDITOR_INTERFACE u32 IsRenderPassEnabled(u32 pass_id) {
    auto* p = GetStdPipeline();
    if (!p) return 0;
    return p->IsPassEnabled(static_cast<RenderPassID>(pass_id)) ? 1u : 0u;
}

EDITOR_INTERFACE u32 IsRenderPassActive(u32 pass_id) {
    auto* p = GetStdPipeline();
    if (!p) return 0;
    return p->IsPassActive(static_cast<RenderPassID>(pass_id)) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GetRenderPassCount() {
    return StandardRenderPipeline::GetPassCount();
}

// ============================================================================
// Settings
// ============================================================================

EDITOR_INTERFACE void GetRenderPipelineSettings(RenderPipelineSettings* out) {
    if (!out) return;
    auto* p = GetStdPipeline();
    if (!p) {
        std::memset(out, 0, sizeof(RenderPipelineSettings));
        return;
    }
    const auto& s = p->GetSettings();
    std::memcpy(out, &s, sizeof(RenderPipelineSettings));
}

EDITOR_INTERFACE void UpdateRenderPipelineSettings(const RenderPipelineSettings* settings) {
    if (!settings) return;
    auto* p = GetStdPipeline();
    if (!p) return;
    p->UpdateSettings(*settings);
}

// ============================================================================
// Parameter Introspection (static data)
// ============================================================================

EDITOR_INTERFACE const ParamDescriptor* GetPipelineParamDescriptors(u32* out_count) {
    if (!out_count) return nullptr;
    return GetParamDescriptors(*out_count);
}

EDITOR_INTERFACE void GetPipelinePassInfo(u32 pass_id,
    const char** out_name, const char** out_description, u32* out_default_enabled) {
    if (pass_id >= static_cast<u32>(RenderPassID::Count)) return;
    const auto& info = GetRenderPassInfo(static_cast<RenderPassID>(pass_id));
    if (out_name)             *out_name = info.name;
    if (out_description)      *out_description = info.description;
    if (out_default_enabled)  *out_default_enabled = info.default_enabled ? 1u : 0u;
}

// ============================================================================
// Viewport
// ============================================================================

EDITOR_INTERFACE void SetPipelineViewportSize(u32 width, u32 height) {
    auto* p = GetStdPipeline();
    if (!p) return;
    p->SetViewportSize(width, height);
}

// ============================================================================
// Shader Hot-Reload
// ============================================================================

EDITOR_INTERFACE u32 ReloadPipelineShader(u64 shader_handle, const void* data, u32 data_size) {
    if (!data || data_size == 0) return 0;
    auto* p = GetStdPipeline();
    if (!p) return 0;
    return p->ReloadShader(
        static_cast<rhi::ShaderHandle>(shader_handle), data, data_size) ? 1u : 0u;
}

// ============================================================================
// Mesh Entity Registration (content_id → ECS entity → render pipeline)
// ============================================================================

// Register a mesh resource with the pipeline and create an ECS entity that
// participates in rendering. geometry_content_id from ProceduralMeshCreate or
// ImportSceneBinary; texture_content_ids is typically [albedo, normal, orm]
// (use 0 / invalid_id for fallback). Returns entity_id, 0 on failure.
EDITOR_INTERFACE u64 PipelineRegisterMeshEntity(u64 geometry_content_id,
                                                const u64* texture_content_ids,
                                                u32 texture_count) {
    auto* p = GetStdPipeline();
    if (!p) return 0;
    // id::invalid_id = 0xffffffff (u32) cast to u64. Slot 0 is a VALID id
    // (first slot allocated by the FreeList). Treat only invalid_id as
    // failure, NOT 0.
    constexpr u64 kInvalidContentId = static_cast<u64>(0xffffffffu);
    if (geometry_content_id == kInvalidContentId) return 0;

    // Convert u64 array → id::id_type array for the engine API.
    // Caller may pass nullptr + texture_count=0 for untextured meshes.
    static thread_local id::id_type tex_buf[8];
    const id::id_type* tex_ptr = nullptr;
    if (texture_content_ids && texture_count > 0) {
        const u32 n = (texture_count < 8) ? texture_count : 8;
        for (u32 i = 0; i < n; ++i) {
            tex_buf[i] = static_cast<id::id_type>(texture_content_ids[i]);
        }
        tex_ptr = tex_buf;
        texture_count = n;
    }

    const id::id_type eid = p->RegisterMeshEntity(
        static_cast<id::id_type>(geometry_content_id), tex_ptr, texture_count);
    return static_cast<u64>(eid);
}

// Unregister a mesh entity and destroy the underlying ECS entity.
// Safe to call with invalid_id (no-op). Note: 0 is a VALID entity id
// (first slot allocated by ECS); only id::invalid_id (0xffffffff) is null.
EDITOR_INTERFACE void PipelineUnregisterMeshEntity(u64 entity_id) {
    auto* p = GetStdPipeline();
    constexpr u64 kInvalidId = static_cast<u64>(0xffffffffu);
    if (!p || entity_id == kInvalidId) return;
    p->UnregisterMeshEntity(static_cast<id::id_type>(entity_id));
}

// ============================================================================
// Streaming Mesh Entity Registration (Phase 9.3b)
// ============================================================================
// GPU-resident streaming meshes (e.g., from GlobalSDFMeshNode) bypass
// content_id and register here directly. Caller owns the StreamingMesh's
// GPU buffers; the pipeline holds only a non-owning pointer.

EDITOR_INTERFACE u64 PipelineRegisterStreamingMeshEntity(
    primal::graphics::StreamingMesh* streaming_mesh,
    const f32* bounds_min,
    const f32* bounds_max)
{
    auto* p = GetStdPipeline();
    if (!p || streaming_mesh == nullptr || !streaming_mesh->IsValid()) return 0;
    if (bounds_min == nullptr || bounds_max == nullptr) return 0;
    auto* scene = p->GetCurrentScene();
    if (scene == nullptr) return 0;

    primal::math::v3 bmin{bounds_min[0], bounds_min[1], bounds_min[2]};
    primal::math::v3 bmax{bounds_max[0], bounds_max[1], bounds_max[2]};
    // C ABI clients are single-buffered (no triple-buffering) — slot=0.
    const id::id_type eid = scene->RegisterStreamingMesh(streaming_mesh, 0, bmin, bmax);
    return eid == id::invalid_id ? 0u : static_cast<u64>(eid);
}

EDITOR_INTERFACE void PipelineUpdateStreamingMeshEntity(
    u64 entity_id,
    u64 generation,
    const f32* bounds_min,
    const f32* bounds_max)
{
    auto* p = GetStdPipeline();
    if (!p || entity_id == 0) return;
    if (bounds_min == nullptr || bounds_max == nullptr) return;
    auto* scene = p->GetCurrentScene();
    if (scene == nullptr) return;

    primal::math::v3 bmin{bounds_min[0], bounds_min[1], bounds_min[2]};
    primal::math::v3 bmax{bounds_max[0], bounds_max[1], bounds_max[2]};
    scene->UpdateStreamingMesh(static_cast<id::id_type>(entity_id), generation, bmin, bmax);
}

EDITOR_INTERFACE void PipelineUnregisterStreamingMeshEntity(u64 entity_id) {
    auto* p = GetStdPipeline();
    if (!p || entity_id == 0) return;
    auto* scene = p->GetCurrentScene();
    if (scene == nullptr) return;
    scene->UnregisterStreamingMesh(static_cast<id::id_type>(entity_id));
}

// ============================================================================
// Editor Mode Toggle
// ============================================================================

// Enable/disable editor mode (lightweight forward rendering path).
EDITOR_INTERFACE void PipelineSetEditorMode(u32 enable) {
    auto* p = GetStdPipeline();
    if (!p) return;
    p->SetEditorMode(enable != 0);
}

EDITOR_INTERFACE u32 PipelineIsEditorMode() {
    auto* p = GetStdPipeline();
    if (!p) return 0;
    return p->IsEditorMode() ? 1u : 0u;
}

// ============================================================================
// Lumen GI Quality Preset
// ============================================================================

// Set Lumen GI quality preset. Triggers Lumen subsystem re-init (expensive —
// do not call per-frame). preset: 0=Off, 1=Low, 2=Medium, 3=High, 4=Ultra, 5=UltraRT.
EDITOR_INTERFACE void PipelineSetLumenConfig(u32 quality_preset) {
    auto* p = GetStdPipeline();
    if (!p) return;
    lumen::LumenConfig config{};
    config.quality = static_cast<lumen::LumenQualityPreset>(quality_preset);
    p->SetLumenConfig(config);
}

// ============================================================================
// Pipeline Lifecycle (Phase 9.3b unblock)
// ============================================================================
// Headless integration tests need a StandardRenderPipeline owned by the dylib
// (not the test binary) so that Pipeline* C ABIs resolve through the dylib's
// RenderPipeline::s_instance. Mixing a test-binary pipeline with dylib C ABI
// calls hits the same ODR issue as CommandBufferManager (see
// GlobalSDF.cpp DebugFill comment).

namespace {
StandardRenderPipeline* g_owned_pipeline = nullptr;
} // anonymous namespace

// Create + Initialize a StandardRenderPipeline owned by the dylib. Idempotent
// (safe to call when already created). device_handle must come from
// GetEngineDeviceHandle. Triggers InitializeSubsystems via SetLumenConfig(Low)
// so forward_renderer_ exists for PipelineRegisterMeshEntity. Returns 1 on
// success (including the "already created" case), 0 on failure.
EDITOR_INTERFACE u32 CreateStandardRenderPipeline(u64 device_handle) {
    if (g_owned_pipeline) return 1;
    if (device_handle == 0) return 0;
    auto* device = reinterpret_cast<rhi::RHIDeviceBase*>(device_handle);
    if (!device) return 0;

    auto* pipeline = new StandardRenderPipeline();
    if (!pipeline->Initialize(device)) {
        std::cerr << "[CreateStandardRenderPipeline] Initialize failed" << std::endl;
        delete pipeline;
        return 0;
    }
    // InitializeSubsystems is lazy-triggered from SetLumenConfig. Low preset
    // keeps memory + startup cost down for tests.
    lumen::LumenConfig config{};
    config.quality = lumen::LumenQualityPreset::Low;
    pipeline->SetLumenConfig(config);

    g_owned_pipeline = pipeline;
    return 1;
}

EDITOR_INTERFACE void DestroyStandardRenderPipeline() {
    if (g_owned_pipeline) {
        g_owned_pipeline->Shutdown();
        delete g_owned_pipeline;
        g_owned_pipeline = nullptr;
    }
}

// ============================================================================
// Light Entity Registration (Path B)
// ============================================================================
//
// C ABI wrappers for StandardRenderPipeline's light entity methods (Task 2).
// PipelineLightDesc translates to engine light_init_info + Transform
// position/rotation. The pipeline creates an ECS entity internally
// (info.entity_id is ignored by RegisterLightEntity); the returned u64 is the
// new entity ID. Use PipelineUpdateLightEntity to mutate it,
// PipelineUnregisterLightEntity to destroy it. Note: 0 is a VALID entity id
// (first slot allocated by ECS); only ~0ull (id::invalid_id) is null.

EDITOR_INTERFACE u64 PipelineRegisterLightEntity(const PipelineLightDesc* desc) {
    constexpr u64 kInvalid = static_cast<u64>(~0ull);
    if (!desc) return kInvalid;
    if (desc->type > 2) return kInvalid;
    if (!light_desc_floats_ok(desc)) return kInvalid;

    auto* p = GetStdPipeline();
    if (!p) return kInvalid;

    // Translate PipelineLightDesc → light_init_info
    light_init_info li{};
    li.entity_id = id::invalid_id;  // pipeline creates entity internally
    li.type = static_cast<graphics::light::type>(desc->type);
    li.color = primal::math::v3{desc->color[0], desc->color[1], desc->color[2]};
    li.intensity = desc->intensity;
    li.is_enabled = desc->is_enabled != 0;
    if (desc->type == 1) {  // point
        li.point_param.range = desc->range;
        li.point_param.attenuation = primal::math::v3{1.f, 0.f, 0.f};
    } else if (desc->type == 2) {  // spot
        li.spot_param.range = desc->range;
        li.spot_param.umbra = desc->umbra;
        li.spot_param.penumbra = desc->penumbra;
        li.spot_param.attenuation = primal::math::v3{1.f, 0.f, 0.f};
    }

    id::id_type eid = p->RegisterLightEntity(li);
    if (eid == id::invalid_id) return kInvalid;

    // Apply transform: position + rotation (from forward direction)
    primal::math::v3 pos{desc->position[0], desc->position[1], desc->position[2]};
    if (desc->type == 0 || desc->type == 2) {
        primal::math::v3 fwd{desc->direction[0], desc->direction[1], desc->direction[2]};
        apply_light_transform(eid, pos, &fwd);
    } else {
        apply_light_transform(eid, pos, nullptr);
    }

    return static_cast<u64>(eid);
}

EDITOR_INTERFACE u32 PipelineUpdateLightEntity(u64 entity_id, const PipelineLightDesc* desc) {
    constexpr u64 kInvalid = static_cast<u64>(~0ull);
    if (!desc || entity_id == kInvalid) return 0;
    if (desc->type > 2) return 0;
    if (!light_desc_floats_ok(desc)) return 0;

    auto* p = GetStdPipeline();
    if (!p) return 0;

    light_init_info li{};
    li.entity_id = id::invalid_id;
    li.type = static_cast<graphics::light::type>(desc->type);
    li.color = primal::math::v3{desc->color[0], desc->color[1], desc->color[2]};
    li.intensity = desc->intensity;
    li.is_enabled = desc->is_enabled != 0;
    if (desc->type == 1) {
        li.point_param.range = desc->range;
        li.point_param.attenuation = primal::math::v3{1.f, 0.f, 0.f};
    } else if (desc->type == 2) {
        li.spot_param.range = desc->range;
        li.spot_param.umbra = desc->umbra;
        li.spot_param.penumbra = desc->penumbra;
        li.spot_param.attenuation = primal::math::v3{1.f, 0.f, 0.f};
    }

    const id::id_type eid = static_cast<id::id_type>(entity_id);
    if (!p->UpdateLightEntity(eid, li)) return 0;

    // Update transform
    primal::math::v3 pos{desc->position[0], desc->position[1], desc->position[2]};
    if (desc->type == 0 || desc->type == 2) {
        primal::math::v3 fwd{desc->direction[0], desc->direction[1], desc->direction[2]};
        apply_light_transform(eid, pos, &fwd);
    } else {
        apply_light_transform(eid, pos, nullptr);
    }
    return 1;
}

EDITOR_INTERFACE void PipelineUnregisterLightEntity(u64 entity_id) {
    constexpr u64 kInvalid = static_cast<u64>(~0ull);
    if (entity_id == kInvalid) return;
    auto* p = GetStdPipeline();
    if (!p) return;
    p->UnregisterLightEntity(static_cast<id::id_type>(entity_id));
}

// ============================================================================
// Pipeline Render Frame (Path B trigger)
// ============================================================================
//
// Central render entry for offscreen / headless rendering. Validates target /
// pipeline / scene, builds a RenderView from camera_id + viewport derived from
// the target texture, syncs ECS lights into the RenderScene, and invokes
// pipeline->Render. surface_id is optional — pass u32(-1) to skip the
// surface-size sanity check (headless tests with no drawable).

EDITOR_INTERFACE u32 PipelineRenderFrame(u64 target_handle, u32 camera_id_in, u32 surface_id) {
    // Spec §4.1 step 2: invalid camera_id → return 0. BuildRenderViewFromCameraId
    // silently returns a default view for invalid ids, so check upfront.
    constexpr u32 kInvalidCameraId = static_cast<u32>(~0u);
    if (camera_id_in == kInvalidCameraId) {
        std::fprintf(stderr, "[PipelineRenderFrame] invalid camera_id %u\n", camera_id_in);
        return 0;
    }

    auto* entry = engine_dll::GetRenderTarget(target_handle);
    if (!entry || !entry->texture) {
        std::fprintf(stderr, "[PipelineRenderFrame] invalid target_handle %llu\n",
                     static_cast<unsigned long long>(target_handle));
        return 0;
    }

    auto* p = GetStdPipeline();
    if (!p) {
        std::fprintf(stderr, "[PipelineRenderFrame] no pipeline\n");
        return 0;
    }
    // Spec §4.1 step 3: GetCurrentScene is non-null after Initialize
    // (StandardRenderPipeline owns default_scene_). Null here is a real
    // engine bug, not a "first frame" condition — return 0 per the error matrix.
    RenderScene* scene = p->GetCurrentScene();
    if (!scene) {
        std::fprintf(stderr, "[PipelineRenderFrame] no scene\n");
        return 0;
    }

    const auto& desc = entry->texture->GetDesc();
    if (desc.size.x == 0 || desc.size.y == 0) {
        std::fprintf(stderr, "[PipelineRenderFrame] zero-sized target\n");
        return 0;
    }

    // Optional surface size sanity check. surface_id == ~0u means headless / skip.
    if (surface_id != static_cast<u32>(~0u)) {
        auto* rs = engine_dll::GetSurface(surface_id);
        if (rs && rs->surface.is_valid()) {
            if (rs->surface.width() != desc.size.x ||
                rs->surface.height() != desc.size.y) {
                std::fprintf(stderr,
                    "[PipelineRenderFrame] target %ux%u vs surface %ux%u (warn, continue)\n",
                    desc.size.x, desc.size.y,
                    rs->surface.width(), rs->surface.height());
            }
        }
    }

    rhi::ViewportDesc viewport{};
    viewport.topLeft.x = 0.f;
    viewport.topLeft.y = 0.f;
    viewport.size.x = static_cast<f32>(desc.size.x);
    viewport.size.y = static_cast<f32>(desc.size.y);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;

    RenderView view = BuildRenderViewFromCameraId(camera_id{camera_id_in}, viewport);

    // Sync ECS lights to RenderScene (Path B lights source). Idempotent per-frame.
    scene_sync::SyncLightsFromECS(*scene);

    p->Render(*scene, view, entry->texture->GetHandle(), desc,
              rhi::handles::INVALID_SYNC);
    return 1;
}

// ============================================================================
// BlitRenderTargetToSurface (Path B present step)
// ============================================================================
//
// After PipelineRenderFrame has rendered offscreen into `target_handle`,
// the Editor calls BlitRenderTargetToSurface to copy the render target's
// texture into the MTKView drawable and present it. Because drawables are
// framebufferOnly (per memory: Material Preview Fragment Blit), this is
// implemented as a fullscreen-triangle fragment shader that samples `src`,
// not an MTLBlitCommandEncoder copy.
//
// Returns 1 on success, 0 on failure (invalid target / invalid surface /
// pipeline not initialized / etc.).

EDITOR_INTERFACE u32 BlitRenderTargetToSurface(u64 target_handle, u32 surface_id) {
    auto* entry = engine_dll::GetRenderTarget(target_handle);
    if (!entry || !entry->texture) {
        std::fprintf(stderr, "[BlitRenderTargetToSurface] invalid target_handle %llu\n",
                     static_cast<unsigned long long>(target_handle));
        return 0;
    }

    auto* rs = engine_dll::GetSurface(surface_id);
    if (!rs || !rs->surface.is_valid()) {
        std::fprintf(stderr, "[BlitRenderTargetToSurface] invalid surface_id %u\n",
                     surface_id);
        return 0;
    }

    // Spec §4.2 step 3: hard-fail on size mismatch (blit cannot scale).
    const auto& target_desc = entry->texture->GetDesc();
    const u32 target_w = static_cast<u32>(target_desc.size.x);
    const u32 target_h = static_cast<u32>(target_desc.size.y);
    const u32 surf_w   = rs->surface.width();
    const u32 surf_h   = rs->surface.height();
    if (target_w != surf_w || target_h != surf_h) {
        std::fprintf(stderr,
            "[BlitRenderTargetToSurface] size mismatch: target %ux%u vs surface %ux%u\n",
            target_w, target_h, surf_w, surf_h);
        return 0;
    }

    // Delegate to surface::blit_and_present, which dispatches through
    // platform_interface → metal::core::blit_surface_and_present. That
    // resolves the RHI ResourceHandle → MTL::Texture*, lazy-inits the
    // blit PSO, acquires the currentDrawable, renders the fullscreen
    // triangle, and presents.
    return rs->surface.blit_and_present(entry->texture->GetHandle());
}

// ============================================================================
// GI Editor Panel (M6/T6.1)
// ============================================================================
//
// GI 面板数据面的 C ABI 投影。UI 宿主（PrimalEditor / WASM shell / 测试）
// 消费方式：
//   - 控件：GetPipelineParamDescriptors（含 "GI Axes" 五轴分组，base =
//     RenderPipelineSettings）+ GetGIDebugParamDescriptors（base =
//     GIDebugParams 进程单例，写后即时生效）+ GetGIVolumeParamDescriptors
//     （base = LumenVolumeConfig，配 volume 列表 CRUD 使用）；
//   - axes 提交：改 GI Axes 控件后调 GIApplyAxesConfig（SetLumenConfig 热重
//     载，轴1 覆写 pass 开关），不要只 UpdateRenderPipelineSettings；
//   - volume 提交：Set/Append/Remove VolumeOverride 内部已置
//     settings_dirty_ —— 下一渲染帧 ApplyConfigChanges 自动重建 Lumen
//     passes（counts/spacing 变更必须重建，即"改后自动触发重初始化"）；
//   - Bake 按钮：GIBakeStaticProbes（grid 取当前 volume 0 配置；CPU 几何
//     由宿主传入；须在渲染帧之外调用）；
//   - 按钮通用机制：GIRegisterPanelAction / GIGetPanelAction / GIInvokePanelAction。
//
// 默认零开销：所有函数被动调用，无每帧轮询；面板不打开 = 无人调用。

EDITOR_INTERFACE const ParamDescriptor* GetGIDebugParamDescriptors(u32* out_count) {
    if (!out_count) return nullptr;
    return gi_panel::GetDebugParamDescriptors(*out_count);
}

EDITOR_INTERFACE const ParamDescriptor* GetGIVolumeParamDescriptors(u32* out_count) {
    if (!out_count) return nullptr;
    return gi_panel::GetVolumeParamDescriptors(*out_count);
}

// --- GIDebugParams 运行时旋钮（进程单例，写后即时生效；三端 AddPass 读取） ---

EDITOR_INTERFACE void GIGetDebugParams(lumen::GIDebugParams* out) {
    if (!out) return;
    *out = lumen::GIDebug();
}

EDITOR_INTERFACE void GISetDebugParams(const lumen::GIDebugParams* params) {
    if (!params) return;
    lumen::GIDebug() = *params;
}

// --- 五轴热重载（SetLumenConfig 路径，勿绕行 UpdateRenderPipelineSettings） ---

EDITOR_INTERFACE u32 GIApplyAxesConfig(const lumen::GIFiveAxisConfig* axes) {
    if (!axes) return 0;
    auto* p = GetStdPipeline();
    if (!p) return 0;
    return gi_panel::ApplyAxesConfig(p, *axes) ? 1u : 0u;
}

// --- volume 列表 CRUD（cap 4；提交自动置 settings_dirty_ 触发下帧重建） ---

EDITOR_INTERFACE u32 GIGetVolumeOverrideCount() {
    auto* p = GetStdPipeline();
    return p ? gi_panel::GetVolumeOverrideCount(p) : 0u;
}

EDITOR_INTERFACE u32 GIGetVolumeOverride(u32 index, lumen::LumenVolumeConfig* out) {
    if (!out) return 0;
    auto* p = GetStdPipeline();
    return (p && gi_panel::GetVolumeOverride(p, index, out)) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GISetVolumeOverride(u32 index, const lumen::LumenVolumeConfig* v) {
    if (!v) return 0;
    auto* p = GetStdPipeline();
    return (p && gi_panel::SetVolumeOverride(p, index, *v)) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GIAppendVolumeOverride() {
    auto* p = GetStdPipeline();
    return (p && gi_panel::AppendVolumeOverride(p)) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GIRemoveVolumeOverride(u32 index) {
    auto* p = GetStdPipeline();
    return (p && gi_panel::RemoveVolumeOverride(p, index)) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GIMaterializeDerivedVolumes() {
    auto* p = GetStdPipeline();
    return (p && gi_panel::MaterializeDerivedVolumes(p)) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GIGetEffectiveVolumeConfig(u32 index, lumen::LumenVolumeConfig* out) {
    if (!out) return 0;
    auto* p = GetStdPipeline();
    return (p && gi_panel::GetEffectiveVolumeConfig(p, index, out)) ? 1u : 0u;
}

// --- Bake（grid 取当前生效 volume 0 配置；CPU 几何宿主提供；渲染帧外调用） ---
// vertices: vertex_count * 3 个 float（xyz 三元组）；indices: index_count 个
// u32。light_dir/light_color/sky_color 可为 null（用 ProbeBakingScene 默认）。
// rays_per_probe=0 / bounce_count=0 → ProbeBakingParams 默认。

EDITOR_INTERFACE u32 GIBakeStaticProbes(const f32* vertices, u32 vertex_count,
                                         const u32* indices, u32 index_count,
                                         const f32* light_dir, const f32* light_color,
                                         f32 light_intensity, const f32* sky_color,
                                         u32 rays_per_probe, u32 bounce_count,
                                         char* out_status, u32 status_cap) {
    auto* p = GetStdPipeline();
    if (!p) {
        if (out_status && status_cap)
            std::snprintf(out_status, status_cap, "Bake failed: no pipeline");
        return 0;
    }
    if ((!vertices && vertex_count) || (!indices && index_count) ||
        (vertex_count && index_count % 3)) {
        if (out_status && status_cap)
            std::snprintf(out_status, status_cap, "Bake failed: bad geometry args");
        return 0;
    }

    // f32 三元组 → primal::math::v3（保持 ProbeBakingScene 的 v3 指针布局）
    std::vector<primal::math::v3> verts;
    std::vector<u32> idx;
    if (vertex_count) {
        verts.reserve(vertex_count);
        for (u32 i = 0; i < vertex_count; ++i)
            verts.push_back(primal::math::v3{vertices[i * 3], vertices[i * 3 + 1], vertices[i * 3 + 2]});
        idx.assign(indices, indices + index_count);
    }

    lumen::ProbeBakingScene scene{};
    if (!verts.empty()) {
        scene.vertices = verts.data();
        scene.indices = idx.data();
        scene.vertex_count = vertex_count;
        scene.index_count = index_count;
    }
    if (light_dir)   scene.light_direction = primal::math::v3{light_dir[0], light_dir[1], light_dir[2]};
    if (light_color) scene.light_color = primal::math::v3{light_color[0], light_color[1], light_color[2]};
    scene.light_intensity = light_intensity;
    if (sky_color)   scene.sky_color = primal::math::v3{sky_color[0], sky_color[1], sky_color[2]};

    lumen::ProbeBakingParams params{};
    if (rays_per_probe) params.rays_per_probe = rays_per_probe;
    if (bounce_count)   params.bounce_count = bounce_count;

    return gi_panel::BakeStaticProbes(p, scene, params, out_status, status_cap) ? 1u : 0u;
}

// --- 面板动作（按钮）通用注册机制（被动容器；不注册 = 零开销） ---

EDITOR_INTERFACE void GIRegisterPanelAction(const char* id, const char* label,
                                            const char* tooltip,
                                            gi_panel::PanelActionFn fn, void* user_data) {
    if (!id || !fn) return;
    gi_panel::PanelAction a{};
    a.id = id;
    a.label = label ? label : id;
    a.tooltip = tooltip ? tooltip : "";
    a.fn = fn;
    a.user_data = user_data;
    gi_panel::RegisterAction(a);
}

EDITOR_INTERFACE void GIUnregisterPanelAction(const char* id) {
    gi_panel::UnregisterAction(id);
}

EDITOR_INTERFACE void GIClearPanelActions() {
    gi_panel::ClearActions();
}

EDITOR_INTERFACE u32 GIGetPanelActionCount() {
    return gi_panel::GetActionCount();
}

EDITOR_INTERFACE u32 GIGetPanelAction(u32 index, const char** out_id,
                                      const char** out_label, const char** out_tooltip) {
    const gi_panel::PanelAction* a = gi_panel::GetActionByIndex(index);
    if (!a) return 0;
    if (out_id)       *out_id = a->id;
    if (out_label)    *out_label = a->label;
    if (out_tooltip)  *out_tooltip = a->tooltip;
    return 1;
}

EDITOR_INTERFACE u32 GIInvokePanelAction(const char* id) {
    if (!id) return 0;
    return gi_panel::InvokeAction(id) ? 1u : 0u;
}

// ============================================================================
// GI Volume Gizmo (M6/T6.2)
// ============================================================================
//
// world volume 的 viewport 拾取/平移/缩放（引擎侧纯数学/数据层
// Engine/Graphics/RenderPipeline/GIVolumeGizmo.h 的 C ABI 投影）。UI 宿主
// 消费方式（典型帧内交互，均在渲染帧之外或帧间隙调用写路径）：
//   - 模式按钮：GIGizmoSetMode（或经 GIRegisterPanelAction + 引擎内
//     gi_gizmo::RegisterDefaultActions 注册的 gizmo.mode.* 四个内置动作）；
//   - 点击拾取：GIGizmoPick —— Select 模式下命中自动 SetSelectedVolume +
//     记录 tMin 为拖拽参考距离，未命中自动取消选中；Translate/Scale 模式
//     下只返回命中信息不动选中态（宿主自行驱动）；
//   - 拖拽平移：宿主用两次 ScreenRay（或自算射线）在引擎侧 DragDelta 换
//     世界位移后调 GIGizmoTranslate（写 origin_override + use_origin_override）；
//   - 拖拽缩放：GIGizmoScale（factor 乘 probe_count[axis]，clamp 4..128；
//     axis 0/1/2 = x/y/z，3 = 三轴同缩）；
//   - 视口画盒：GIGizmoGetWireframe（24 端点 × 3 float 经输出缓冲区返回，
//     计数经 outCount 返回；容量不足返回 0 且 *outCount = 需要的端点数）。
// 拾取/编辑对象 = settings.lumen.volume_overrides（overrides 为空时 Pick 恒
// 未命中 —— 宿主先 GIMaterializeDerivedVolumes 物化）。写路径均 copy →
// UpdateSettings 置 settings_dirty_，下一渲染帧自动重建 Lumen passes。

EDITOR_INTERFACE void GIGizmoSetMode(u32 mode) {
    if (mode > static_cast<u32>(gi_gizmo::GizmoMode::Scale)) return;
    gi_gizmo::SetMode(static_cast<gi_gizmo::GizmoMode>(mode));
}

EDITOR_INTERFACE u32 GIGizmoGetMode() {
    return static_cast<u32>(gi_gizmo::GetMode());
}

EDITOR_INTERFACE u32 GIGizmoGetSelectedVolume() {
    const u32 sel = gi_gizmo::GetSelectedVolume();
    return (sel == gi_gizmo::kNoVolumeSelected) ? 0xFFFFFFFFu : sel;
}

// invViewProj: 16 个 float，列主序（m.columns[c] = 第 c 列）。
// viewOrigin: 3 个 float。outVolumeIndex/outTMin 可为 null。
EDITOR_INTERFACE u32 GIGizmoPick(f32 screenX, f32 screenY,
                                 u32 viewportW, u32 viewportH,
                                 const f32* invViewProj, const f32* viewOrigin,
                                 u32* outVolumeIndex, f32* outTMin) {
    if (!invViewProj || !viewOrigin) return 0;
    primal::math::m4x4 m{};
    for (u32 c = 0; c < 4; ++c)
        m.columns[c] = primal::math::v4{invViewProj[c * 4 + 0], invViewProj[c * 4 + 1],
                                invViewProj[c * 4 + 2], invViewProj[c * 4 + 3]};
    const primal::math::v3 origin{viewOrigin[0], viewOrigin[1], viewOrigin[2]};

    auto* p = GetStdPipeline();
    const gi_gizmo::GIVolumeHit h =
        p ? gi_gizmo::PickPipelineVolumes(p, screenX, screenY, viewportW,
                                          viewportH, m, origin)
          : gi_gizmo::GIVolumeHit{};
    if (!h.hit) {
        // Select 模式点击空白 = 取消选中（典型 gizmo UX）
        if (gi_gizmo::GetMode() == gi_gizmo::GizmoMode::Select)
            gi_gizmo::SetSelectedVolume(gi_gizmo::kNoVolumeSelected);
        return 0;
    }
    if (outVolumeIndex) *outVolumeIndex = h.volumeIndex;
    if (outTMin)        *outTMin        = h.tMin;
    if (gi_gizmo::GetMode() == gi_gizmo::GizmoMode::Select) {
        gi_gizmo::SetSelectedVolume(h.volumeIndex);
        gi_gizmo::SetDragReferenceDistance(h.tMin);
    }
    return 1;
}

// delta: 3 个 float（世界位移，典型来自 gi_gizmo::DragDelta）。
EDITOR_INTERFACE u32 GIGizmoTranslate(u32 index, const f32* delta) {
    if (!delta) return 0;
    auto* p = GetStdPipeline();
    if (!p) return 0;
    const primal::math::v3 d{delta[0], delta[1], delta[2]};
    return gi_gizmo::TranslateVolume(p, index, d) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GIGizmoScale(u32 index, f32 factor, u32 axis) {
    auto* p = GetStdPipeline();
    if (!p) return 0;
    return gi_gizmo::ScaleVolume(p, index, factor, axis) ? 1u : 0u;
}

// outEndpoints: 调用方缓冲区（float 数）；capacity = 缓冲区 float 容量。
// 成功写入 24 端点 × 3 = 72 float，*outCount = 24 并返回 1。
// 容量不足/无管线/越界 → 返回 0（容量不足时 *outCount = 24 供重试分配）。
EDITOR_INTERFACE u32 GIGizmoGetWireframe(u32 index, f32* outEndpoints,
                                         u32 capacity, u32* outCount) {
    const u32 requiredFloats = gi_gizmo::kWireframeEndpointCount * 3;
    if (outCount) *outCount = 0;
    if (!outCount) return 0;
    if (!outEndpoints || capacity < requiredFloats) {
        *outCount = gi_gizmo::kWireframeEndpointCount;   // 供宿主重试分配
        return 0;
    }
    auto* p = GetStdPipeline();
    if (!p) return 0;
    primal::math::v3 endpoints[gi_gizmo::kWireframeEndpointCount];
    if (!gi_gizmo::GetPipelineVolumeWireframe(p, index, endpoints)) return 0;
    for (u32 i = 0; i < gi_gizmo::kWireframeEndpointCount; ++i) {
        outEndpoints[i * 3 + 0] = endpoints[i].x;
        outEndpoints[i * 3 + 1] = endpoints[i].y;
        outEndpoints[i * 3 + 2] = endpoints[i].z;
    }
    *outCount = gi_gizmo::kWireframeEndpointCount;
    return 1;
}

} // extern "C"
