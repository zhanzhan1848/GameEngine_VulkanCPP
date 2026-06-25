// ShadowDepth.wgsl — Dawn port of Nanite/ShadowDepth.metal.
//
// Depth-only vertex shader for shadow map rasterization via vertex pulling.
// Reads the visible_clusters list (populated by ExecuteShadowCulling CPU-side
// or shadow_cluster_culling GPU-side), looks up geometry data, transforms
// positions into the light's clip space. No fragment shader — depth-only pass.
//
// Drawn via DrawIndirect with vertex_count = 126*3, instance_count = visible.
//
// Bindings (WebGPU single namespace):
//   0: uniform ShadowDepthUniforms — light VP + visible count
//   1: array<u32, read>            — visible_clusters
//   2: array<ClusterMap, read>     — cluster_map
//   3: array<InstanceData, read>   — instances
//   4: array<MeshletData, read>    — meshlets
//   5: array<u32, read>            — meshlet_vertex_indices
//   6: array<u32, read>            — meshlet_triangle_indices (packed u8 in u32)
//   7: array<f32, read>            — vertex_positions (packed_float3, 12 bytes/vertex)

struct InstanceData {
    world_matrix: mat4x4<f32>,
    inverse_world_matrix: mat4x4<f32>,
    meta0: vec4<u32>,
    meta1: vec4<u32>,
    bounds_center: vec3<f32>,
    bounds_radius: f32,
    _pad_br: vec4<u32>,
};

struct MeshletData {
    header: vec4<u32>,   // vertex_offset, triangle_offset, vertex_count, triangle_count
    cone0: vec4<f32>,
    cone1: vec4<f32>,
    cone2: vec4<f32>,
};

struct ClusterMap {
    data: vec4<u32>,   // globalMeshletIndex, instanceIndex, materialID, _pad
};

// Matches C++ ShadowDepthCB (sizeof == 80). visible_cluster_count + 3 pad u32s
// collapse into one vec4 to maintain 16-byte uniform alignment.
struct ShadowDepthUniforms {
    light_view_projection: mat4x4<f32>,
    counts: vec4<u32>,   // visible_cluster_count, _pad, _pad, _pad
};

@group(0) @binding(0) var<uniform> uniforms: ShadowDepthUniforms;
@group(0) @binding(1) var<storage, read> visible_clusters: array<u32>;
@group(0) @binding(2) var<storage, read> cluster_map: array<ClusterMap>;
@group(0) @binding(3) var<storage, read> instances: array<InstanceData>;
@group(0) @binding(4) var<storage, read> meshlets: array<MeshletData>;
@group(0) @binding(5) var<storage, read> meshlet_vertex_indices: array<u32>;
@group(0) @binding(6) var<storage, read> meshlet_triangle_indices: array<u32>;
@group(0) @binding(7) var<storage, read> vertex_positions: array<f32>;

struct VSOut {
    @builtin(position) position: vec4<f32>,
};

@vertex
fn shadow_depth_vs(
    @builtin(vertex_index) vertex_id: u32,
    @builtin(instance_index) instance_id: u32,
) -> VSOut {
    var out: VSOut;

    let triangle_index = vertex_id / 3u;
    let vertex_in_triangle = vertex_id % 3u;

    // instance_id indexes into visible_clusters[]. DrawIndirect already clamps
    // instance_count to the visible count, so no bounds check needed.
    let global_cluster_idx = visible_clusters[instance_id];

    let cmap = cluster_map[global_cluster_idx];
    let meshlet_idx = cmap.data.x;
    let inst_idx = cmap.data.y;

    let inst = instances[inst_idx];
    let meshlet = meshlets[meshlet_idx];

    let tri_count = meshlet.header.w;
    if (triangle_index >= tri_count) {
        // Out of bounds — push behind camera so it gets clipped.
        out.position = vec4<f32>(0.0, 0.0, 2.0, 1.0);
        return out;
    }

    // meshlet_triangle_indices is array<u32> but the C++ data is u8 per byte
    // (RHIMeshAsset.h: utl::vector<u8>). Extract bytes via shift/mask — same
    // pattern as GlobalSDFVoxelization.wgsl:155-161.
    let tri_offset = meshlet.header.y + triangle_index * 3u;
    let base_byte = tri_offset + vertex_in_triangle;
    let word_idx = base_byte / 4u;
    let byte_shift = (base_byte % 4u) * 8u;
    let local_vertex_idx = (meshlet_triangle_indices[word_idx] >> byte_shift) & 0xFFu;

    let vert_offset = meshlet.header.x;
    let global_vertex_idx = meshlet_vertex_indices[vert_offset + local_vertex_idx];

    // Vertex positions are packed_float3 (12 bytes/vertex) → array<f32> view.
    let lp = vec3<f32>(
        vertex_positions[global_vertex_idx * 3u],
        vertex_positions[global_vertex_idx * 3u + 1u],
        vertex_positions[global_vertex_idx * 3u + 2u]
    );

    let world_pos = inst.world_matrix * vec4<f32>(lp, 1.0);
    var clip = uniforms.light_view_projection * world_pos;

    // Depth bias: in WebGPU Z=[0,1] space, adding to Z makes depth larger
    // (= farther from light), giving front-facing surfaces a margin in the
    // shadow comparison. Matches metal:138.
    clip.z = clip.z + 0.001;

    out.position = clip;
    return out;
}
