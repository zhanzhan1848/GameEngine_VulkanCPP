#version 450 core

// T4.6.5 part 17.3 — Vulkan port of EngineTest/shaders/VisibilityBuffer.metal
// Vertex stage. Source fn is `main` (glslangValidator convention); -e renames
// the SPIR-V entry point to `visibility_vertex_shader` so the C++ loader
// (which passes "visibility_vertex_shader" to CreateShader) finds it.
//
// Bindings (mirror Metal [[buffer(N)]]):
//   0: UBO  DrawConstants        constants
//   1: SSBO MeshletData[]        meshlets
//   2: SSBO uint[]               meshlet_vertices
//   3: SSBO uint[]               meshlet_triangles
//   4: SSBO vec3[]               positions

struct MeshletData {
    uvec4 header;
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
    MeshletData meshlets[];
} meshletData;

// T4.6.5 part 17.3: meshlet_vertices (binding 2) and meshlet_triangles (binding 3)
// are declared to match the Metal layout but not read in this stub. They make the
// pipeline's DSL match the binding declaration expected by future fully-wired
// visibility passes.
layout(set = 0, binding = 2) readonly buffer MeshletVerts {
    uint _meshlet_vertices[];
} _mv;
layout(set = 0, binding = 3) readonly buffer MeshletTris {
    uint _meshlet_triangles[];
} _mt;

layout(set = 0, binding = 4) readonly buffer PositionBuffer {
    vec3 positions[];
} pos;

layout(location = 0) out VSOut {
    vec4 position;
    float depth;
    flat uint meshlet_id;
    flat uint primitive_id;
} OUT;

void main() {
    uint vid = gl_VertexIndex;
    uint instance_id = gl_InstanceIndex;

    uint meshlet_id = instance_id;
    uint vertex_in_meshlet = vid / 3u;
    uint vertex_in_triangle = vid % 3u;

    uint primitive_idx = (vertex_in_meshlet / 3u) + (meshlet_id * 128u);
    if (primitive_idx >= (meshlet_id * 128u + 128u)) {
        primitive_idx = meshlet_id * 128u;
    }

    uint vertex_idx = vid;
    vec3 world_pos = pos.positions[vertex_idx];
    vec4 clip_pos = dc.constants.proj_matrix * dc.constants.view_matrix *
                    dc.constants.world_matrix * vec4(world_pos, 1.0);

    OUT.position = clip_pos;
    OUT.depth = clip_pos.z / clip_pos.w;
    OUT.meshlet_id = meshlet_id;
    OUT.primitive_id = primitive_idx;

    gl_Position = clip_pos;
}
