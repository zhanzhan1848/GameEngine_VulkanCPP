#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

// PipelineDraw.frag — GLSL fragment shader for GPU-driven meshlet rendering.
// Outputs GBuffer MRT (albedo/normal/orm/velocity) for deferred lighting.
//
// NOTE: Bindings 10/11/12 are separate OpTypeImage (matching WGSL output and
// the C++ SampledImage descriptor type), NOT combined sampler2DArray. We use
// GL_EXT_samplerless_texture_functions to sample with an explicit sampler.

// Re-declare UBO for debug_mode and has_prev_frame access.
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

layout(set = 0, binding = 9)  readonly buffer MaterialDataBuffer { uint mats[]; } mdb;
layout(set = 0, binding = 10) uniform texture2DArray albedo_textures;
layout(set = 0, binding = 11) uniform texture2DArray normal_textures;
layout(set = 0, binding = 12) uniform texture2DArray orm_textures;
layout(set = 0, binding = 13) uniform sampler texture_sampler;

layout(location = 0) in VSOut {
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
} IN;

// GBuffer MRT outputs.
layout(location = 0) out vec4 out_albedo;    // BGRA8
layout(location = 1) out vec4 out_normal;    // BGRA8
layout(location = 2) out vec4 out_orm;       // BGRA8
layout(location = 3) out vec2 out_velocity;  // RG16F

// MaterialData: 48 bytes = 12 u32s per material.
struct MaterialInfo {
    uint  albedo_texture_idx;
    uint  normal_texture_idx;
    uint  orm_texture_idx;
    vec3  albedo_tint;
    float metallic_factor;
    float roughness_factor;
    float normal_scale;
    vec2  uv_scale;
    uint  flags;
};

MaterialInfo load_material(uint idx) {
    uint base = idx * 12u;
    MaterialInfo m;
    m.albedo_texture_idx  = mdb.mats[base + 0u];
    m.normal_texture_idx  = mdb.mats[base + 1u];
    m.orm_texture_idx     = mdb.mats[base + 2u];
    m.albedo_tint = vec3(
        uintBitsToFloat(mdb.mats[base + 3u]),
        uintBitsToFloat(mdb.mats[base + 4u]),
        uintBitsToFloat(mdb.mats[base + 5u])
    );
    m.metallic_factor   = uintBitsToFloat(mdb.mats[base + 6u]);
    m.roughness_factor  = uintBitsToFloat(mdb.mats[base + 7u]);
    m.normal_scale      = uintBitsToFloat(mdb.mats[base + 8u]);
    m.uv_scale = vec2(
        uintBitsToFloat(mdb.mats[base + 9u]),
        uintBitsToFloat(mdb.mats[base + 10u])
    );
    m.flags = mdb.mats[base + 11u];
    return m;
}

// Hash u32 → vec3 color for debug visualization.
vec3 hash_id_to_color(uint id) {
    uint h = id * 2654435761u;
    h = h ^ (h >> 16u);
    h = h * 0x85ebca6bu;
    h = h ^ (h >> 13u);
    h = h * 0xc2b2ae35u;
    h = h ^ (h >> 16u);
    return vec3(
        float(h & 0xFFu) / 255.0,
        float((h >> 8u) & 0xFFu) / 255.0,
        float((h >> 16u) & 0xFFu) / 255.0
    );
}

void main() {
    // Error sentinel: bright magenta for missing material.
    if (IN.material_id == 0xFFFFFFFFu) {
        out_albedo   = vec4(1.0, 0.0, 1.0, 1.0);
        out_normal   = vec4(IN.normal * 0.5 + 0.5, 1.0);
        out_orm      = vec4(1.0, 0.5, 0.0, 1.0);
        out_velocity = vec2(0.0);
        return;
    }

    // Back-face normal flip (CullMode::None).
    vec3 N = normalize(IN.normal);
    if (!gl_FrontFacing) N = -N;
    vec3 T = normalize(IN.tangent);
    vec3 B = normalize(cross(N, T));

    MaterialInfo mat = load_material(IN.material_id);
    vec2 scaled_uv = fract(IN.uv * mat.uv_scale);

    int albedo_layers = textureSize(albedo_textures, 0).z;
    int normal_layers = textureSize(normal_textures, 0).z;
    int orm_layers    = textureSize(orm_textures, 0).z;

    // === Albedo ===
    vec4 albedo_sample = vec4(1.0);
    if (mat.albedo_texture_idx != 0xFFFFFFFFu && int(mat.albedo_texture_idx) < albedo_layers) {
        albedo_sample = texture(sampler2DArray(albedo_textures, texture_sampler),
                                vec3(scaled_uv, float(mat.albedo_texture_idx)));
    } else {
        albedo_sample = vec4(1.0, 0.0, 0.0, 1.0);
    }
    vec3 base_color = albedo_sample.rgb * mat.albedo_tint;
    out_albedo = vec4(base_color, 1.0);

    // Normal from vertex shader (with back-face flip).
    out_normal = vec4(N * 0.5 + 0.5, 1.0);

    // === ORM ===
    float occlusion = 1.0;
    float roughness = max(mat.roughness_factor, 0.04);
    float metallic  = mat.metallic_factor;
    if (mat.orm_texture_idx != 0xFFFFFFFFu && int(mat.orm_texture_idx) < orm_layers) {
        vec3 orm_sample = texture(sampler2DArray(orm_textures, texture_sampler),
                                  vec3(scaled_uv, float(mat.orm_texture_idx))).rgb;
        occlusion = orm_sample.r;
        roughness = orm_sample.g;
        metallic  = orm_sample.b;
    }
    out_orm = vec4(occlusion, roughness, metallic, 1.0);

    // === Velocity ===
    out_velocity = vec2(0.0);
}
