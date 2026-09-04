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
    };
}
