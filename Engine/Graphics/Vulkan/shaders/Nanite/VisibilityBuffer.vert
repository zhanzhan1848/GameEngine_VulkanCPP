#version 450 core

// T4.6.5 part 22.1 — meshlet-aware vertex pulling.
// Mirrors Engine/Graphics/Dawn/shaders/Nanite/GPUDrivenDraw.wgsl:211-328 (Stage3 vertex shader).
// Source fn is `main`; -e renames the SPIR-V entry point to `visibility_vertex_shader`.
//
// Bindings (8 total):
//   0: UBO  DrawConstants
//   1: SSBO Meshlet[]               meshlets (64B each: 4×uvec4)
//   2: SSBO uint[]                  meshlet_vertices
//   3: SSBO uint[]                  meshlet_triangles (byte-packed u8 indices)
//   4: SSBO float[]                 positions (3 floats per vertex, tightly packed)
//   5: SSBO uint[]                  compact_cluster_ids (visible cluster list)
//   6: SSBO uvec4[]                 cluster_map (global_meshlet_index, instance_index, material_id, _pad)
//   7: SSBO InstanceData[]          instance_data (192B each)

// 64B Meshlet struct — matches C++ RHIMeshlet.
struct Meshlet {
    uvec4 header;    // .x=vertex_offset, .y=triangle_offset, .z=vertex_count, .w=triangle_count
    vec4  cone0;     // cone_apex[3] + cone_axis[0]
    vec4  cone1;     // cone_axis[1..2] + cone_cutoff + center[0]
    vec4  cone2;     // center[1..2] + radius + padding
};

// 192B InstanceData — matches C++ graphics::InstanceData. Note math::v3
// (simd::float3 on macOS) has sizeof=16, so bounds_radius is at offset 176,
// NOT 172 (despite the C++ comment saying 172).
struct InstanceData {
    mat4 world_matrix;            // offset   0
    mat4 inverse_world_matrix;    // offset  64
    uvec4 meta0;                  // offset 128: geometry_id, material_id, cluster_start, cluster_count
    uvec4 meta1;                  // offset 144: cluster_map_base, _pad, _pad, _pad
    float bounds_center_x;        // offset 160
    float bounds_center_y;        // offset 164
    float bounds_center_z;        // offset 168
    float _pad_after_center;      // offset 172 (compensates simd::float3 sizeof=16)
    float bounds_radius;          // offset 176
    uvec3 bounds_padding;         // offset 180..188 (3 u32s)
    float bounds_padding2;        // offset 188 (final entry, struct total 192B)
};

struct DrawConstants {
    mat4 view_matrix;
    mat4 proj_matrix;
    mat4 world_matrix;
    uint view_width;
    uint view_height;
    uint meshlet_count;
    uint _pad;
};

layout(set = 0, binding = 0) uniform ConstantsBlock {
    DrawConstants constants;
} dc;

layout(set = 0, binding = 1) readonly buffer MeshletBuffer {
    Meshlet meshlets[];
} mb;

layout(set = 0, binding = 2) readonly buffer MeshletVertsBuffer {
    uint meshlet_vertices[];
} mvb;

layout(set = 0, binding = 3) readonly buffer MeshletTrisBuffer {
    uint meshlet_triangles[];
} mtb;

layout(set = 0, binding = 4) readonly buffer PositionBuffer {
    float positions[];
} pb;

layout(set = 0, binding = 5) readonly buffer CompactClusterIdsBuffer {
    uint compact_cluster_ids[];
} ccib;

layout(set = 0, binding = 6) readonly buffer ClusterMapBuffer {
    uvec4 cluster_map[];
} cmb;

layout(set = 0, binding = 7) readonly buffer InstanceDataBuffer {
    InstanceData instance_data[];
} idb;

layout(location = 0) out VSOut {
    vec4 position;
    float depth;
    flat uint meshlet_id;
    flat uint primitive_id;
} OUT;

void main() {
    uint vid = gl_VertexIndex;
    uint instance_id = gl_InstanceIndex;

    // Step 1: visible cluster index → global cluster ID.
    uint global_cluster_id = ccib.compact_cluster_ids[instance_id];

    // Step 2: cluster ID → meshlet + instance.
    uvec4 cmap = cmb.cluster_map[global_cluster_id];
    uint global_meshlet_index = cmap.x;
    uint instance_index = cmap.y;
    // uint material_id = cmap.z;  // unused in visibility pass

    InstanceData inst = idb.instance_data[instance_index];
    Meshlet meshlet = mb.meshlets[global_meshlet_index];

    uint triangle_count = meshlet.header.w;
    uint actual_vertex_count = triangle_count * 3u;

    // Step 3: vertex pulling — clip degenerate padding vertices.
    if (vid >= actual_vertex_count) {
        OUT.position = vec4(0.0, 0.0, 0.0, 0.0);
        OUT.depth = 0.0;
        OUT.meshlet_id = 0u;
        OUT.primitive_id = 0u;
        gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Step 4: local vertex index from byte-packed u8 triangle indices.
    uint tri_offset = meshlet.header.y;
    uint base_byte = tri_offset + vid;
    uint word_idx = base_byte / 4u;
    uint byte_shift = (base_byte % 4u) * 8u;
    uint local_vertex_idx = (mtb.meshlet_triangles[word_idx] >> byte_shift) & 0xFFu;

    // Step 5: index into meshlet_vertices to get global position index.
    uint vert_offset = meshlet.header.x;
    uint vert_idx = mvb.meshlet_vertices[vert_offset + local_vertex_idx];

    // Step 6: pull position (tightly packed float[3]).
    vec3 pos = vec3(
        pb.positions[vert_idx * 3u + 0u],
        pb.positions[vert_idx * 3u + 1u],
        pb.positions[vert_idx * 3u + 2u]
    );

    // Step 7: transform to clip space via instance world matrix.
    vec4 world_pos = inst.world_matrix * vec4(pos, 1.0);
    vec4 view_pos = dc.constants.view_matrix * world_pos;
    vec4 clip_pos = dc.constants.proj_matrix * view_pos;

    OUT.position = clip_pos;
    OUT.depth = clip_pos.z / clip_pos.w;
    OUT.meshlet_id = global_meshlet_index;
    OUT.primitive_id = vid / 3u;

    gl_Position = clip_pos;
}
