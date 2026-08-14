#version 450 core

// DebugDraw.vert — Minimal vertex shader that bypasses ALL meshlet logic.
// Draws raw triangles directly from the index buffer via vertex pulling.
// This isolates whether the position data itself is correct.
//
// Bindings:
//   0: UBO  (mat4 view + proj)
//   4: SSBO float[] positions (packed vec3, 12B/vertex)
//   5: SSBO uint[]  compact_cluster_ids (unused here but must match layout)
//   7: SSBO InstanceData[] instance_data

layout(set = 0, binding = 0) uniform ConstantsBlock {
    mat4 view_matrix;
    mat4 proj_matrix;
    mat4 world_matrix;
    uint view_width;
    uint view_height;
    uint meshlet_count;
    uint padding;
    mat4 prev_view_matrix;
    mat4 prev_proj_matrix;
    uint has_prev_frame;
    uint debug_mode;
    uint pad2_1;
    uint pad2_2;
} dc;

struct InstanceData {
    mat4  world_matrix;
    mat4  inverse_world_matrix;
    uvec4 meta0;
    uvec4 meta1;
    float bounds_center_x;
    float bounds_center_y;
    float bounds_center_z;
    float _pad1;
    float bounds_radius;
    uvec3 _pad2;
    float _pad3;
};

layout(set = 0, binding = 4) readonly buffer PositionBuffer { float pos[]; } pb;
layout(set = 0, binding = 7) readonly buffer InstanceDataBuffer { InstanceData insts[]; } idb;

layout(location = 0) out vec3 vWorldPos;

void main() {
    // Each instance draws one meshlet's worth (384 verts = 128 tris).
    // But for debug, we treat vid as a raw global vertex index within
    // a simple sequential draw. We use instance_index to pick instance.
    uint vid = gl_VertexIndex;
    uint instance_id = gl_InstanceIndex;

    // Use first instance's world matrix (all Sponza meshes are identity anyway).
    InstanceData inst = idb.insts[0];

    // Pull position directly by vertex index.
    // vid goes 0..383 per instance. Map it to a position.
    // For a true test, we just output vid mod 3 as a degenerate position
    // to see if the pipeline itself works.
    vec3 p = vec3(
        pb.pos[vid * 3u + 0u],
        pb.pos[vid * 3u + 1u],
        pb.pos[vid * 3u + 2u]
    );

    vec4 world_pos = inst.world_matrix * vec4(p, 1.0);
    vWorldPos = world_pos.xyz;
    gl_Position = dc.proj_matrix * (dc.view_matrix * world_pos);
}
