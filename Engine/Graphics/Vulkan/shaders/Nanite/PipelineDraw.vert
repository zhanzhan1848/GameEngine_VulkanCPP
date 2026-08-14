#version 450 core

// PipelineDraw.vert — GPU-driven meshlet vertex shader for pipeline model data.
// Diagnostic version: outputs the global vertex index as a color so we can
// verify the meshlet triangle assembly chain:
//   compact_cluster_ids → cluster_map → meshlet → meshlet_triangles → meshlet_vertices → position

layout(set = 0, binding = 0) uniform ConstantsBlock {
    mat4  view_matrix;
    mat4  proj_matrix;
    mat4  world_matrix;
    uint  view_width;
    uint  view_height;
    uint  meshlet_count;
    uint  padding;
    mat4  prev_view_matrix;
    mat4  prev_proj_matrix;
    uint  has_prev_frame;
    uint  debug_mode;
    uint  pad2_1;
    uint  pad2_2;
} dc;

struct Meshlet {
    uvec4 header;   // vertex_offset, triangle_offset, vertex_count, triangle_count
    vec4  cone0;
    vec4  cone1;
    vec4  cone2;
};

struct InstanceData {
    mat4  world_matrix;          // offset   0 (64 bytes)
    mat4  inverse_world_matrix;  // offset  64 (64 bytes)
    uvec4 meta0;                 // offset 128 (16 bytes)
    uvec4 meta1;                 // offset 144 (16 bytes)
    float bounds_center_x;       // offset 160
    float bounds_center_y;       // offset 164
    float bounds_center_z;       // offset 168
    float _pad1;                 // offset 172
    float bounds_radius;         // offset 176
    uint  _pad2;                 // offset 180
    uint  _pad3;                 // offset 184
    uint  _pad4;                 // offset 188
};                               // total: 192 bytes

layout(set = 0, binding = 1) readonly buffer MeshletBuffer      { Meshlet      mls[]; } mb;
layout(set = 0, binding = 2) readonly buffer MeshletVertsBuffer { uint         mvs[]; } mvb;
layout(set = 0, binding = 3) readonly buffer MeshletTrisBuffer  { uint         mts[]; } mtb;
layout(set = 0, binding = 4) readonly buffer PositionBuffer     { float        pos[]; } pb;
layout(set = 0, binding = 5) readonly buffer ClusterIdsBuffer   { uint         cids[]; } cib;
layout(set = 0, binding = 6) readonly buffer ClusterMapBuffer   { uvec4        cmap[]; } cmb;
layout(set = 0, binding = 7) readonly buffer InstanceDataBuffer { InstanceData insts[]; } idb;
layout(set = 0, binding = 8) readonly buffer ElementBuffer      { uint         elems[]; } eb;

layout(location = 0) out VSOut {
    vec2  uv;
    vec3  normal;
    vec3  tangent;
    vec3  bitangent;
    flat uint material_id;
    flat uint meshlet_id;
    flat uint triangle_id;
    flat uint mesh_id;
    vec4  current_clip;
    vec4  previous_clip;
    vec3  object_normal;
} OUT;

vec3 unpack_normal(uint packed, uint color_t_sign) {
    float hi = float((packed >> 16u) & 0xFFFFu);
    float lo = float(packed & 0xFFFFu);
    float inv = 2.0 / 65535.0;
    vec2 f = vec2(hi * inv - 1.0, lo * inv - 1.0);
    float d = dot(f, f);
    if (d > 1.0) return vec3(0.0, 0.0, 1.0);
    float z = sqrt(max(0.0, 1.0 - d));
    uint signs = (color_t_sign >> 24u) & 0xFFu;
    float nSign = float(signs & 0x02u) - 1.0;
    return vec3(f.x, f.y, z * nSign);
}

void main() {
    uint vid = gl_VertexIndex;
    uint instance_id = gl_InstanceIndex;

    uint global_cluster_id = cib.cids[instance_id];
    uvec4 cmap = cmb.cmap[global_cluster_id];
    uint global_meshlet_index = cmap.x;
    uint instance_index = cmap.y;
    uint material_id = cmap.z;

    InstanceData inst = idb.insts[instance_index];
    Meshlet meshlet = mb.mls[global_meshlet_index];

    uint triangle_count = meshlet.header.w;
    uint actual_vertex_count = triangle_count * 3u;

    if (vid >= actual_vertex_count) {
        OUT.uv = vec2(0.0);
        OUT.normal = vec3(0.0, 0.0, 1.0);
        OUT.tangent = vec3(1.0, 0.0, 0.0);
        OUT.bitangent = vec3(0.0, 1.0, 0.0);
        OUT.material_id = material_id;
        OUT.meshlet_id = global_meshlet_index;
        OUT.triangle_id = 0u;
        OUT.mesh_id = instance_index;
        OUT.current_clip = vec4(0.0);
        OUT.previous_clip = vec4(0.0);
        OUT.object_normal = vec3(0.0, 1.0, 0.0);
        gl_Position = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // Triangle index: now stored as u32 array (one index per uint).
    uint tri_offset = meshlet.header.y;
    uint local_vertex_idx = mtb.mts[tri_offset + vid];

    uint vert_offset = meshlet.header.x;
    uint vert_idx = mvb.mvs[vert_offset + local_vertex_idx];

    vec3 p = vec3(
        pb.pos[vert_idx * 3u + 0u],
        pb.pos[vert_idx * 3u + 1u],
        pb.pos[vert_idx * 3u + 2u]
    );

    // Element (24 bytes = 6 u32s per vertex).
    uint elem_base = vert_idx * 6u;
    uint color_t_sign = eb.elems[elem_base + 0u];
    uint packed_normal = eb.elems[elem_base + 1u];
    uint packed_tangent = eb.elems[elem_base + 2u];
    vec2 uv = vec2(uintBitsToFloat(eb.elems[elem_base + 4u]),
                   uintBitsToFloat(eb.elems[elem_base + 5u]));

    vec3 normal = unpack_normal(packed_normal, color_t_sign);
    vec3 tangent = unpack_normal(packed_tangent, color_t_sign);
    float tangent_sign = ((color_t_sign & 0xFF000000u) != 0u) ? -1.0 : 1.0;
    uv.y = 1.0 - uv.y;

    vec4 world_pos = inst.world_matrix * vec4(p, 1.0);
    vec4 view_pos = dc.view_matrix * world_pos;
    vec4 clip_pos = dc.proj_matrix * view_pos;

    mat3 normal_matrix = mat3(
        inst.world_matrix[0].xyz,
        inst.world_matrix[1].xyz,
        inst.world_matrix[2].xyz
    );

    OUT.object_normal = normal;

    vec3 world_normal = normalize(normal_matrix * normal);
    vec3 world_tangent = normalize(normal_matrix * tangent);
    if (abs(dot(world_tangent, world_normal)) > 0.999) {
        vec3 up = (abs(world_normal.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        world_tangent = normalize(cross(up, world_normal));
    } else {
        world_tangent = normalize(world_tangent - dot(world_tangent, world_normal) * world_normal);
    }
    vec3 world_bitangent = cross(world_normal, world_tangent) * tangent_sign;

    OUT.uv = uv;
    OUT.normal = world_normal;
    OUT.tangent = world_tangent;
    OUT.bitangent = world_bitangent;
    OUT.material_id = material_id;
    OUT.meshlet_id = global_meshlet_index;
    OUT.triangle_id = vid / 3u;
    OUT.mesh_id = instance_index;
    OUT.current_clip = clip_pos;
    OUT.previous_clip = clip_pos;

    gl_Position = clip_pos;
}
