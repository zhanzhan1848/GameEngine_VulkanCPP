#version 450 core

#if defined(SC_CAPTURE_GEOMETRY_AUDIT_VARIANT) && !defined(SC_CAPTURE_SOURCE_AUDIT_VARIANT)
#error "SC_CAPTURE_GEOMETRY_AUDIT_VARIANT requires SC_CAPTURE_SOURCE_AUDIT_VARIANT"
#endif

// CardCapture.vert — T8.12 card-view capture vertex shader.
// Draws scene geometry from one Surface Cache card's orthographic view into
// the card's atlas pages (MRT: albedo / oct-normal / axis-depth).
//
// Decodes instances exactly like PipelineDraw.vert (working GBuffer path):
//   visible-cluster list (binding 5, CPU-built per face) → cmap → meshlet /
//   instance / material. The ONLY deltas vs PipelineDraw are:
//   - binding 0 is a card Params UBO (ortho view/proj + card frame) instead
//     of the main camera ConstantsBlock;
//   - outputs world_pos so the fragment stage can emit the card-axis depth
//     (worldToCardUV semantics: dot(local, fwd), positive into the view).

layout(set = 0, binding = 0) uniform CardParams {
    mat4  view;        // card ortho view (world → card view space)
    mat4  proj;        // card ortho projection
    vec4  center;      // card center, xyz
    vec4  fwd;         // card forward axis, xyz (unit, sign folded per dir)
    vec4  misc;        // x = depth_near, y = depth_far
} cb;

struct Meshlet {
    uvec4 header;   // vertex_offset, triangle_offset, vertex_count, triangle_count
    vec4  cone0;
    vec4  cone1;
    vec4  cone2;
};

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
    uint  _pad2;
    uint  _pad3;
    uint  _pad4;
};

layout(set = 0, binding = 1) readonly buffer MeshletBuffer      { Meshlet mls[]; } mb;
layout(set = 0, binding = 2) readonly buffer MeshletVertsBuffer { uint    mvs[]; } mvb;
layout(set = 0, binding = 3) readonly buffer MeshletTrisBuffer  { uint    mts[]; } mtb;
layout(set = 0, binding = 4) readonly buffer PositionBuffer     { float   pos[]; } pb;
layout(set = 0, binding = 5) readonly buffer VisibleClusters    { uint    cids[]; } cib;
layout(set = 0, binding = 6) readonly buffer ClusterMapBuffer   { uvec4   cmap[]; } cmb;
layout(set = 0, binding = 7) readonly buffer InstanceDataBuffer { InstanceData insts[]; } idb;
layout(set = 0, binding = 8) readonly buffer ElementBuffer      { uint    elems[]; } eb;

layout(location = 0) out VSOut {
    vec2 uv;
    vec3 normal;
    flat uint material_id;
    vec3 world_pos;
#ifdef SC_CAPTURE_SOURCE_AUDIT_VARIANT
    // Actual source of the rasterized meshlet triangle.  These are carried
    // flat to the fragment tag so the audit can replay the resident meshlet
    // bytes without inferring a triangle from the card or ray.
    flat uint actual_cluster_id;
    flat uint actual_triangle_id;
#ifdef SC_CAPTURE_GEOMETRY_AUDIT_VARIANT
    // Capture-time primitive witness.  The three values are flat so every
    // fragment of the primitive carries the same resident triangle source.
    flat vec3 geometry_triangle0_world;
    flat vec3 geometry_triangle1_world;
    flat vec3 geometry_triangle2_world;
    flat uint geometry_instance_index;
#endif
#endif
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

#ifdef SC_CAPTURE_GEOMETRY_AUDIT_VARIANT
// Fetch the three packed-index corners using the same meshlet triangle byte
// stream, global position buffer, and instance world transform as production.
// This helper is diagnostic-only and does not replace the production fetch.
vec3 fetch_audit_world_vertex(Meshlet meshlet, InstanceData inst,
                               uint local_triangle_id, uint corner) {
    uint byte_index = meshlet.header.y + local_triangle_id * 3u + corner;
    uint local_vertex_idx =
        (mtb.mts[byte_index >> 2u] >> ((byte_index & 3u) * 8u)) & 0xFFu;
    uint vert_idx = mvb.mvs[meshlet.header.x + local_vertex_idx];
    vec3 p = vec3(
        pb.pos[vert_idx * 3u + 0u],
        pb.pos[vert_idx * 3u + 1u],
        pb.pos[vert_idx * 3u + 2u]
    );
    return (inst.world_matrix * vec4(p, 1.0)).xyz;
}
#endif

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
        OUT.normal = vec3(0.0, 1.0, 0.0);
        OUT.material_id = material_id;
        OUT.world_pos = vec3(0.0);
#ifdef SC_CAPTURE_SOURCE_AUDIT_VARIANT
        OUT.actual_cluster_id = global_cluster_id;
        OUT.actual_triangle_id = 0xFFFFFFFFu;
#ifdef SC_CAPTURE_GEOMETRY_AUDIT_VARIANT
        OUT.geometry_triangle0_world = vec3(0.0);
        OUT.geometry_triangle1_world = vec3(0.0);
        OUT.geometry_triangle2_world = vec3(0.0);
        OUT.geometry_instance_index = 0xFFFFFFFFu;
#endif
#endif
        gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    uint tri_offset = meshlet.header.y;
    // CPU uploads one byte per local index; SSBO uint words contain four.
    uint byte_index = tri_offset + vid;
    uint local_vertex_idx = (mtb.mts[byte_index >> 2u] >> ((byte_index & 3u) * 8u)) & 0xFFu;
    uint vert_offset = meshlet.header.x;
    uint vert_idx = mvb.mvs[vert_offset + local_vertex_idx];

    vec3 p = vec3(
        pb.pos[vert_idx * 3u + 0u],
        pb.pos[vert_idx * 3u + 1u],
        pb.pos[vert_idx * 3u + 2u]
    );

    uint elem_base = vert_idx * 6u;
    uint color_t_sign = eb.elems[elem_base + 0u];
    uint packed_normal = eb.elems[elem_base + 1u];
    vec2 uv = vec2(uintBitsToFloat(eb.elems[elem_base + 4u]),
                   uintBitsToFloat(eb.elems[elem_base + 5u]));

    vec3 normal = unpack_normal(packed_normal, color_t_sign);

    vec4 world_pos = inst.world_matrix * vec4(p, 1.0);
    mat3 normal_matrix = mat3(
        inst.world_matrix[0].xyz,
        inst.world_matrix[1].xyz,
        inst.world_matrix[2].xyz
    );
    vec3 world_normal = normalize(normal_matrix * normal);

    uv.y = 1.0 - uv.y;
    OUT.uv = uv;
    OUT.normal = world_normal;
    OUT.material_id = material_id;
    OUT.world_pos = world_pos.xyz;
#ifdef SC_CAPTURE_SOURCE_AUDIT_VARIANT
    OUT.actual_cluster_id = global_cluster_id;
    OUT.actual_triangle_id = vid / 3u;
#ifdef SC_CAPTURE_GEOMETRY_AUDIT_VARIANT
    uint local_triangle_id = vid / 3u;
    OUT.geometry_triangle0_world = fetch_audit_world_vertex(meshlet, inst,
                                                            local_triangle_id, 0u);
    OUT.geometry_triangle1_world = fetch_audit_world_vertex(meshlet, inst,
                                                            local_triangle_id, 1u);
    OUT.geometry_triangle2_world = fetch_audit_world_vertex(meshlet, inst,
                                                            local_triangle_id, 2u);
    OUT.geometry_instance_index = instance_index;
#endif
#endif

    gl_Position = cb.proj * (cb.view * world_pos);
    // Card views use Metal's Y-up clip convention; Vulkan uses a positive
    // viewport height. Match worldToCardUV's increasing local-v atlas rows.
    gl_Position.y = -gl_Position.y;
}
