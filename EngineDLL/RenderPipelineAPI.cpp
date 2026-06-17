#if defined(_MSC_VER)
#include "Common.h"
#include "CommonHeaders.h"
#include "Graphics/RenderPipeline/RenderPipeline.h"
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Graphics/RenderPipeline/PipelineQualityConfig.h"
#include "Graphics/Lumen/LumenTypes.h"
#include <cstring>

#pragma comment(lib, "Engine.lib")

#elif defined(__clang__)
#include "Common.h"
#include "CommonHeaders.h"
#include "Graphics/RenderPipeline/RenderPipeline.h"
#include "Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Graphics/RenderPipeline/PipelineQualityConfig.h"
#include "Graphics/Lumen/LumenTypes.h"
#include <cstring>

#endif

using namespace primal;
using namespace primal::graphics;

namespace {
StandardRenderPipeline* GetStdPipeline() {
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
    if (!p || geometry_content_id == 0) return 0;

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
// Safe to call with 0 / invalid_id (no-op).
EDITOR_INTERFACE void PipelineUnregisterMeshEntity(u64 entity_id) {
    auto* p = GetStdPipeline();
    if (!p || entity_id == 0) return;
    p->UnregisterMeshEntity(static_cast<id::id_type>(entity_id));
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

} // extern "C"
