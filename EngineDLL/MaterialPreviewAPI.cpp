// MaterialPreview C API: thin wrapper around MaterialPreviewRenderer for P/Invoke callers.
// Caller owns the RHI device and command buffer; handles are passed as u64.
// preview_id is 1-based (0 is the error sentinel). All functions are no-throw.

#include "Common.h"
#include "CommonHeaders.h"
#include "../Engine/Graphics/RHI/Core/RHIDevice.h"
#include "../Engine/Graphics/RHI/Core/RHIResource.h"
#include "../Engine/Graphics/RHI/Core/RHICommand.h"
#include "../Engine/Graphics/MaterialPreview/MaterialPreviewRenderer.h"
#include "../Engine/Graphics/MaterialGraph/MaterialGraph.h"
#include "../Engine/Graphics/MaterialGraph/MaterialGraphSerializer.h"
#include "../Engine/Content/ContentToEngine.h"

#include <iostream>
#include <new>
#include "Engine/Common/CoreLogStream.h"

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
        PRIMAL_LOG_STREAM(Error, "Engine") << "[MaterialPreviewAPI] CreateMaterialPreview: null device_handle\n";
        return 0;
    }

    auto* renderer = new (std::nothrow) MaterialPreviewRenderer();
    if (!renderer) {
        PRIMAL_LOG_STREAM(Error, "Engine") << "[MaterialPreviewAPI] CreateMaterialPreview: allocation failed\n";
        return 0;
    }
    if (!renderer->Initialize(device, width, height)) {
        PRIMAL_LOG_STREAM(Error, "Engine") << "[MaterialPreviewAPI] CreateMaterialPreview: Initialize failed\n";
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
        PRIMAL_LOG_STREAM(Error, "Engine") << "[MaterialPreviewAPI] SetMaterialPreviewGraph: JSON parse failed\n";
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

// One-shot synchronous render: caller passes the RHI device handle (obtained
// via GetEngineDeviceHandle) and a pre-allocated RGBA8 buffer of width*height*4
// bytes. Creates a Graphics cmd buffer, renders preview into color_target, then
// uses a Transfer cmd buffer to copy color_target → Readback buffer, submits,
// waits, maps, swaps BGRA→RGBA per pixel into the caller's buffer.
//
// Returns bytes written (= width*height*4) on success, 0 on failure.
//
// Caller does not own any RHI resource. Intended for ~30Hz editor previews, not
// per-frame gameplay use.
EDITOR_INTERFACE u64 RenderMaterialPreviewToRGBA(u32 preview_id, u64 device_handle,
                                                 u8* out_rgba, u64 buffer_size) {
    u32 idx = slot_to_index(preview_id);
    if (idx == UINT32_MAX) return 0;
    auto* renderer = previews[idx];
    if (!out_rgba || buffer_size == 0) return 0;

    auto* device = reinterpret_cast<rhi::RHIDeviceBase*>(device_handle);
    if (!device) {
        PRIMAL_LOG_STREAM(Error, "Engine") << "[MaterialPreviewAPI] RenderMaterialPreviewToRGBA: null device_handle\n";
        return 0;
    }

    const u32 w = renderer->GetWidth();
    const u32 h = renderer->GetHeight();
    const u64 expected = static_cast<u64>(w) * h * 4;
    if (buffer_size < expected) {
        PRIMAL_LOG_STREAM(Error, "Engine") << "[MaterialPreviewAPI] RenderMaterialPreviewToRGBA: buffer too small ("
                  << buffer_size << " < " << expected << ")\n";
        return 0;
    }

    // 1) Render preview into color_target on a Graphics cmd buffer.
    auto cmd_handle = device->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
    auto* cmd = rhi::GetCommandBuffer(cmd_handle);
    if (!cmd) {
        PRIMAL_LOG_STREAM(Error, "Engine") << "[MaterialPreviewAPI] RenderMaterialPreviewToRGBA: CreateCommandBuffer failed\n";
        return 0;
    }
    cmd->Begin();
    // Rotate through the triple-buffered view/scene constant buffers.
    static std::atomic<u32> s_frame_idx{0};
    u32 fi = s_frame_idx.fetch_add(1) % rhi::MAX_FRAMES_IN_FLIGHT;
    rhi::ResourceHandle color_tex = renderer->Render(cmd, fi, 1.0f / 30.0f);
    if (color_tex == rhi::handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Engine") << "[MaterialPreviewAPI] RenderMaterialPreviewToRGBA: Render failed\n";
        return 0;
    }
    cmd->End();

    rhi::QueueSubmitInfo submit{};
    submit.cmdBuffer = cmd_handle;
    device->Submit(submit);
    cmd->WaitForCompletion();

    // 2) Copy color_target → readback buffer on a Transfer cmd buffer.
    rhi::BufferDesc bufDesc{
        expected,
        rhi::BufferType::Unknown,
        rhi::GPUMemoryUsage::Readback,
        rhi::GPUMemoryUsage::Readback,
        0,
    };
    auto readback = device->CreateBuffer(bufDesc);
    if (readback == rhi::handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Engine") << "[MaterialPreviewAPI] RenderMaterialPreviewToRGBA: CreateBuffer failed\n";
        return 0;
    }

    auto copy_handle = device->CreateCommandBuffer(rhi::CommandQueueType::Transfer);
    auto* copy_cmd = rhi::GetCommandBuffer(copy_handle);
    if (!copy_cmd) {
        device->DestroyBuffer(readback);
        PRIMAL_LOG_STREAM(Error, "Engine") << "[MaterialPreviewAPI] RenderMaterialPreviewToRGBA: copy cmd buffer failed\n";
        return 0;
    }
    copy_cmd->Begin();
    rhi::BufferTextureCopyRegion region{
        0, 0, 0,
        { 0, 0, 1 },
        rhi::Offset3D{0, 0, 0},
        rhi::Extent3D{w, h, 1u},
    };
    copy_cmd->CopyTextureToBuffer(color_tex, readback, &region, 1);
    copy_cmd->End();

    rhi::QueueSubmitInfo copy_submit{};
    copy_submit.cmdBuffer = copy_handle;
    device->Submit(copy_submit);
    copy_cmd->WaitForCompletion();

    // 3) Map readback buffer, swap BGRA→RGBA per pixel, copy to caller.
    auto* buf_res = rhi::ResourceManager::Instance().GetResource(readback);
    u64 bytes_written = 0;
    if (buf_res) {
        void* mapped = buf_res->Map(0, expected);
        if (mapped) {
            const u8* src = static_cast<const u8*>(mapped);
            for (u64 i = 0; i < expected; i += 4) {
                // Metal BGRA8_UNorm → Avalonia WriteableBitmap Rgba8888
                out_rgba[i + 0] = src[i + 2];  // R
                out_rgba[i + 1] = src[i + 1];  // G
                out_rgba[i + 2] = src[i + 0];  // B
                out_rgba[i + 3] = src[i + 3];  // A
            }
            bytes_written = expected;
            buf_res->Unmap();
        } else {
            PRIMAL_LOG_STREAM(Error, "Engine") << "[MaterialPreviewAPI] RenderMaterialPreviewToRGBA: Map returned null\n";
        }
    } else {
        PRIMAL_LOG_STREAM(Error, "Engine") << "[MaterialPreviewAPI] RenderMaterialPreviewToRGBA: GetResource returned null\n";
    }

    device->DestroyBuffer(readback);
    return bytes_written;
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
