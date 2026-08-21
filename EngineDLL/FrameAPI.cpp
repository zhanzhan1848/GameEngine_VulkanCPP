// FrameAPI.cpp - Implements RenderFrame: drives one frame of the render surface.
#include "Common.h"
#include "CommonHeaders.h"
#include "EngineAPIInternal.h"
#include "../Graphics/Renderer.h"
#include "FrameAPI.h"

#include <cstdio>

using namespace primal;

// FrameAPI.h 已在 extern "C" 块内声明本函数。MSVC 对"块内声明 + 带(dllexport)
// 的定义"组合误判 C2375（不同的链接）——RenderFrame 是 EngineDLL 中唯一在头
// 文件预声明的导出函数，其余 EDITOR_INTERFACE 函数均无前置声明故不触发。
// MSVC 下改用与声明完全一致的裸 extern "C"；符号导出由 CMake 的
// CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS 兜底。Apple 保留 visibility 属性。
#if defined(_MSC_VER)
extern "C" u32 RenderFrame(const RenderFrameParams* params)
#else
EDITOR_INTERFACE u32 RenderFrame(const RenderFrameParams* params)
#endif
{
    if (!params) {
        std::fprintf(stderr, "[RenderFrame] null params\n");
        return 0;
    }

    if (params->surface_id >= engine_dll::GetSurfaceCount()) {
        std::fprintf(stderr, "[RenderFrame] invalid surface_id %u (count=%u)\n",
                     params->surface_id, engine_dll::GetSurfaceCount());
        return 0;
    }

    auto* rs = engine_dll::GetSurface(params->surface_id);
    if (!rs || !rs->surface.is_valid()) {
        std::fprintf(stderr, "[RenderFrame] surface %u not valid\n", params->surface_id);
        return 0;
    }

    graphics::frame_info info{};
    info.camera = graphics::camera_id{ params->camera_id };
    info.light_set_key = params->light_set_key;
    info.render_item_count = params->render_item_count;
    // frame_info has non-const pointers by design (legacy). Cast carefully.
    info.render_item_ids = const_cast<id::id_type*>(
        reinterpret_cast<const id::id_type*>(params->render_item_ids));
    info.thresholds = const_cast<f32*>(params->thresholds);
    info.average_frame_time = params->average_frame_time > 0.f ? params->average_frame_time : 16.7f;
    info.last_frame_time = params->last_frame_time > 0.f ? params->last_frame_time : 16.7f;

    rs->surface.render(info);
    return 1;
}
