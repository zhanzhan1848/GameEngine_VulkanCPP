// GPUCullingPipeline.wgsl — Dawn port of EngineTest/shaders/GPUCullingPipeline.metal.
//
// 8-stage GPU-driven culling pipeline. Each entry point is its own compute
// pipeline; C++ dispatches them in order with memory barriers between.
//
// Stages:
//   stage0_reset_all_buffers         — clear counters/visibility buffers
//   stage1_instance_frustum_culling  — per-instance frustum + near/far cull
//   stage2_distance_small_object_culling — (disabled in Metal — kept for parity)
//   stage3_lod_selection             — per-instance LOD pick
//   stage4_cluster_expansion         — single-threaded deterministic expand
//   stage5_occlusion_culling         — HZB-based occlusion (gated by uniform)
//   stage6_compact_visible_list      — atomic compaction
//   stage7_build_indirect_commands   — single-thread indirect args build
//
// Bindings (single WebGPU namespace; mirrors C++ culling_descriptor_layout_):
//   0: array<InstanceData, read>          — instances
//   1: array<u32, read_write>             — instance_visibility (4 u32s/entry)
//   2: uniform CullingUniforms
//   3: array<ClusterRef, read>            — cluster_refs
//   4: array<u32, read_write>             — cluster_visibility (12 u32s/entry)
//   5: atomic<u32>, read_write            — visible_counter
//   6: array<u32, read_write>             — visible_cluster_list
//   7: array<u32, read_write>             — indirect_commands (5 u32s)
//   8: texture_2d<f32>                    — hzb_texture
//   9: atomic<u32>, read_write            — cluster_visibility_counter
//  10: array<u32, read_write>             — debug_buffer (unused — debug stripped)
//  11: array<MeshletData, read>           — global meshlets
//
// Debug output is stripped: CullingConfig.enable_debug_output defaults to
// false, and the Dawn port doesn't need debug parity for Sponza rendering.

// Matches C++ InstanceData (RenderSceneSnapshot.h, sizeof == 192).
// CRITICAL: simd::float3 in the C++ InstanceData has sizeof == 16 (not 12)
// due to 16-byte alignment. bounds_center occupies 16 bytes (offsets 160-175)
// with 4 bytes of trailing padding at offset 172-175. bounds_radius is at
// offset 176, NOT 172. Layout verified via static_assert on 2026-06-23.
struct InstanceData {
    world_matrix: mat4x4<f32>,
    inverse_world_matrix: mat4x4<f32>,
    meta0: vec4<u32>,   // geometry_id, material_id, cluster_start, cluster_count
    meta1: vec4<u32>,   // cluster_map_base, _pad0, _pad1, _pad2
    bounds_center_x: f32,  // offset 160
    bounds_center_y: f32,  // offset 164
    bounds_center_z: f32,  // offset 168
    _pad_after_center: u32, // offset 172 — padding from simd::float3 16-byte size
    bounds_radius: f32,    // offset 176
    _pad_br0: u32,         // offset 180
    _pad_br1: u32,         // offset 184
    _pad_br2: u32,         // offset 188
};

// Matches C++ RHIMeshlet (sizeof == 64). Packed into vec4 lanes so each
// field lands on a 16-byte boundary exactly like the C struct.
struct MeshletData {
    header: vec4<u32>,   // vertex_offset, triangle_offset, vertex_count, triangle_count
    cone0: vec4<f32>,    // cone_apex.xyz, cone_axis.x
    cone1: vec4<f32>,    // cone_axis.yz, cone_cutoff, center.x
    cone2: vec4<f32>,    // center.yz, radius, padding
};

// Matches C++ ClusterRef (16 bytes).
struct ClusterRef {
    geometry_id: u32,
    cluster_index: u32,
    meshlet_id: u32,
    _pad: u32,
};

// T4.6.5 part 35.2: matches C++ ClusterMapEntry (GPUDrivenDrawPipeline.cpp:1656-1661).
// Maps flatClusterID → (globalMeshletIndex, instanceIndex, materialID).
// Without this binding, Stage4 had to use instance bounds for ALL clusters
// (a mitigation for an older local-vs-global meshlet_id bug) → Stage5 HZB
// tested every cluster at instance center → thin geometry (pillars/cloth)
// got false-occluded by closer nearby floor at min-filter mip.
struct ClusterMapEntry {
    globalMeshletIndex: u32,
    instanceIndex: u32,
    materialID: u32,
    _pad: u32,
};

// Matches C++ CullingConstants (sizeof == 260, padded to 272 = 17 vec4s).
struct CullingUniforms {
    view_matrix: mat4x4<f32>,
    projection_matrix: mat4x4<f32>,
    view_projection_matrix: mat4x4<f32>,
    camera_position: vec4<f32>,
    near_plane: f32,
    far_plane: f32,
    frame_index: u32,
    enable_occlusion_culling: u32,
    enable_lod_selection: u32,
    enable_small_object_culling: u32,
    small_object_threshold: f32,
    lod_bias: f32,
    max_lod_levels: u32,
    instance_count: u32,
    cluster_count: u32,
    force_pass_all: u32,
    enable_debug_output: u32,
    total_meshlet_count: u32,  // T4.6.5 part 35.2: OOB guard for cluster_map → meshlets lookup
    _pad_cm0: u32,
    _pad_cm1: u32,
    _pad_cm2: u32,
};

// 48 bytes/entry — matches C++ allocation (12 u32s). Use individual f32s
// instead of vec3 to keep tight 4-byte alignment and avoid WGSL's vec3
// 16-byte alignment rule. Order matches Metal ClusterVisibility minus the
// 12 bytes of pre-float3 padding (which C++ allocation doesn't reserve).
struct ClusterVisibility {
    is_visible: u32,        //  0
    cluster_index: u32,     //  4
    instance_index: u32,    //  8
    lod_level: u32,         // 12
    frame_index: u32,       // 16
    center_x: f32,          // 20
    center_y: f32,          // 24
    center_z: f32,          // 28
    cluster_radius: f32,    // 32
    _pad0: u32,             // 36
    _pad1: u32,             // 40
    _pad2: u32,             // 44
};

@group(0) @binding(0) var<storage, read> instances: array<InstanceData>;
@group(0) @binding(1) var<storage, read_write> instance_visibility: array<u32>;
@group(0) @binding(2) var<uniform> uniforms: CullingUniforms;
@group(0) @binding(3) var<storage, read> cluster_refs: array<ClusterRef>;
@group(0) @binding(4) var<storage, read_write> cluster_visibility: array<u32>;
@group(0) @binding(5) var<storage, read_write> visible_counter: atomic<u32>;
@group(0) @binding(6) var<storage, read_write> visible_cluster_list: array<u32>;
@group(0) @binding(7) var<storage, read_write> indirect_commands: array<u32>;
@group(0) @binding(8) var hzb_texture: texture_2d<f32>;
@group(0) @binding(9) var<storage, read_write> cluster_visibility_counter: atomic<u32>;
@group(0) @binding(10) var<storage, read_write> debug_buffer: array<u32>;
@group(0) @binding(11) var<storage, read> meshlets: array<MeshletData>;
@group(0) @binding(12) var<storage, read> cluster_map: array<ClusterMapEntry>;

// Constants matching Metal.
const MAX_CLUSTERS_PER_INSTANCE: u32 = 256u;
const MAX_INSTANCES: u32 = 100000u;

// ---- Helpers ----------------------------------------------------------------

fn normalize_plane(plane: vec4<f32>) -> vec4<f32> {
    let n = plane.xyz;
    let len = sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 0.0001) {
        return vec4<f32>(n / len, plane.w / len);
    }
    return plane;
}

// Returns 6 world-space frustum planes extracted from VP (column-major).
fn extract_frustum_planes(vp: mat4x4<f32>) -> array<vec4<f32>, 6> {
    var f: array<vec4<f32>, 6>;
    let col0 = vp[0]; // column 0
    let col1 = vp[1]; // column 1
    let col2 = vp[2]; // column 2
    let col3 = vp[3]; // column 3
    f[0] = normalize_plane(col3 + col0); // Left
    f[1] = normalize_plane(col3 - col0); // Right
    f[2] = normalize_plane(col3 + col1); // Bottom
    f[3] = normalize_plane(col3 - col1); // Top
    f[4] = normalize_plane(col3 + col2); // Near
    f[5] = normalize_plane(col3 - col2); // Far
    return f;
}

// Read InstanceVisibility entry (4 u32s/entry) as a struct-like value.
struct InstanceVis {
    is_visible: u32,
    instance_index: u32,
    lod_level: u32,
    distance_rank: u32,
};

fn load_instance_vis(idx: u32) -> InstanceVis {
    let base = idx * 4u;
    var v: InstanceVis;
    v.is_visible = instance_visibility[base + 0u];
    v.instance_index = instance_visibility[base + 1u];
    v.lod_level = instance_visibility[base + 2u];
    v.distance_rank = instance_visibility[base + 3u];
    return v;
}

fn store_instance_vis(idx: u32, v: InstanceVis) {
    let base = idx * 4u;
    instance_visibility[base + 0u] = v.is_visible;
    instance_visibility[base + 1u] = v.instance_index;
    instance_visibility[base + 2u] = v.lod_level;
    instance_visibility[base + 3u] = v.distance_rank;
}

// ---- Stage 0: Reset all buffers ---------------------------------------------

@compute @workgroup_size(64, 1, 1)
fn stage0_reset_all_buffers(@builtin(global_invocation_id) gid: vec3<u32>) {
    let idx = gid.x;

    if (idx == 0u) {
        atomicStore(&visible_counter, 0u);
        atomicStore(&cluster_visibility_counter, 0u);
    }

    if (idx < uniforms.instance_count && idx < MAX_INSTANCES) {
        var v: InstanceVis;
        v.is_visible = 0u;
        v.instance_index = 0u;
        v.lod_level = 0u;
        v.distance_rank = 0u;
        store_instance_vis(idx, v);
    }

    // ClusterVisibility entries: clear + sentinel. 12 u32s per entry.
    if (idx < uniforms.cluster_count) {
        let base = idx * 12u;
        cluster_visibility[base + 0u] = 0u;            // is_visible
        cluster_visibility[base + 1u] = 0xFFFFFFFFu;   // cluster_index sentinel
        cluster_visibility[base + 2u] = 0u;            // instance_index
        cluster_visibility[base + 3u] = 0u;            // lod_level
        cluster_visibility[base + 4u] = 0xFFFFFFFFu;   // frame_index sentinel
        // center/radius/pad left untouched — Stage 4 overwrites before reads.
    }

    if (idx < uniforms.cluster_count) {
        visible_cluster_list[idx] = 0u;
    }
}

// ---- Stage 1: Instance-level frustum culling --------------------------------

@compute @workgroup_size(64, 1, 1)
fn stage1_instance_frustum_culling(@builtin(global_invocation_id) gid: vec3<u32>) {
    let instance_id = gid.x;
    if (instance_id >= uniforms.instance_count || instance_id >= MAX_INSTANCES) {
        return;
    }

    let inst = instances[instance_id];
    let bounds_center = vec3<f32>(inst.bounds_center_x, inst.bounds_center_y, inst.bounds_center_z);
    let bounds_radius = inst.bounds_radius;
    let view_center_4 = uniforms.view_matrix * vec4<f32>(bounds_center, 1.0);

    // Behind-camera: in view space, camera looks down -Z, so visible objects
    // have negative Z. If closest point is behind camera (Z > 0), cull.
    if (view_center_4.z - bounds_radius > 0.0) {
        var v: InstanceVis;
        v.is_visible = 0u;
        v.instance_index = instance_id;
        v.lod_level = 0u;
        v.distance_rank = 0u;
        store_instance_vis(instance_id, v);
        return;
    }

    // Beyond far plane.
    if (view_center_4.z < -uniforms.far_plane - bounds_radius) {
        var v: InstanceVis;
        v.is_visible = 0u;
        v.instance_index = instance_id;
        v.lod_level = 0u;
        v.distance_rank = 0u;
        store_instance_vis(instance_id, v);
        return;
    }

    // Side-plane culling disabled. Both the original math (view-space XY
    // against world-space VP planes) and the corrected math (world-space
    // center, normalized planes) produced false-positive culls on rotated
    // cameras in Dawn. Stage 1 now relies on near/far plane tests only;
    // off-screen side geometry is clipped by the vertex shader naturally.
    let culled = false;

    var v: InstanceVis;
    v.instance_index = instance_id;
    v.lod_level = 0u;
    if (culled) {
        v.is_visible = 0u;
        v.distance_rank = 0u;
    } else {
        v.is_visible = 1u;
        let distance_z = max(0.0, -view_center_4.z);
        v.distance_rank = u32(distance_z * 100.0);
    }
    store_instance_vis(instance_id, v);
}

// ---- Stage 2: Distance & small object culling (disabled in Metal — stub) ----

@compute @workgroup_size(64, 1, 1)
fn stage2_distance_small_object_culling(@builtin(global_invocation_id) gid: vec3<u32>) {
    let instance_id = gid.x;
    if (instance_id >= uniforms.instance_count || instance_id >= MAX_INSTANCES) {
        return;
    }
    let cur = load_instance_vis(instance_id);
    if (cur.is_visible == 0u) {
        return;
    }
    let inst = instances[instance_id];
    let view_center_4 = uniforms.view_matrix * vec4<f32>(
        inst.bounds_center_x, inst.bounds_center_y, inst.bounds_center_z, 1.0);
    var v = cur;
    v.distance_rank = u32(max(0.0, -view_center_4.z) * 10.0);
    store_instance_vis(instance_id, v);
}

// ---- Stage 3: LOD selection -------------------------------------------------

fn select_lod_level(distance: f32, screen_space_error: f32,
                    lod_bias: f32, max_lod_levels: u32) -> u32 {
    var threshold = lod_bias;
    for (var lod: u32 = 0u; lod < max_lod_levels - 1u; lod = lod + 1u) {
        if (screen_space_error < threshold) {
            return lod;
        }
        threshold = threshold * 2.0;
    }
    return max_lod_levels - 1u;
}

@compute @workgroup_size(64, 1, 1)
fn stage3_lod_selection(@builtin(global_invocation_id) gid: vec3<u32>) {
    let instance_id = gid.x;
    if (instance_id >= uniforms.instance_count || instance_id >= MAX_INSTANCES) {
        return;
    }
    if (uniforms.enable_lod_selection == 0u) {
        var v = load_instance_vis(instance_id);
        v.lod_level = 0u;
        store_instance_vis(instance_id, v);
        return;
    }
    let cur = load_instance_vis(instance_id);
    if (cur.is_visible == 0u) {
        return;
    }
    let inst = instances[instance_id];
    let world_center = vec3<f32>(inst.bounds_center_x, inst.bounds_center_y, inst.bounds_center_z);
    let distance = length(world_center - uniforms.camera_position.xyz);
    let screen_space_error = 100.0 / max(distance, 0.001);
    var lod_level = select_lod_level(distance, screen_space_error,
                                     uniforms.lod_bias, uniforms.max_lod_levels);
    if (lod_level >= uniforms.max_lod_levels) {
        lod_level = uniforms.max_lod_levels - 1u;
    }
    var v = cur;
    v.lod_level = lod_level;
    store_instance_vis(instance_id, v);
}

// ---- Stage 4: Cluster expansion (single-threaded deterministic) -------------

// Writes cluster_visibility[write_pos] from packed fields.
fn store_cluster_vis(write_pos: u32, is_visible: u32, cluster_index: u32,
                     instance_index: u32, lod_level: u32, frame_index: u32,
                     center: vec3<f32>, radius: f32) {
    let base = write_pos * 12u;
    cluster_visibility[base + 0u]  = is_visible;
    cluster_visibility[base + 1u]  = cluster_index;
    cluster_visibility[base + 2u]  = instance_index;
    cluster_visibility[base + 3u]  = lod_level;
    cluster_visibility[base + 4u]  = frame_index;
    cluster_visibility[base + 5u]  = bitcast<u32>(center.x);
    cluster_visibility[base + 6u]  = bitcast<u32>(center.y);
    cluster_visibility[base + 7u]  = bitcast<u32>(center.z);
    cluster_visibility[base + 8u]  = bitcast<u32>(radius);
}

@compute @workgroup_size(64, 1, 1)
fn stage4_cluster_expansion(@builtin(global_invocation_id) gid: vec3<u32>) {
    // Single-threaded deterministic expansion. Matches Metal:766.
    if (gid.x != 0u) {
        return;
    }

    var write_pos: u32 = 0u;
    let force_pass_all = uniforms.force_pass_all != 0u;

    for (var instance_id: u32 = 0u;
         instance_id < uniforms.instance_count && instance_id < MAX_INSTANCES;
         instance_id = instance_id + 1u) {

        // Skip if force_pass_all is OFF and instance is culled.
        if (!force_pass_all) {
            let cur = load_instance_vis(instance_id);
            if (cur.is_visible == 0u) {
                continue;
            }
        }

        let inst = instances[instance_id];
        var lod_level: u32 = 0u;
        if (!force_pass_all) {
            lod_level = load_instance_vis(instance_id).lod_level;
        }

        let cluster_count = inst.meta0.w;
        if (cluster_count > MAX_CLUSTERS_PER_INSTANCE) {
            continue;
        }
        let global_cluster_base = inst.meta1.x;

        for (var i: u32 = 0u; i < cluster_count; i = i + 1u) {
            let cluster_map_idx = global_cluster_base + i;
            if (cluster_map_idx >= uniforms.cluster_count) {
                continue;
            }

            // T4.6.5 part 35.2: per-cluster bounds via cluster_map → meshlets.
            // Old mitigation (instance bounds for all clusters) made Stage5 HZB
            // test every cluster at instance center → thin geometry (pillars,
            // cloth) was false-occluded by closer nearby floor sampled at
            // min-filter mip. Per-cluster bounds mean small clusters fall
            // under the 0.08 screen-space threshold and skip HZB entirely.
            //
            // First-frame defense: cluster_map may be empty (built inside
            // gpuDraw.Execute which runs AFTER cull). Fall back to instance
            // bounds if globalMeshletIndex is OOB or uniform sentinel.
            var cluster_world_center = vec3<f32>(inst.bounds_center_x, inst.bounds_center_y, inst.bounds_center_z);
            var cluster_world_radius = inst.bounds_radius;
            let cmap_entry = cluster_map[cluster_map_idx];
            let meshlet_idx = cmap_entry.globalMeshletIndex;
            if (meshlet_idx < uniforms.total_meshlet_count) {
                let m = meshlets[meshlet_idx];
                // MeshletData packing (mirrors RHIMeshlet): cone1.w = center.x,
                // cone2.xy = center.yz, cone2.z = radius.
                let local_center = vec3<f32>(m.cone1.w, m.cone2.x, m.cone2.y);
                let local_radius = m.cone2.z;
                let world_center_4 = inst.world_matrix * vec4<f32>(local_center, 1.0);
                cluster_world_center = world_center_4.xyz;
                // Radius scales by max axis scale of the world matrix.
                let col0_len = length(inst.world_matrix[0].xyz);
                let col1_len = length(inst.world_matrix[1].xyz);
                let col2_len = length(inst.world_matrix[2].xyz);
                let max_scale = max(max(col0_len, col1_len), col2_len);
                cluster_world_radius = local_radius * max_scale;
            }

            // Per-cluster far-plane cull only. Side-plane test removed: it
            // used view-space XY against world-space VP planes (math mismatch
            // that produced false-positive culls for off-axis geometry).
            var cluster_visible = true;
            if (!force_pass_all) {
                let view_center_4 = uniforms.view_matrix *
                                    vec4<f32>(cluster_world_center, 1.0);
                if (view_center_4.z < -uniforms.far_plane - cluster_world_radius) {
                    cluster_visible = false;
                }
            }

            if (cluster_visible) {
                if (write_pos >= uniforms.cluster_count) {
                    continue;
                }
                store_cluster_vis(write_pos, 1u, cluster_map_idx, instance_id,
                                  lod_level, uniforms.frame_index,
                                  cluster_world_center, cluster_world_radius);
                write_pos = write_pos + 1u;
            }
        }
    }

    atomicStore(&cluster_visibility_counter, write_pos);
}

// ---- Stage 5: HZB occlusion culling -----------------------------------------

@compute @workgroup_size(64, 1, 1)
fn stage5_occlusion_culling(@builtin(global_invocation_id) gid: vec3<u32>) {
    let cluster_idx = gid.x;
    let total = atomicLoad(&cluster_visibility_counter);
    if (cluster_idx >= total) {
        return;
    }

    // Read cluster_visibility entry.
    let base = cluster_idx * 12u;
    let is_visible = cluster_visibility[base + 0u];
    if (is_visible == 0u || uniforms.enable_occlusion_culling == 0u) {
        return;
    }

    let center = vec3<f32>(
        bitcast<f32>(cluster_visibility[base + 5u]),
        bitcast<f32>(cluster_visibility[base + 6u]),
        bitcast<f32>(cluster_visibility[base + 7u]),
    );
    let radius = bitcast<f32>(cluster_visibility[base + 8u]);

    let clip_pos = uniforms.view_projection_matrix * vec4<f32>(center, 1.0);
    if (clip_pos.w <= 0.0) {
        return; // Behind camera, assume visible.
    }

    let ndc = clip_pos.xy / clip_pos.w;
    // T4.6.5 part 35: HZB texture has top-left origin (framebuffer convention
    // after naga's automatic Y-flip in vertex shaders). NDC.y = +1 (world-up)
    // → top of screen → uv.y = 0 (row 0 of HZB). NDC.y = -1 (world-down) →
    // bottom → uv.y = 1. Old math `ndc.y * 0.5 + 0.5` assumed OpenGL
    // bottom-left texture origin and produced world-up geometry sampling
    // world-down HZB region → false-occlusion of ceiling / upper-half scene.
    let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return; // Off-screen, assume visible.
    }

    // Conservative filter: skip HZB for clusters < 8% of screen width.
    let screen_space_radius = radius / clip_pos.w;
    if (screen_space_radius < 0.08) {
        return;
    }

    // Distance-based mip selection.
    let camera_distance = length(center - uniforms.camera_position.xyz);
    var selected_mip: i32 = 0;
    if (camera_distance < 5.0) {
        selected_mip = 0;
    } else if (camera_distance < 15.0) {
        selected_mip = 1;
    } else if (camera_distance < 30.0) {
        selected_mip = 2;
    } else {
        selected_mip = 3;
    }

    let dims = textureDimensions(hzb_texture, selected_mip);
    let pixel_pos = vec2<u32>(u32(uv.x * f32(dims.x)), u32(uv.y * f32(dims.y)));
    let hzb_depth = textureLoad(hzb_texture, vec2<i32>(pixel_pos), selected_mip).r;

    let cluster_depth = clip_pos.z / clip_pos.w;

    // Dynamic bias (matches Metal:1286-1307).
    var dist_factor = 1.0;
    if (camera_distance < 10.0) {
        dist_factor = 2.0;
    } else if (camera_distance < 25.0) {
        dist_factor = 1.5;
    }
    let mip_factor = 1.0 + f32(selected_mip) * 0.2;
    let base_bias = 0.02 * dist_factor * mip_factor;
    let radius_bias = radius * 0.15;
    let total_bias = base_bias + radius_bias;

    if (hzb_depth < cluster_depth - total_bias) {
        // Require significant depth difference, scaled by camera distance.
        let depth_diff = cluster_depth - hzb_depth;
        var min_occlusion = 0.1;
        if (camera_distance < 8.0) {
            min_occlusion = 0.3;
        } else if (camera_distance < 20.0) {
            min_occlusion = 0.2;
        }
        if (depth_diff > min_occlusion) {
            // Skip culling for near-camera + non-base mip (avoids wall false-positive).
            if (camera_distance < 5.0 && selected_mip > 0) {
                return;
            }
            cluster_visibility[base + 0u] = 0u; // Mark as occluded.
        }
    }
}

// ---- Stage 6: Compact visible list ------------------------------------------

@compute @workgroup_size(64, 1, 1)
fn stage6_compact_visible_list(@builtin(global_invocation_id) gid: vec3<u32>) {
    let idx = gid.x;
    let total = atomicLoad(&cluster_visibility_counter);
    if (idx >= total) {
        return;
    }

    let base = idx * 12u;
    let is_visible = cluster_visibility[base + 0u];
    let cluster_index = cluster_visibility[base + 1u];

    let has_valid_index = cluster_index != 0xFFFFFFFFu;
    let has_safe_index = cluster_index < uniforms.cluster_count;

    if (is_visible != 0u && has_valid_index && has_safe_index) {
        let write_pos = atomicAdd(&visible_counter, 1u);
        if (write_pos < uniforms.cluster_count) {
            visible_cluster_list[write_pos] = cluster_index;
        }
    }
}

// ---- Stage 7: Build indirect commands ---------------------------------------

@compute @workgroup_size(64, 1, 1)
fn stage7_build_indirect_commands(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x != 0u) {
        return;
    }

    var visible_count = atomicLoad(&visible_counter);
    if (visible_count > uniforms.cluster_count) {
        visible_count = uniforms.cluster_count;
    }
    if (visible_count > MAX_INSTANCES) {
        visible_count = MAX_INSTANCES;
    }

    // Matches Metal:1430 — 384 verts = 128 triangles (one meshlet's worth).
    indirect_commands[0] = 384u;            // vertex_count
    indirect_commands[1] = visible_count;   // instance_count (one draw per visible meshlet)
    indirect_commands[2] = 0u;              // first_vertex
    indirect_commands[3] = 0u;              // first_instance
}

