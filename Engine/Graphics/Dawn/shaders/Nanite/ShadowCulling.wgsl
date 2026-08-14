// ShadowCulling.wgsl — Dawn port of Nanite/ShadowCulling.metal.
//
// GPU-driven shadow cluster culling. Culls instances and meshlets against the
// light frustum for cascaded shadow maps, then stream-compacts the visible
// clusters via atomic counter into indirect draw args.
//
// NOTE: The Dawn test bed currently does culling CPU-side (see
// GPUDrivenDrawPipeline::ExecuteShadowCulling). These kernels are created but
// not dispatched. They exist for future GPU-driven culling parity.
//
// Bindings (WebGPU single namespace):
//   0: array<InstanceData, read>        — instances
//   1: array<MeshletData, read>         — meshlets
//   2: array<ClusterMap, read>          — cluster_map
//   3: uniform ShadowCullUniforms       — frustum planes + VP + counts
//   4: atomic<u32> read_write           — visible_counter
//   5: array<u32, read_write>           — visible_clusters
//   6: array<u32, read_write>           — indirect_args (4 u32s)
//
// Entry points (each its own pipeline):
//   shadow_cluster_culling  (workgroup_size 64)
//   shadow_finalize_indirect (workgroup_size 64; only thread 0 does work)

// Matches C++ InstanceData (RenderSceneSnapshot.h, sizeof == 192).
// CRITICAL: simd::float3 in the C++ struct has sizeof == 16 (16-byte alignment),
// so bounds_center occupies offsets 160-175 with 4 bytes of padding at 172-175.
// bounds_radius is at offset 176, NOT 172. Verified via static_assert on 2026-06-23.
struct InstanceData {
    world_matrix: mat4x4<f32>,
    inverse_world_matrix: mat4x4<f32>,
    meta0: vec4<u32>,   // geometry_id, material_id, cluster_start, cluster_count
    meta1: vec4<u32>,   // cluster_map_base, _pad0, _pad1, _pad2
    bounds_center_x: f32,   // offset 160
    bounds_center_y: f32,   // offset 164
    bounds_center_z: f32,   // offset 168
    _pad_after_center: u32, // offset 172 — padding from simd::float3 16-byte size
    bounds_radius: f32,     // offset 176
    _pad_br0: u32,          // offset 180
    _pad_br1: u32,          // offset 184
    _pad_br2: u32,          // offset 188
};

// Matches C++ RHIMeshlet (sizeof == 64).
struct MeshletData {
    header: vec4<u32>,   // vertex_offset, triangle_offset, vertex_count, triangle_count
    cone0: vec4<f32>,    // cone_apex.xyz, cone_axis.x
    cone1: vec4<f32>,    // cone_axis.yz, cone_cutoff, center.x
    cone2: vec4<f32>,    // center.yz, radius, _pad
};

struct ClusterMap {
    data: vec4<u32>,   // globalMeshletIndex, instanceIndex, materialID, _pad
};

// Matches C++ ShadowCullUniforms (sizeof == 176). vec4-aligned to match std140
// layout (num_instances + cascade_index + 2 pad u32s collapse into one vec4).
struct ShadowCullUniforms {
    frustum_planes: array<vec4<f32>, 6>,
    light_view_projection: mat4x4<f32>,
    counts: vec4<u32>,   // num_instances, cascade_index, _pad, _pad
};

@group(0) @binding(0) var<storage, read> instances: array<InstanceData>;
@group(0) @binding(1) var<storage, read> meshlets: array<MeshletData>;
@group(0) @binding(2) var<storage, read> cluster_map: array<ClusterMap>;
@group(0) @binding(3) var<uniform> uniforms: ShadowCullUniforms;
@group(0) @binding(4) var<storage, read_write> visible_counter: atomic<u32>;
@group(0) @binding(5) var<storage, read_write> visible_clusters: array<u32>;
@group(0) @binding(6) var<storage, read_write> indirect_args: array<u32>;

fn sphere_vs_frustum(center: vec3<f32>, radius: f32) -> bool {
    for (var i: i32 = 0; i < 6; i = i + 1) {
        let d = dot(vec4<f32>(center, 1.0), uniforms.frustum_planes[i]);
        if (d < -radius) {
            return false;
        }
    }
    return true;
}

@compute @workgroup_size(64, 1, 1)
fn shadow_cluster_culling(@builtin(global_invocation_id) gid: vec3<u32>) {
    let instance_id = gid.x;
    if (instance_id >= uniforms.counts.x) { return; }

    let inst = instances[instance_id];

    // Stage 1: instance bounding sphere vs light frustum
    let center = vec3<f32>(inst.bounds_center_x, inst.bounds_center_y, inst.bounds_center_z);
    if (!sphere_vs_frustum(center, inst.bounds_radius)) {
        return;
    }

    // Stage 2: per-cluster (meshlet) bounding sphere cull
    let cluster_start = inst.meta0.z;
    let cluster_count = inst.meta0.w;

    for (var c: u32 = 0u; c < cluster_count; c = c + 1u) {
        let global_cluster_idx = cluster_start + c;
        let cmap = cluster_map[global_cluster_idx];
        let meshlet_idx = cmap.data.x;
        let meshlet = meshlets[meshlet_idx];

        // Meshlet center = (cone1.w, cone2.x, cone2.y); radius = cone2.z
        let local_center = vec3<f32>(meshlet.cone1.w, meshlet.cone2.x, meshlet.cone2.y);
        let world_center = (inst.world_matrix * vec4<f32>(local_center, 1.0)).xyz;

        if (!sphere_vs_frustum(world_center, meshlet.cone2.z)) {
            continue;
        }

        // Stage 3: append to visible list
        let list_idx = atomicAdd(&visible_counter, 1u);
        visible_clusters[list_idx] = global_cluster_idx;
    }

    // First thread seeds the indirect args (instance_count is finalized by
    // shadow_finalize_indirect).
    if (instance_id == 0u) {
        indirect_args[0] = 126u * 3u;  // vertex_count
        indirect_args[1] = 0u;          // instance_count
        indirect_args[2] = 0u;          // first_vertex
        indirect_args[3] = 0u;          // first_instance
    }
}

@compute @workgroup_size(64, 1, 1)
fn shadow_finalize_indirect(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x != 0u) { return; }

    let count = atomicLoad(&visible_counter);

    // Insertion sort visible_clusters[0..count-1] for deterministic ordering.
    for (var i: u32 = 1u; i < count; i = i + 1u) {
        let key = visible_clusters[i];
        var j: i32 = i32(i) - 1;
        while j >= 0 && visible_clusters[u32(j)] > key {
            visible_clusters[u32(j) + 1u] = visible_clusters[u32(j)];
            j = j - 1;
        }
        visible_clusters[u32(j) + 1u] = key;
    }

    indirect_args[1] = count;
    atomicStore(&visible_counter, 0u);
}
