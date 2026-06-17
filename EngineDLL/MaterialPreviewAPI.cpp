// MaterialPreview C API: thin wrapper around MaterialPreviewRenderer for P/Invoke callers.
// Caller owns the RHI device and command buffer; handles are passed as u64.
// preview_id is 1-based (0 is the error sentinel). All functions are no-throw.

#include "Common.h"
#include "CommonHeaders.h"
#include "../Engine/Graphics/RHI/Core/RHIDevice.h"
#include "../Engine/Graphics/RHI/Core/RHICommand.h"
#include "../Engine/Graphics/MaterialPreview/MaterialPreviewRenderer.h"
#include "../Engine/Graphics/MaterialGraph/MaterialGraph.h"
#include "../Engine/Graphics/MaterialGraph/MaterialGraphSerializer.h"
#include "../Engine/Content/ContentToEngine.h"

#include <iostream>
#include <new>

using namespace primal;
using namespace primal::graphics;

namespace {

// 1-based slot map; index 0 is reserved as invalid.
utl::vector<MaterialPreviewRenderer*> previews;

u32 slot_to_index(u32 preview_id) {
    if (preview_id == 0 || preview_id > previews.size()) return UINT32_MAX;
    MaterialPreviewRenderer* r = previews[preview_id - 1];
    return r ? (preview_id - 1) : UINT32_MAX;
}

} // anonymous namespace

extern "C" {

// --- Lifecycle ---

EDITOR_INTERFACE u32 CreateMaterialPreview(u64 device_handle, u32 width, u32 height) {
    auto* device = reinterpret_cast<rhi::RHIDeviceBase*>(device_handle);
    if (!device) {
        std::cerr << "[MaterialPreviewAPI] CreateMaterialPreview: null device_handle\n";
        return 0;
    }

    auto* renderer = new (std::nothrow) MaterialPreviewRenderer();
    if (!renderer) {
        std::cerr << "[MaterialPreviewAPI] CreateMaterialPreview: allocation failed\n";
        return 0;
    }
    if (!renderer->Initialize(device, width, height)) {
        std::cerr << "[MaterialPreviewAPI] CreateMaterialPreview: Initialize failed\n";
        delete renderer;
        return 0;
    }

    previews.push_back(renderer);
    return static_cast<u32>(previews.size());  // 1-based id
}

EDITOR_INTERFACE void DestroyMaterialPreview(u32 preview_id) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return;
    MaterialPreviewRenderer* r = previews[idx];
    r->Shutdown();
    delete r;
    previews[idx] = nullptr;
}

EDITOR_INTERFACE void DestroyAllMaterialPreviews() {
    for (auto*& r : previews) {
        if (r) {
            r->Shutdown();
            delete r;
            r = nullptr;
        }
    }
    previews.clear();
    // Procedural meshes created by MaterialPreviewRenderer::CreateMeshes live in
    // this dylib's rhi_mesh_assets free_list. Clear them here so the dylib's
    // static destructors don't trip the ~free_list _size != 0 assertion.
    primal::content::shutdown();
}

// --- Configuration ---

EDITOR_INTERFACE u32 SetMaterialPreviewGraph(u32 preview_id, const char* graph_json) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX || !graph_json) return 0;

    material_graph::MaterialGraph graph;
    if (!material_graph::MaterialGraphSerializer::DeserializeIntoGraph(
            std::string(graph_json), graph)) {
        std::cerr << "[MaterialPreviewAPI] SetMaterialPreviewGraph: JSON parse failed\n";
        return 0;
    }
    return previews[idx]->SetMaterialGraph(graph) ? 1u : 0u;
}

EDITOR_INTERFACE void SetMaterialPreviewModel(u32 preview_id, u32 model_type) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return;
    if (model_type >= static_cast<u32>(PreviewModelType::Count)) return;
    previews[idx]->SetPreviewModel(static_cast<PreviewModelType>(model_type));
}

EDITOR_INTERFACE u32 SetMaterialPreviewMesh(u32 preview_id, u64 geometry_content_id) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return 0;
    return previews[idx]->SetPreviewMeshFromAsset(
        static_cast<id::id_type>(geometry_content_id)) ? 1u : 0u;
}

EDITOR_INTERFACE void ClearMaterialPreviewMesh(u32 preview_id) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return;
    previews[idx]->ClearPreviewMeshOverride();
}

EDITOR_INTERFACE void SetMaterialPreviewMode2D(u32 preview_id, u32 enable) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return;
    previews[idx]->SetMode2D(enable != 0);
}

EDITOR_INTERFACE u32 IsMaterialPreviewMode2D(u32 preview_id) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return 0;
    return previews[idx]->IsMode2D() ? 1u : 0u;
}

EDITOR_INTERFACE void ResizeMaterialPreview(u32 preview_id, u32 width, u32 height) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return;
    previews[idx]->Resize(width, height);
}

// --- Rendering ---

// Returns the color target ResourceHandle as u64, or 0 on failure.
EDITOR_INTERFACE u64 RenderMaterialPreview(u32 preview_id, u64 cmd_handle, u32 frame_index, f32 dt) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return 0;
    auto* cmd = reinterpret_cast<rhi::RHICommandBuffer*>(cmd_handle);
    if (!cmd) return 0;
    rhi::ResourceHandle tex = previews[idx]->Render(cmd, frame_index, dt);
    return static_cast<u64>(tex);
}

// Fragment-shader blit into a swap chain backbuffer. Replaces cmd->BlitTexture because
// Metal swap chain drawables are framebufferOnly and reject MTLBlitCommandEncoder writes.
EDITOR_INTERFACE void BlitMaterialPreviewToBackbuffer(u32 preview_id, u64 cmd_handle,
                                                       u64 dst_texture_handle,
                                                       u32 dst_width, u32 dst_height) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return;
    auto* cmd = reinterpret_cast<rhi::RHICommandBuffer*>(cmd_handle);
    if (!cmd) return;
    previews[idx]->BlitToTexture(cmd, static_cast<rhi::ResourceHandle>(dst_texture_handle),
                                  dst_width, dst_height);
}

EDITOR_INTERFACE u64 GetMaterialPreviewTexture(u32 preview_id) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return 0;
    rhi::ResourceHandle tex = previews[idx]->GetOutputTexture();
    return static_cast<u64>(tex);
}

EDITOR_INTERFACE u32 GetMaterialPreviewWidth(u32 preview_id) {
    u32 idx = slot_to_index(preview_id);
    return (idx == UINT32_MAX) ? 0u : previews[idx]->GetWidth();
}

EDITOR_INTERFACE u32 GetMaterialPreviewHeight(u32 preview_id) {
    u32 idx = slot_to_index(preview_id);
    return (idx == UINT32_MAX) ? 0u : previews[idx]->GetHeight();
}

// --- Camera configuration ---

EDITOR_INTERFACE void SetMaterialPreviewCameraPosition(u32 preview_id, const f32* xyz) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX || !xyz) return;
    previews[idx]->SetCameraPosition(xyz[0], xyz[1], xyz[2]);
}

EDITOR_INTERFACE void GetMaterialPreviewCameraPosition(u32 preview_id, f32* out_xyz) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX || !out_xyz) return;
    auto v = previews[idx]->GetCameraPosition();
    out_xyz[0] = v.x; out_xyz[1] = v.y; out_xyz[2] = v.z;
}

EDITOR_INTERFACE void SetMaterialPreviewCameraTarget(u32 preview_id, const f32* xyz) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX || !xyz) return;
    previews[idx]->SetCameraTarget(xyz[0], xyz[1], xyz[2]);
}

EDITOR_INTERFACE void SetMaterialPreviewCameraFov(u32 preview_id, f32 radians) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return;
    previews[idx]->SetCameraFov(radians);
}

EDITOR_INTERFACE void SetMaterialPreviewRotationSpeed(u32 preview_id, f32 rad_per_sec) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return;
    previews[idx]->SetRotationSpeed(rad_per_sec);
}

// --- Light configuration ---

EDITOR_INTERFACE void SetMaterialPreviewLightDirection(u32 preview_id, const f32* normalized_dir) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX || !normalized_dir) return;
    previews[idx]->SetLightDirection(normalized_dir[0], normalized_dir[1], normalized_dir[2]);
}

EDITOR_INTERFACE void SetMaterialPreviewLightColor(u32 preview_id, const f32* rgb) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX || !rgb) return;
    previews[idx]->SetLightColor(rgb[0], rgb[1], rgb[2]);
}

EDITOR_INTERFACE void SetMaterialPreviewLightIntensity(u32 preview_id, f32 intensity) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return;
    previews[idx]->SetLightIntensity(intensity);
}

// --- Background configuration ---

EDITOR_INTERFACE void SetMaterialPreviewBackgroundMode(u32 preview_id, u32 mode) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return;
    // mode: 0 = Gradient, 1 = SolidColor
    if (mode > 1) return;
    previews[idx]->SetBackgroundMode(static_cast<PreviewBackgroundMode>(mode));
}

EDITOR_INTERFACE void SetMaterialPreviewBackgroundColors(u32 preview_id,
                                                          const f32* top_rgb,
                                                          const f32* bot_rgb) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return;
    if (top_rgb && bot_rgb) {
        previews[idx]->SetBackgroundColors(
            top_rgb[0], top_rgb[1], top_rgb[2],
            bot_rgb[0], bot_rgb[1], bot_rgb[2]);
    }
}

} // extern "C"
