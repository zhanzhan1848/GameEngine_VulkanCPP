/**
 * @file SurfaceCacheAtlasInit.metal
 * @brief Metal port of SurfaceCacheAtlasInit.comp (T8.13 Vulkan↔Metal sync).
 *
 * Populates card atlas regions with default material data so LightEval has
 * valid albedo (alpha=1) to process. Without this the Metal heap-allocated
 * atlas pages start as garbage and the SC↔probe feedback loop latches it.
 *
 * First-frame-only dispatch (atlas_seeded_), 1D over total_texels — same
 * flattened texel mapping as LightEval. Metal slots follow the module
 * convention: write targets [[texture(0..3)]], params [[buffer(1)]],
 * card_dispatch [[buffer(2)]].
 */

#include <metal_stdlib>
using namespace metal;
#include "SurfaceCacheData.metal"

struct AtlasInitParams {
    FlattenedLightingParams params;
};

kernel void surfaceCacheAtlasInit(
    uint global_tid [[thread_position_in_grid]],

    texture2d<float, access::write>  albedo_atlas   [[texture(0)]],
    texture2d<float, access::write>  normal_atlas   [[texture(1)]],
    texture2d<float, access::write>  depth_atlas    [[texture(2)]],
    texture2d<float, access::write>  emissive_atlas [[texture(3)]],

    constant AtlasInitParams& params [[buffer(1)]],
    constant CardDispatchInfo* cd    [[buffer(2)]])
{
    uint tid = global_tid;
    if (tid >= params.params.total_texels) return;

    // Binary search: find card whose [texel_offset, texel_offset+texel_count)
    // contains tid (same as LightEval).
    uint lo = 0u;
    uint hi = params.params.card_count;
    while (lo < hi) {
        uint mid = (lo + hi) >> 1u;
        if (cd[mid].texel_offset + cd[mid].texel_count <= tid)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo >= params.params.card_count) return;

    uint local_texel = tid - cd[lo].texel_offset;
    uint res = cd[lo].resolution;
    uint local_x = local_texel % res;
    uint local_y = local_texel / res;
    uint2 atlas_uv = uint2(cd[lo].atlas_offset_x + local_x,
                           cd[lo].atlas_offset_y + local_y);

    // [P0 有效性种子 2026-09-20，与 .comp 同步] 未捕获 texel 显式无效：
    // alpha=0（LightEval 门跳过）、depth=-1（查询契约 depth<0 拒绝）。
    // 旧种子 alpha=1/depth=0.5 伪造"卡平面前 0.5m 的灰色表面"——恰落在
    // 0.9 查询容差带内被当合法测量接受（均匀灰填充源之一）。
    albedo_atlas.write(float4(0.0f, 0.0f, 0.0f, 0.0f), atlas_uv);
    normal_atlas.write(float4(0.5f, 0.5f, 0.0f, 1.0f), atlas_uv);
    depth_atlas.write(float4(-1.0f, 0.0f, 0.0f, 1.0f), atlas_uv);
    emissive_atlas.write(float4(0.0f, 0.0f, 0.0f, 1.0f), atlas_uv);
}
