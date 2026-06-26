#if defined(_MSC_VER)
#include "Common.h"
#include "CommonHeaders.h"
#include "Graphics/RenderPipeline/RenderPipeline.h"
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Graphics/RenderPipeline/StreamingMesh.h"
#include "Graphics/RenderPipeline/PipelineQualityConfig.h"
#include "Graphics/Lumen/LumenTypes.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include <cstring>

#pragma comment(lib, "Engine.lib")

#elif defined(__clang__)
#include "Common.h"
#include "CommonHeaders.h"
#include "Graphics/RenderPipeline/RenderPipeline.h"
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Graphics/RenderPipeline/StreamingMesh.h"
#include "Graphics/RenderPipeline/PipelineQualityConfig.h"
#include "Graphics/Lumen/LumenTypes.h"
#include "Graphics/Scene/RenderSceneSnapshot.h"
#include "Graphics/RenderScene.h"  // full type for unique_ptr<RenderScene> in g_fallback_scene
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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>

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

// Fallback RenderScene used by PipelineRenderFrame when the pipeline has no
// current_scene_ (i.e., no prior Render() call in this process). The scene is
// synced from ECS each frame via scene_sync::SyncLightsFromECS / SyncEntities.
// Owned by the dylib so it lives as long as the pipeline.
RenderScene* g_fallback_scene = nullptr;

RenderScene* GetFallbackScene() {
    if (!g_fallback_scene) g_fallback_scene = new RenderScene();
    return g_fallback_scene;
}

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
    // Fallback scene is owned by the dylib; release on pipeline teardown.
    delete g_fallback_scene;
    g_fallback_scene = nullptr;
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
    // Prefer the pipeline's current scene (set by a prior Render() call); fall
    // back to a dylib-owned scene for headless / first-frame use. The fallback
    // path is the common case for offscreen Editor rendering and integration
    // tests that never went through a windowed Render() path.
    RenderScene* scene = p->GetCurrentScene();
    if (!scene) scene = GetFallbackScene();
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

} // extern "C"
