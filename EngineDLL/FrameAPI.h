// FrameAPI.h - C ABI for driving a render frame from the Editor (C# P/Invoke).
#pragma once

#include <cstdint>

using u32 = uint32_t;
using u64 = uint64_t;
using f32 = float;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RenderFrameParams {
    u32        surface_id;          // from CreateRenderSurface
    u32        camera_id;           // from CreateCamera
    u64        light_set_key;       // from CreateLightSet (0 = empty)
    u32        render_item_count;
    const u64* render_item_ids;     // array of AddRenderItem ids
    const f32* thresholds;          // optional, may be NULL -> engine uses defaults
    f32        average_frame_time;
    f32        last_frame_time;
} RenderFrameParams;

// Returns 1 on success, 0 on failure.
u32 RenderFrame(const RenderFrameParams* params);

#ifdef __cplusplus
}
#endif
