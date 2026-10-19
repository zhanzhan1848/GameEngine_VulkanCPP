#pragma once

// Lightmap C ABI（2026-10-19 lightmap 计划，M4）。
//
// 契约：导出只增不改；LightmapBakeSettings 布局由
// Engine/Graphics/Baking/LightmapBaker.h 的 static_assert 锁死（280B），
// C# 侧镜像必须逐字段对齐（见 PrimalEditor NativeEngineBridge）。
// 全部同步（GIBakeStaticProbes 先例）；进度回调可能来自 worker 线程
// ——宿主自行 marshal 到 UI 线程。
//
// 失败语义：LightmapBake 返回 0 且 out_status_json 填
// {"ok":false,"error":"..."}；成功返回 1 且填
// {"ok":true,"path":"...","entries":[{"name":"...","atlas_index":0,
//  "scale_bias":[sx,sy,bx,by]},...]}。

#include "Common.h"

// 与 primal::graphics::baking::LightmapBakeSettings 逐字节同构（280B，
// static_assert 在 LightmapBaker.h；此处 C 镜像供宿主直接构造）。
struct LightmapBakeSettingsC {
    unsigned int atlas_size;
    unsigned int samples_per_texel;
    unsigned int bounces;
    float texel_density;
    int denoise;
    unsigned int dilation_px;
    char output_dir[256];
};

// phase: 0=collect 1=rasterize 2=trace 3=filter 4=dilate 5=encode
typedef void (*LightmapProgressCallbackC)(void* user, unsigned int phase,
                                          unsigned int done, unsigned int total);

EDITOR_INTERFACE unsigned int LightmapBake(const struct LightmapBakeSettingsC* settings,
                                           LightmapProgressCallbackC progress,
                                           void* progress_user,
                                           char* out_status_json,
                                           unsigned int status_cap);

EDITOR_INTERFACE unsigned int PipelineSetLightmapPage(const char* path);

EDITOR_INTERFACE void PipelineClearLightmap();
