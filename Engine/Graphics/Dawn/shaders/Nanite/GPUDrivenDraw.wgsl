// GPUDrivenDraw.wgsl — Dawn port of EngineTest/shaders/GPUDrivenDraw.metal.
//
// GPU-driven meshlet draw via vertex pulling + DrawIndirect. Produces GBuffer
// MRT (albedo/normal/orm/velocity) for downstream deferred lighting.
//
// Bindings (single WebGPU namespace; mirrors C++ draw_descriptor_layout_):
//   0: uniform DrawConstants          — camera matrices (vertex+fragment)
//   1: array<Meshlet, read>           — global_meshlet_buffer_
//   2: array<u32, read>               — global_meshlet_vertices_buffer_
//   3: array<u32, read>               — global_meshlet_triangles_buffer_ (u8 packed)
//   4: array<f32, read>               — global_vertex_buffer_ (packed_float3, 12B/vertex)
//   5: array<u32, read>               — visible_cluster_list (from culling)
//   6: array<ClusterMap, read>        — cluster_map_buffer_
//   7: array<InstanceData, read>      — global_instance_data_buffer_
//   8: array<u32, read>               — global_element_buffer_ (VertexElement packed)
//   9: array<u32, read>               — global_material_data_buffer_ (MaterialData packed)
//  10: texture_2d_array<f32>          — albedo_texture_array_
//  11: texture_2d_array<f32>          — normal_texture_array_
//  12: texture_2d_array<f32>          — orm_texture_array_
//  13: sampler                        — texture_sampler_

// Matches C++ DrawConstants (352 bytes). prev_view_matrix at offset 208 is
// 16-aligned naturally (208 / 16 = 13). padding2 declared as 3 individual
// u32s to avoid WGSL vec3<u32> 16-byte alignment pushing the field to 352.
struct DrawConstants {
    view_matrix: mat4x4<f32>,
    proj_matrix: mat4x4<f32>,
    world_matrix: mat4x4<f32>,
    view_width: u32,
    view_height: u32,
    meshlet_count: u32,
    padding: u32,
    prev_view_matrix: mat4x4<f32>,
    prev_proj_matrix: mat4x4<f32>,
    has_prev_frame: u32,
    debug_mode: u32,   // 0=off, 1=meshlet_id, 2=triangle_id, 3=mesh_id
    pad2_1: u32,
    pad2_2: u32,
};

// Matches C++ RHIMeshlet (64 bytes). See GlobalSDFVoxelization.wgsl.
struct Meshlet {
    header: vec4<u32>,   // vertex_offset, triangle_offset, vertex_count, triangle_count
    cone0: vec4<f32>,
    cone1: vec4<f32>,
    cone2: vec4<f32>,
};

struct ClusterMap {
    data: vec4<u32>,   // globalMeshletIndex, instanceIndex, materialID, _pad
};

// Matches C++ InstanceData (192 bytes).
// CRITICAL: simd::float3 in C++ has sizeof == 16 (16-byte alignment), so
// bounds_center occupies offsets 160-175 with 4 bytes of trailing padding.
// bounds_radius is at offset 176, NOT 172.
struct InstanceData {
    world_matrix: mat4x4<f32>,
    inverse_world_matrix: mat4x4<f32>,
    meta0: vec4<u32>,   // geometry_id, material_id, cluster_start, cluster_count
    meta1: vec4<u32>,   // cluster_map_base, _pad0, _pad1, _pad2
    bounds_center_x: f32,    // offset 160
    bounds_center_y: f32,    // offset 164
    bounds_center_z: f32,    // offset 168
    _pad_after_center: u32,  // offset 172
    bounds_radius: f32,      // offset 176
    _pad_br0: u32,           // offset 180
    _pad_br1: u32,           // offset 184
    _pad_br2: u32,           // offset 188
};

@group(0) @binding(0) var<uniform> uniforms: DrawConstants;
@group(0) @binding(1) var<storage, read> meshlets: array<Meshlet>;
@group(0) @binding(2) var<storage, read> meshlet_vertices: array<u32>;
@group(0) @binding(3) var<storage, read> meshlet_triangles: array<u32>;
@group(0) @binding(4) var<storage, read> positions: array<f32>;
@group(0) @binding(5) var<storage, read> compact_cluster_ids: array<u32>;
@group(0) @binding(6) var<storage, read> cluster_map: array<ClusterMap>;
@group(0) @binding(7) var<storage, read> instance_data: array<InstanceData>;
@group(0) @binding(8) var<storage, read> elements: array<u32>;
@group(0) @binding(9) var<storage, read> material_data: array<u32>;
@group(0) @binding(10) var albedo_textures: texture_2d_array<f32>;
@group(0) @binding(11) var normal_textures: texture_2d_array<f32>;
@group(0) @binding(12) var orm_textures: texture_2d_array<f32>;
@group(0) @binding(13) var texture_sampler: sampler;

// ---- Per-vertex element access (24 bytes = 6 u32s per VertexElement) --------

struct VertexElement {
    color_t_sign: u32,
    normal: u32,
    tangent: u32,
    uv: vec2<f32>,
};

fn load_vertex_element(idx: u32) -> VertexElement {
    let base = idx * 6u;
    var e: VertexElement;
    e.color_t_sign = elements[base + 0u];
    e.normal = elements[base + 1u];
    e.tangent = elements[base + 2u];
    // base + 3u is padding.
    e.uv = vec2<f32>(
        bitcast<f32>(elements[base + 4u]),
        bitcast<f32>(elements[base + 5u]),
    );
    return e;
}

// ---- Per-material access (48 bytes = 12 u32s per MaterialData) --------------

struct MaterialInfo {
    albedo_texture_idx: u32,
    normal_texture_idx: u32,
    orm_texture_idx: u32,
    albedo_tint: vec3<f32>,
    metallic_factor: f32,
    roughness_factor: f32,
    normal_scale: f32,
    uv_scale: vec2<f32>,
    flags: u32,
};

fn load_material(idx: u32) -> MaterialInfo {
    let base = idx * 12u;
    var m: MaterialInfo;
    m.albedo_texture_idx = material_data[base + 0u];
    m.normal_texture_idx = material_data[base + 1u];
    m.orm_texture_idx = material_data[base + 2u];
    m.albedo_tint = vec3<f32>(
        bitcast<f32>(material_data[base + 3u]),
        bitcast<f32>(material_data[base + 4u]),
        bitcast<f32>(material_data[base + 5u]),
    );
    m.metallic_factor = bitcast<f32>(material_data[base + 6u]);
    m.roughness_factor = bitcast<f32>(material_data[base + 7u]);
    m.normal_scale = bitcast<f32>(material_data[base + 8u]);
    m.uv_scale = vec2<f32>(
        bitcast<f32>(material_data[base + 9u]),
        bitcast<f32>(material_data[base + 10u]),
    );
    m.flags = material_data[base + 11u];
    return m;
}

// ---- Helpers ----------------------------------------------------------------

// Unpacks a 2-component normal/tangent stored as 2x i16 packed into a u32.
// High 16 bits = x, low 16 bits = y. Maps [-32767, 32767] to [-1, 1].
fn unpack_normal(packed: u32) -> vec3<f32> {
    let hi = f32((packed >> 16u) & 0xFFFFu);
    let lo = f32(packed & 0xFFFFu);
    var f = vec2<f32>(hi, lo) / 32767.0 - 1.0;
    let d = dot(f, f);
    if (d > 1.0) {
        return vec3<f32>(0.0, 0.0, 1.0);
    }
    let z = sqrt(max(0.0, 1.0 - d));
    return vec3<f32>(f.x, f.y, z);
}

// Vertex shader output. Interpolated across the triangle.
struct VSOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) tangent: vec3<f32>,
    @location(3) bitangent: vec3<f32>,
    @location(4) @interpolate(flat) material_id: u32,
    @location(5) @interpolate(flat) meshlet_id: u32,
    @location(6) @interpolate(flat) triangle_id: u32,
    @location(7) @interpolate(flat) mesh_id: u32,
    @location(8) current_clip: vec4<f32>,
    @location(9) previous_clip: vec4<f32>,
};

// Hash u32 → vec3 color for debug visualization. Uses the PCG-style hash
// from MeshletDebug in the Metal port; output in [0,1]^3.
fn hash_id_to_color(id: u32) -> vec3<f32> {
    var h = id * 2654435761u;
    h = h ^ (h >> 16u);
    h = h * 0x85ebca6bu;
    h = h ^ (h >> 13u);
    h = h * 0xc2b2ae35u;
    h = h ^ (h >> 16u);
    return vec3<f32>(
        f32(h & 0xFFu) / 255.0,
        f32((h >> 8u) & 0xFFu) / 255.0,
        f32((h >> 16u) & 0xFFu) / 255.0,
    );
}

// ---- Vertex shader ----------------------------------------------------------

@vertex
fn gpu_driven_vertex_shader(
    @builtin(vertex_index) vertex_id: u32,
    @builtin(instance_index) instance_id: u32,
) -> VSOutput {
    var out: VSOutput;

    // Step 1: visible cluster index → global cluster ID.
    let global_cluster_id = compact_cluster_ids[instance_id];

    // Step 2: cluster ID → meshlet + instance + material.
    let cmap = cluster_map[global_cluster_id];
    let global_meshlet_index = cmap.data.x;
    let instance_index = cmap.data.y;
    let material_id = cmap.data.z;

    let inst = instance_data[instance_index];
    let meshlet = meshlets[global_meshlet_index];

    let triangle_count = meshlet.header.w;

    // Step 3: vertex pulling. Meshlet has 128 triangles max → 384 verts.
    let actual_vertex_count = triangle_count * 3u;
    if (vertex_id >= actual_vertex_count) {
        // Degenerate padding vertex — clip to NaN-ish position.
        out.position = vec4<f32>(0.0, 0.0, 0.0, 0.0);
        out.uv = vec2<f32>(0.0);
        out.normal = vec3<f32>(0.0, 0.0, 1.0);
        out.tangent = vec3<f32>(1.0, 0.0, 0.0);
        out.bitangent = vec3<f32>(0.0, 1.0, 0.0);
        out.material_id = material_id;
        out.meshlet_id = global_meshlet_index;
        out.triangle_id = 0u;
        out.mesh_id = instance_index;
        out.current_clip = out.position;
        out.previous_clip = out.position;
        return out;
    }

    // Local vertex index from packed u8 array.
    let tri_offset = meshlet.header.y;
    let base_byte = tri_offset + vertex_id;
    let word_idx = base_byte / 4u;
    let byte_shift = (base_byte % 4u) * 8u;
    let local_vertex_idx = (meshlet_triangles[word_idx] >> byte_shift) & 0xFFu;

    let vert_offset = meshlet.header.x;
    let vert_idx = meshlet_vertices[vert_offset + local_vertex_idx];

    // Step 4: pull position + element.
    let pos = vec3<f32>(
        positions[vert_idx * 3u],
        positions[vert_idx * 3u + 1u],
        positions[vert_idx * 3u + 2u],
    );
    let element = load_vertex_element(vert_idx);

    let normal = unpack_normal(element.normal);
    let tangent = unpack_normal(element.tangent);

    // Tangent sign stored in high byte of color_t_sign (0xFF = negative).
    let tangent_sign = select(1.0, -1.0,
        (element.color_t_sign & 0xFF000000u) != 0u);

    // Flip V to match WebGPU texture coordinate convention (same as Metal port).
    let uv = vec2<f32>(element.uv.x, 1.0 - element.uv.y);

    // Step 5: transforms.
    let world_pos = inst.world_matrix * vec4<f32>(pos, 1.0);
    let view_pos = uniforms.view_matrix * world_pos;
    out.position = uniforms.proj_matrix * view_pos;

    // Velocity: current vs previous clip position.
    out.current_clip = out.position;
    if (uniforms.has_prev_frame != 0u) {
        out.previous_clip = uniforms.prev_proj_matrix *
                            (uniforms.prev_view_matrix * world_pos);
    } else {
        out.previous_clip = out.current_clip;
    }

    // Build 3x3 for normal/tangent transform (upper-left of world_matrix).
    var normal_matrix: mat3x3<f32>;
    normal_matrix[0] = inst.world_matrix[0].xyz;
    normal_matrix[1] = inst.world_matrix[1].xyz;
    normal_matrix[2] = inst.world_matrix[2].xyz;

    var world_normal = normalize(normal_matrix * normal);
    var world_tangent = normalize(normal_matrix * tangent);

    // Gram-Schmidt orthogonalize; rebuild frame if near-parallel.
    if (abs(dot(world_tangent, world_normal)) > 0.999) {
        let up = select(vec3<f32>(0.0, 1.0, 0.0),
                        vec3<f32>(1.0, 0.0, 0.0),
                        abs(world_normal.y) < 0.999);
        world_tangent = normalize(cross(up, world_normal));
    } else {
        world_tangent = normalize(world_tangent -
                                  dot(world_tangent, world_normal) * world_normal);
    }
    let world_bitangent = cross(world_normal, world_tangent) * tangent_sign;

    out.uv = uv;
    out.normal = world_normal;
    out.tangent = world_tangent;
    out.bitangent = world_bitangent;
    out.material_id = material_id;
    out.meshlet_id = global_meshlet_index;
    out.triangle_id = vertex_id / 3u;
    out.mesh_id = instance_index;
    return out;
}

// ---- Fragment shader --------------------------------------------------------

struct FSOutput {
    @location(0) albedo: vec4<f32>,
    @location(1) normal: vec4<f32>,
    @location(2) orm: vec4<f32>,
    @location(3) velocity: vec2<f32>,
};

@fragment
fn gpu_driven_fragment_shader(in: VSOutput) -> FSOutput {
    var out: FSOutput;

    // Error sentinel: bright magenta. Visible against any valid scene color
    // so we can spot missing material data immediately.
    if (in.material_id == 0xFFFFFFFFu) {
        out.albedo    = vec4<f32>(1.0, 0.0, 1.0, 1.0); // MAGENTA = no material
        out.normal    = vec4<f32>(in.normal * 0.5 + 0.5, 1.0);
        out.orm       = vec4<f32>(1.0, 0.5, 0.0, 1.0);
        out.velocity  = vec2<f32>(0.0);
        return out;
    }

    let mat = load_material(in.material_id);
    let scaled_uv = fract(in.uv * mat.uv_scale);

    let albedo_array_size = textureNumLayers(albedo_textures);
    let normal_array_size = textureNumLayers(normal_textures);
    let orm_array_size    = textureNumLayers(orm_textures);

    // === Albedo ===
    var albedo_sample = vec4<f32>(1.0);
    if (mat.albedo_texture_idx != 0xFFFFFFFFu &&
        mat.albedo_texture_idx < albedo_array_size) {
        albedo_sample = textureSampleLevel(albedo_textures, texture_sampler,
                                           scaled_uv, mat.albedo_texture_idx, 0.0);
    } else {
        // Bad idx → RED so we can tell albedo binding is broken vs texture issue.
        albedo_sample = vec4<f32>(1.0, 0.0, 0.0, 1.0);
    }

    let base_color = albedo_sample.rgb * mat.albedo_tint;
    out.albedo = vec4<f32>(base_color, 1.0);

    // === Normal (tangent-space → world) ===
    var world_normal = in.normal;
    if (mat.normal_texture_idx != 0xFFFFFFFFu &&
        mat.normal_texture_idx < normal_array_size) {
        let normal_sample = textureSampleLevel(normal_textures, texture_sampler,
                                               scaled_uv, mat.normal_texture_idx, 0.0).rgb;
        if (length(normal_sample) > 0.1) {
            let tangent_normal = normal_sample * 2.0 - 1.0;
            let N = normalize(in.normal);
            let T = normalize(in.tangent);
            let B = normalize(in.bitangent);
            var tbn: mat3x3<f32>;
            tbn[0] = T;
            tbn[1] = B;
            tbn[2] = N;
            world_normal = normalize(tbn * tangent_normal);
        }
    }
    out.normal = vec4<f32>(world_normal * 0.5 + 0.5, 1.0);

    // === ORM ===
    var occlusion = 1.0;
    var roughness = max(mat.roughness_factor, 0.04);
    var metallic  = mat.metallic_factor;
    if (mat.orm_texture_idx != 0xFFFFFFFFu &&
        mat.orm_texture_idx < orm_array_size) {
        let orm_sample = textureSampleLevel(orm_textures, texture_sampler,
                                            scaled_uv, mat.orm_texture_idx, 0.0);
        occlusion = orm_sample.r;
        roughness = orm_sample.g;
        metallic  = orm_sample.b;
    }
    out.orm = vec4<f32>(occlusion, roughness, metallic, 1.0);

    // === Velocity ===
    if (uniforms.has_prev_frame != 0u) {
        let current_ndc  = in.current_clip.xy / in.current_clip.w;
        let previous_ndc = in.previous_clip.xy / in.previous_clip.w;
        out.velocity = (current_ndc - previous_ndc) * 0.5;
    } else {
        out.velocity = vec2<f32>(0.0);
    }

    // === Debug visualization override ===
    // Replaces albedo with a hashed color based on the selected ID source.
    // Normal/ORM are forced neutral so deferred lighting applies a flat shade
    // without washing out the per-id colors.
    if (uniforms.debug_mode != 0u) {
        // mode 1 → meshlet_id, mode 2 → triangle_id, mode 3 → mesh_id
        let id = select(in.meshlet_id,
                        select(in.triangle_id, in.mesh_id, uniforms.debug_mode == 3u),
                        uniforms.debug_mode != 1u);
        out.albedo = vec4<f32>(hash_id_to_color(id), 1.0);
        out.normal = vec4<f32>(0.5, 0.5, 1.0, 1.0);  // neutral, lit flat
        out.orm = vec4<f32>(1.0, 0.9, 0.0, 1.0);     // full AO, high roughness, non-metal
    }

    return out;
}
