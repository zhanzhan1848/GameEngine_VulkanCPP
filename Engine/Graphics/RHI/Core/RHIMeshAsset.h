#pragma once
#include "CommonHeaders.h"
#include "RHITypes.h"
// IWYU：本头直接使用 utl::vector——此前依赖其他 TU 的传递包含，
// EngineDLL/RenderPipelineAPI.cpp 的包含顺序触发编译失败。
#include "Utilities/Vector.h"

namespace primal::graphics::rhi
{
    struct RHIMeshlet
    {
        u32 vertex_offset;
        u32 triangle_offset;
        u32 vertex_count;
        u32 triangle_count;
        f32 cone_apex[3];
        f32 cone_axis[3];
        f32 cone_cutoff;
        f32 center[3];
        f32 radius;
        u32 padding;
    };

    struct RHISDFData
    {
        u32 resolution[3];
        f32 bounds_min[3];
        f32 bounds_max[3];
        utl::vector<u16> data;
        utl::vector<u8> voxels;
        utl::vector<u16> vector_field;

        // T4.3: 1 = data 是有符号距离（pack_float<16>(dist + max_dim,
        // 0, 2*max_dim)，fast winding number 判内）；0 = 旧 unsigned
        // dist = u16/65535 * max_dim。由 blob elements_type 的
        // sdf_signed_flag (0x40) 位驱动；上传走 R16_UNorm，shader 按
        // scale/offset 解码（DDGI detail-SDF 精化）。
        u8 signed_encoding{ 0 };
    };

    // T4.2 离线 card bake：一张烘焙卡片（mesh 局部空间）。只携带
    // SurfaceCacheCard 的消费字段（center/extent/axis_direction/resolution）；
    // atlas 摆放（atlas_offset_*）仍是运行时 RebuildCardAllocation 的决定。
    struct RHICardBakeCard
    {
        f32 center[3];
        u32 axis_direction;   // 低字节 = axis (0=X,1=Y,2=Z)，次字节 = dir (0/1)——SurfaceCacheCard ABI
        f32 extent[3];        // 半尺寸
        u32 resolution;       // 烘焙时紧致投影 × 64 texels/unit，钳 [16,256]；运行时按页重取整
    };
    static_assert(sizeof(RHICardBakeCard) == 32, "RHICardBakeCard wire size drift (blob 'CDBK' section, 32 B/card)");

    // 暴露体素面 surfel（未来探针播种用，当前无运行时消费者）。
    struct RHICardBakeSurfel
    {
        f32 pos[3];           // 暴露面中心（mesh 局部空间）
        f32 area;             // 面积（单位²）
        f32 normal[3];        // 单位外法线
        u32 pad;
    };
    static_assert(sizeof(RHICardBakeSurfel) == 32, "RHICardBakeSurfel wire size drift (blob 'CDBK' section, 32 B/surfel)");

    struct RHICardBakeData
    {
        utl::vector<RHICardBakeCard> cards;
        utl::vector<RHICardBakeSurfel> surfels;

        // 空 cards = 该 mesh 无烘焙数据（旧 blob / 退化网格），运行时走 AABB fallback。
        [[nodiscard]] bool HasCards() const { return !cards.empty(); }
    };

    struct RHIMeshAsset
    {
        u32 lod_id;
        u32 material_idx;
        f32 lod_threshold;
        
        // Raw buffers
        utl::vector<u8> position_buffer;
        utl::vector<u8> element_buffer;
        utl::vector<u8> index_buffer;
        u32 index_size; // 2 or 4
        u32 num_vertices;
        u32 num_indices;
        u32 elements_type;

        // Meshlets
        utl::vector<RHIMeshlet> meshlets;
        utl::vector<u32> meshlet_vertices;
        utl::vector<u8> meshlet_triangles;

        // SDF
        RHISDFData sdf;

        // A9 Lightmap UV sidecar (A9 地基段): raw f32 (u, v) pairs, 8 bytes
        // per vertex, uv_count = size / 8. Carried from the blob's UVLM
        // section (elements_type lightmap_uv_flag); empty for meshes without
        // lightmap UVs. No GPU consumer yet — future lightmap sampling binds
        // this as its own vertex-pulling buffer (per-backend, zero impact on
        // the existing 20B element format).
        utl::vector<u8> lightmap_uv_buffer;

        // T4.2 离线 card bake（blob "CDBK" 尾段，elements_type 0x20 旗标）。
        // 空 = 无烘焙数据，SurfaceCache CardGenerator 走 AABB fallback。
        RHICardBakeData card_bake;
    };
}
