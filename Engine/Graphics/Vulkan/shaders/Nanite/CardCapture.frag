#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

// CardCapture.frag — T8.12 card-view capture pixel shader.
// Material sampling identical to PipelineDraw.frag (binding 9 material SSBO,
// 10 albedo texture2DArray, 11 sampler); outputs the three card-atlas pages:
//   RT0 albedo_atlas  rgba8  = base_color, alpha 1
//   RT1 normal_atlas  rg16f  = oct-encoded world normal (worldToCardUV consumers)
//   RT2 depth_atlas   r32f   = axis depth, dot(world_pos - card_center, fwd)
//     — same formula as SurfaceCacheCapture.comp::projectToCard's out_depth.

layout(set = 0, binding = 0) uniform CardParams {
    mat4  view;
    mat4  proj;
    vec4  center;
    vec4  fwd;
    vec4  misc;
} cb;

layout(set = 0, binding = 9)  readonly buffer MaterialDataBuffer { uint mats[]; } mdb;
layout(set = 0, binding = 10) uniform texture2DArray albedo_textures;
layout(set = 0, binding = 11) uniform sampler texture_sampler;

layout(location = 0) in VSOut {
    vec2 uv;
    vec3 normal;
    flat uint material_id;
    vec3 world_pos;
} IN;

layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out float out_axis_depth;

struct MaterialInfo {
    uint  albedo_texture_idx;
    vec3  albedo_tint;
};

MaterialInfo load_material(uint idx) {
    uint base = idx * 12u;
    MaterialInfo m;
    m.albedo_texture_idx = mdb.mats[base + 0u];
    m.albedo_tint = vec3(
        uintBitsToFloat(mdb.mats[base + 3u]),
        uintBitsToFloat(mdb.mats[base + 4u]),
        uintBitsToFloat(mdb.mats[base + 5u])
    );
    return m;
}

vec2 octEncode(vec3 n) {
    float l1norm = abs(n.x) + abs(n.y) + abs(n.z);
    vec2 result = n.xy / l1norm;
    if (n.z < 0.0) {
        result = (1.0 - abs(result.yx)) * mix(vec2(-1.0), vec2(1.0), step(vec2(0.0), result.xy));
    }
    return result * 0.5 + 0.5;
}

void main() {
    // Missing-material sentinel: neutral gray (never magenta — atlas texels
    // feed probes).
    MaterialInfo mat = load_material(IN.material_id);
    vec2 scaled_uv = fract(IN.uv * vec2(1.0));

    int albedo_layers = textureSize(albedo_textures, 0).z;
    vec4 albedo_sample = vec4(0.5);
    if (mat.albedo_texture_idx != 0xFFFFFFFFu && int(mat.albedo_texture_idx) < albedo_layers) {
        albedo_sample = texture(sampler2DArray(albedo_textures, texture_sampler),
                                vec3(scaled_uv, float(mat.albedo_texture_idx)));
    }
    vec3 base_color = albedo_sample.rgb * mat.albedo_tint;

    vec3 N = normalize(IN.normal);
    // Card projections mirror U for three faces. Raster winding therefore
    // cannot identify the physical side of this two-sided captured surface.
    if (dot(N, cb.fwd.xyz) > 0.0) N = -N;

    out_albedo = vec4(base_color, 1.0);
    out_normal = vec4(octEncode(N), 0.0, 1.0);
    out_axis_depth = dot(IN.world_pos - cb.center.xyz, cb.fwd.xyz);
}
