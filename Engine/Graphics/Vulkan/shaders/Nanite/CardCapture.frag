#version 450 core
#extension GL_EXT_samplerless_texture_functions : require

#if defined(SC_CAPTURE_GEOMETRY_AUDIT_VARIANT) && !defined(SC_CAPTURE_SOURCE_AUDIT_VARIANT)
#error "SC_CAPTURE_GEOMETRY_AUDIT_VARIANT requires SC_CAPTURE_SOURCE_AUDIT_VARIANT"
#endif

// CardCapture.frag — T8.12 card-view capture pixel shader.
// Material sampling identical to PipelineDraw.frag (binding 9 material SSBO,
// 10 albedo texture2DArray, 11 sampler); outputs the three card-atlas pages:
//   RT0 albedo_atlas  rgba8  = base_color, alpha packed transport/validity
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
#ifdef SC_CAPTURE_SOURCE_AUDIT_VARIANT
    flat uint actual_cluster_id;
    flat uint actual_triangle_id;
#ifdef SC_CAPTURE_GEOMETRY_AUDIT_VARIANT
    flat vec3 geometry_triangle0_world;
    flat vec3 geometry_triangle1_world;
    flat vec3 geometry_triangle2_world;
    flat uint geometry_instance_index;
#endif
#endif
} IN;

layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out float out_axis_depth;
#ifdef SC_CAPTURE_SOURCE_AUDIT_VARIANT
layout(location = 3) out uvec4 out_source_tag;
// tag = (last_writer_frame, kind=1, global_cluster_id, local_triangle_id)
#ifdef SC_CAPTURE_GEOMETRY_AUDIT_VARIANT
layout(location = 4) out uvec4 out_geometry_point;
layout(location = 5) out uvec4 out_geometry_triangle0;
layout(location = 6) out uvec4 out_geometry_triangle1;
layout(location = 7) out uvec4 out_geometry_triangle2;
#endif
#endif

struct MaterialInfo {
    uint  albedo_texture_idx;
    vec3  albedo_tint;
    vec2  uv_scale;
    uint  flags;
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
    m.flags = mdb.mats[base + 11u];
    m.uv_scale = vec2(uintBitsToFloat(mdb.mats[base + 9u]),
                      uintBitsToFloat(mdb.mats[base + 10u]));
    return m;
}

// Preserve the long-standing alpha-validity contract (valid texels have
// alpha > 0.5) while carrying a compact transport payload for Surface Cache.
// bit7 = validity, bits[3:7) = transmission UNorm4, bits[0:3) = thickness
// UNorm3 reserved for the next transport phase (zero means default thickness).
uint encode_card_transport_alpha(uint flags) {
    uint transmission8 = (flags >> 8u) & 0xFFu;
    uint transmission4 = (transmission8 + 8u) / 17u;
    uint thickness8 = (flags >> 16u) & 0xFFu;
    uint thickness3 = (thickness8 + 18u) / 36u;
    return 0x80u | (transmission4 << 3u) | thickness3;
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
    if (IN.material_id >= uint(mdb.mats.length()) / 12u) discard;
    MaterialInfo mat = load_material(IN.material_id);
    if (any(isnan(mat.albedo_tint)) || any(isinf(mat.albedo_tint)) ||
        any(isnan(mat.uv_scale)) || any(isinf(mat.uv_scale)) ||
        any(lessThan(mat.albedo_tint, vec3(0.0)))) discard;
    vec2 scaled_uv = fract(IN.uv * mat.uv_scale);

    int albedo_layers = textureSize(albedo_textures, 0).z;
    // The main material path uses the tint for a constant material. An
    // invalid array index cannot create a measured neutral-gray surface.
    vec4 albedo_sample = vec4(1.0);
    if (mat.albedo_texture_idx != 0xFFFFFFFFu) {
        if (mat.albedo_texture_idx >= uint(albedo_layers)) discard;
        albedo_sample = texture(sampler2DArray(albedo_textures, texture_sampler),
                                vec3(scaled_uv, float(mat.albedo_texture_idx)));
    }
    vec3 base_color = albedo_sample.rgb * mat.albedo_tint;

    float axis_depth = dot(IN.world_pos - cb.center.xyz, cb.fwd.xyz);
    if (any(isnan(base_color)) || any(isinf(base_color)) ||
        any(isnan(IN.normal)) || any(isinf(IN.normal)) ||
        dot(IN.normal, IN.normal) < 1e-8 || isnan(axis_depth) ||
        isinf(axis_depth) || axis_depth < 0.0) discard;

    vec3 N = normalize(IN.normal);
    // Card projections mirror U for three faces. Raster winding therefore
    // cannot identify the physical side of this two-sided captured surface.
    if (dot(N, cb.fwd.xyz) > 0.0) N = -N;

    out_albedo = vec4(base_color,
                      float(encode_card_transport_alpha(mat.flags)) / 255.0);
    out_normal = vec4(octEncode(N), 0.0, 1.0);
    out_axis_depth = axis_depth;
#ifdef SC_CAPTURE_SOURCE_AUDIT_VARIANT
    // misc.z is intentionally carried as float in CardParams for the
    // existing capture ABI; preserve its exact frame bits in the audit MRT.
    out_source_tag = uvec4(floatBitsToUint(cb.misc.z), 1u,
                           IN.actual_cluster_id, IN.actual_triangle_id);
#ifdef SC_CAPTURE_GEOMETRY_AUDIT_VARIANT
    // Capture-time bit-preserving witness:
    //   RT4 = interpolated world point + writer frame
    //   RT5 = triangle corner 0 + actual cluster
    //   RT6 = triangle corner 1 + actual local triangle
    //   RT7 = triangle corner 2 + actual instance
    out_geometry_point = uvec4(floatBitsToUint(IN.world_pos.xyz),
                               floatBitsToUint(cb.misc.z));
    out_geometry_triangle0 = uvec4(floatBitsToUint(IN.geometry_triangle0_world.xyz),
                                   IN.actual_cluster_id);
    out_geometry_triangle1 = uvec4(floatBitsToUint(IN.geometry_triangle1_world.xyz),
                                   IN.actual_triangle_id);
    out_geometry_triangle2 = uvec4(floatBitsToUint(IN.geometry_triangle2_world.xyz),
                                   IN.geometry_instance_index);
#endif
#endif
}
