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
}

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

} // extern "C"
