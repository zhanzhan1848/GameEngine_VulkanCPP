// GlobalSDFVoxelization.wgsl — Dawn port of Nanite/GlobalSDFVoxelization.metal.
//
// Each thread processes one voxel: walks visible instances, does bounds-cull +
// meshlet-cull + per-triangle point-to-triangle distance, tracks min distance,
// writes the result to the cascade SDF texture as R16_Float.
//
// Dispatch: (resolution, resolution, resolution), workgroup_size = (4, 4, 4).
//
// Bindings (WebGPU — single namespace, no Metal texture/buffer split):
//   0: texture_storage_3d<r16float, write>  — SDF output (cascade)
//   1: uniform CascadeUniforms
//   2: storage<read> array<f32>             — vertex positions (packed_float3, 12 bytes/vertex)
//   3: storage<read> array<MeshletData>     — meshlet descriptors
//   4: storage<read> array<u32>             — meshlet_vertex_indices (u32 per global index)
//   5: storage<read> array<u32>             — meshlet_triangle_indices (u32 per byte; C++ repacks u8→u32)
//   6: storage<read> array<ClusterMap>      — cluster index map
//   7: storage<read> array<InstanceData>    — instance descriptors

struct CascadeUniforms {
    // xyz = cascade origin, w = voxel_size. Matches C++ CascadeUniformsCB
    // at GlobalSDF.cpp:430-435.
    origin: vec4<f32>,
    // res_x, res_y, res_z, num_instances. vec4<u32> keeps 16-byte uniform stride.
    params: vec4<u32>,
};

// Matches C++ InstanceData (RenderSceneSnapshot.h:19-34, sizeof == 192).
// CRITICAL: C++ uses math::v3 = simd::float3 which has sizeof == 16 (not 12)
// due to 16-byte alignment. bounds_center occupies offsets 160-175 (with 4
// bytes of trailing padding). bounds_radius is at offset 176, NOT 172.
struct InstanceData {
    world_matrix: mat4x4<f32>,           // offset   0
    inverse_world_matrix: mat4x4<f32>,   // offset  64
    meta0: vec4<u32>,                    // offset 128 — geometry_id, material_id, cluster_start, cluster_count
    meta1: vec4<u32>,                    // offset 144 — cluster_map_base, _pad0, _pad1, _pad2
    bounds_center_x: f32,                // offset 160
    bounds_center_y: f32,                // offset 164
    bounds_center_z: f32,                // offset 168
    _pad_after_center: u32,              // offset 172 — padding from simd::float3 16-byte size
    bounds_radius: f32,                  // offset 176
    _pad_br0: u32,                       // offset 180
    _pad_br1: u32,                       // offset 184
    _pad_br2: u32,                       // offset 188
};

// Matches C++ RHIMeshlet (RHIMeshAsset.h:7-19, sizeof == 64). Packed into
// vec4 lanes so each field lands on a 16-byte boundary exactly like the C struct.
struct MeshletData {
    header: vec4<u32>,   // offset  0 — vertex_offset, triangle_offset, vertex_count, triangle_count
    cone0: vec4<f32>,    // offset 16 — cone_apex.xyz, cone_axis.x
    cone1: vec4<f32>,    // offset 32 — cone_axis.yz, cone_cutoff, center.x
    cone2: vec4<f32>,    // offset 48 — center.yz, radius, padding
};

struct ClusterMap {
    data: vec4<u32>,   // globalMeshletIndex, instanceIndex, materialID, _pad
};

// Adapted from GlobalSDFVoxelization.metal:69-113.
fn point_to_triangle_distance(p: vec3<f32>, a: vec3<f32>, b: vec3<f32>, c: vec3<f32>) -> f32 {
    let ab = b - a;
    let ac = c - a;
    let ap = p - a;

    let d1 = dot(ab, ap);
    let d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) { return length(ap); }

    let bp = p - b;
    let d3 = dot(ab, bp);
    let d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) { return length(bp); }

    let vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        let v = d1 / (d1 - d3);
        return length(ap + v * ab);
    }

    let cp = p - c;
    let d5 = dot(ab, cp);
    let d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) { return length(cp); }

    let vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        let w = d2 / (d2 - d6);
        return length(ap + w * ac);
    }

    let va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        let w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return length(b + w * (c - b) - p);
    }

    let denom = 1.0 / (va + vb + vc);
    let v = vb * denom;
    let w = vc * denom;
    return length(ap + ab * v + ac * w - p);
}

@group(0) @binding(0) var sdf_output: texture_storage_3d<r16float, write>;
@group(0) @binding(1) var<uniform> cascade: CascadeUniforms;
@group(0) @binding(2) var<storage, read> vertex_positions: array<f32>;
@group(0) @binding(3) var<storage, read> meshlets: array<MeshletData>;
@group(0) @binding(4) var<storage, read> meshlet_vertex_indices: array<u32>;
@group(0) @binding(5) var<storage, read> meshlet_triangle_indices: array<u32>;
@group(0) @binding(6) var<storage, read> cluster_map: array<ClusterMap>;
@group(0) @binding(7) var<storage, read> instance_data: array<InstanceData>;

@compute @workgroup_size(4, 4, 4)
fn voxelize_sdf(@builtin(global_invocation_id) tid: vec3<u32>) {
    let resolution = cascade.params.xyz;
    if (any(tid >= resolution)) { return; }

    let origin = cascade.origin.xyz;
    let voxel_size = cascade.origin.w;
    let num_instances = cascade.params.w;

    let voxel_pos = origin + (vec3<f32>(tid) + 0.5) * voxel_size;

    var min_dist = 1.0e10;

    for (var inst_idx: u32 = 0u; inst_idx < num_instances; inst_idx++) {
        if (min_dist <= 0.0) { break; }

        let inst = instance_data[inst_idx];

        // Instance bounding sphere cull (matches metal:159-161).
        let inst_center = vec3<f32>(inst.bounds_center_x, inst.bounds_center_y, inst.bounds_center_z);
        let to_center = voxel_pos - inst_center;
        let dist_to_bounds = length(to_center) - inst.bounds_radius;
        if (dist_to_bounds > voxel_size * 4.0) { continue; }

        let cluster_start = inst.meta0.z;
        let cluster_count = inst.meta0.w;

        for (var c: u32 = 0u; c < cluster_count; c++) {
            let global_cluster_idx = cluster_start + c;
            let cmap = cluster_map[global_cluster_idx];
            let meshlet_idx = cmap.data.x;

            let meshlet = meshlets[meshlet_idx];

            // Reconstruct meshlet center & radius (RHIMeshlet.h:7-19).
            // center = (cone1.w, cone2.x, cone2.y), radius = cone2.z.
            let meshlet_center = vec3<f32>(meshlet.cone1.w, meshlet.cone2.x, meshlet.cone2.y);
            let meshlet_radius = meshlet.cone2.z;

            // Transform meshlet center to world space.
            let world_center = inst.world_matrix * vec4<f32>(meshlet_center, 1.0);
            let dist_to_meshlet = length(voxel_pos - world_center.xyz) - meshlet_radius;
            if (dist_to_meshlet > voxel_size * 2.0) { continue; }

            let tri_offset = meshlet.header.y;   // bytes (matches metal)
            let tri_count = meshlet.header.w;
            let vert_offset = meshlet.header.x;

            for (var t: u32 = 0u; t < tri_count; t++) {
                // meshlet_triangle_indices is bound as array<u32> but the C++ data
                // is u8 per byte (RHIMeshAsset.h:49 utl::vector<u8>). Read individual
                // bytes via word/shift math — avoids C++ repack.
                let base_byte = tri_offset + t * 3u;
                let w0 = base_byte / 4u;
                let w1 = (base_byte + 1u) / 4u;
                let w2 = (base_byte + 2u) / 4u;
                let i0 = (meshlet_triangle_indices[w0] >> ((base_byte % 4u) * 8u)) & 0xFFu;
                let i1 = (meshlet_triangle_indices[w1] >> (((base_byte + 1u) % 4u) * 8u)) & 0xFFu;
                let i2 = (meshlet_triangle_indices[w2] >> (((base_byte + 2u) % 4u) * 8u)) & 0xFFu;

                let gi0 = meshlet_vertex_indices[vert_offset + i0];
                let gi1 = meshlet_vertex_indices[vert_offset + i1];
                let gi2 = meshlet_vertex_indices[vert_offset + i2];

                // Vertex positions are packed_float3 (12 bytes/vertex) → array<f32> view.
                let lp0 = vec3<f32>(vertex_positions[gi0 * 3u], vertex_positions[gi0 * 3u + 1u], vertex_positions[gi0 * 3u + 2u]);
                let lp1 = vec3<f32>(vertex_positions[gi1 * 3u], vertex_positions[gi1 * 3u + 1u], vertex_positions[gi1 * 3u + 2u]);
                let lp2 = vec3<f32>(vertex_positions[gi2 * 3u], vertex_positions[gi2 * 3u + 1u], vertex_positions[gi2 * 3u + 2u]);

                let wp0 = (inst.world_matrix * vec4<f32>(lp0, 1.0)).xyz;
                let wp1 = (inst.world_matrix * vec4<f32>(lp1, 1.0)).xyz;
                let wp2 = (inst.world_matrix * vec4<f32>(lp2, 1.0)).xyz;

                let d = point_to_triangle_distance(voxel_pos, wp0, wp1, wp2);
                min_dist = min(min_dist, d);
            }
        }
    }

    textureStore(sdf_output, vec3<i32>(tid), vec4<f32>(min_dist, 0.0, 0.0, 0.0));
}
