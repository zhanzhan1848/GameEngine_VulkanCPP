#ifndef LUMEN_DDGI_FIRST_NATIVE_NORMAL_GLSL
#define LUMEN_DDGI_FIRST_NATIVE_NORMAL_GLSL

#ifndef DDGI_FIRST_NATIVE_LENGTH
#define DDGI_FIRST_NATIVE_LENGTH length
#endif

// This file is included only by the DDGI_FIRST_SURFACE_VARIANT child.  It is
// deliberately a diagnostic source proof: it never changes the parent
// first-surface distance or any SC query.  The only production value it may
// replace is the incumbent HN for a finite, same-first-hit, well-conditioned
// normal reconstruction.

const float DDGI_FIRST_NATIVE_F32_EPS = 1.1920928955078125e-7;
const float DDGI_FIRST_NATIVE_NORMAL_GATE = 2.0e-5;
const float DDGI_FIRST_NATIVE_QUAD_GATE = 2.0e-3;

// Result status is intentionally small and stable in the readback contract.
// 0 unknown/unsupported, 1 accepted native N, 2 parent first-surface reject,
// 3 incumbent HN already valid, 4 source/identity guard, 5 conditioning,
// 6 material guard.  A status other than 1 never changes HN.
const uint DDGI_FIRST_NATIVE_UNKNOWN = 0u;
const uint DDGI_FIRST_NATIVE_ACCEPTED = 1u;
const uint DDGI_FIRST_NATIVE_PARENT_REJECT = 2u;
const uint DDGI_FIRST_NATIVE_OLD_HN = 3u;
const uint DDGI_FIRST_NATIVE_SOURCE_GUARD = 4u;
const uint DDGI_FIRST_NATIVE_CONDITIONING = 5u;
const uint DDGI_FIRST_NATIVE_MATERIAL = 6u;

// Bits in record 20.w.  Every failed reason has its own bit; a report may
// therefore retain overlapping guard failures without inventing a fallback.
const uint DDGI_FIRST_NATIVE_BIT_PARENT = 1u << 0u;
const uint DDGI_FIRST_NATIVE_BIT_OLD_HN = 1u << 1u;
const uint DDGI_FIRST_NATIVE_BIT_TRIANGLE = 1u << 2u;
const uint DDGI_FIRST_NATIVE_BIT_SOURCE = 1u << 3u;
const uint DDGI_FIRST_NATIVE_BIT_POINT = 1u << 4u;
const uint DDGI_FIRST_NATIVE_BIT_BARY = 1u << 5u;
const uint DDGI_FIRST_NATIVE_BIT_IDENTITY = 1u << 6u;
const uint DDGI_FIRST_NATIVE_BIT_MATERIAL = 1u << 7u;
const uint DDGI_FIRST_NATIVE_BIT_NORMAL = 1u << 8u;
const uint DDGI_FIRST_NATIVE_BIT_TANGENT = 1u << 9u;
const uint DDGI_FIRST_NATIVE_BIT_TEXTURE = 1u << 10u;
const uint DDGI_FIRST_NATIVE_BIT_CONDITIONING = 1u << 11u;
const uint DDGI_FIRST_NATIVE_BIT_ACCEPTED = 1u << 31u;

struct DDGIFirstNativeNormalResult {
    uint status;
    uint status_bits;
    uint parent_status;
    uint material_id;
    uint material_flags;
    uint instance_id;
    uint source_status;
    uint point_guard;
    uint instance_guard;
    uint path_code;
    uvec3 vertex_ids;
    vec3 world_point;
    vec3 bary_native_n;
    float bary_native_length;
    vec3 ray_native_n;
    vec3 ray_geom_n;
    float geom_length;
    vec3 vertex_n0;
    vec3 vertex_n1;
    vec3 vertex_n2;
    uint packed_n0;
    uint packed_n1;
    uint packed_n2;
    vec3 raw_geom_n;
    float flip_geom;
    vec2 uv;
    vec2 scaled_uv;
    vec3 tangent;
    uint normal_layer;
    vec4 normal_sample;
    vec4 texel00;
    vec4 texel10;
    vec4 texel01;
    vec4 texel11;
    vec3 mapped_n;
    vec3 tangent_normal;
    float normal_scale;
    vec2 base_texel;
    float flip_native;
    float n_bound;
    float t_bound;
    float mapped_bound;
    vec3 before_hn;
    vec3 after_hn;
    float old_hn_valid;
};

float ddgiFirstNativeGamma(uint n) {
    float ne = float(n) * DDGI_FIRST_NATIVE_F32_EPS;
    return ne / max(1.0 - ne, 0.5);
}

bool ddgiFirstNativeFinite4(vec4 v) {
    return ddgiFirstFinite(v.x) && ddgiFirstFinite(v.y) &&
           ddgiFirstFinite(v.z) && ddgiFirstFinite(v.w);
}

bool ddgiFirstNativeExactAxis(uint packed) {
    uint hi = (packed >> 16u) & 0xffffu;
    uint lo = packed & 0xffffu;
    bool xAxis = (hi == 0u || hi == 65535u) &&
                 (lo == 32767u || lo == 32768u);
    bool yAxis = (lo == 0u || lo == 65535u) &&
                 (hi == 32767u || hi == 32768u);
    return xAxis || yAxis;
}

// The packed unit-circle boundary is the one numerically ill-conditioned
// branch.  Exact axis encodings are accepted explicitly; a non-axis value in
// the uncertainty interval is UNKNOWN.  Values demonstrably outside the
// circle keep the native decoder's +Z fallback.
vec3 ddgiFirstNativeUnpack(uint packed, uint color_t_sign,
                           out float error_bound, out uint path) {
    float hi = float((packed >> 16u) & 0xffffu);
    float lo = float(packed & 0xffffu);
    float inv = 2.0 / 65535.0;
    float fx = hi * inv - 1.0;
    float fy = lo * inv - 1.0;
    float d = fx * fx + fy * fy;
    float d_error = ddgiFirstNativeGamma(8u) * max(abs(d), 1.0) +
                    4.0 * DDGI_FIRST_NATIVE_F32_EPS;
    bool exact_axis = ddgiFirstNativeExactAxis(packed);
    if (d > 1.0 + d_error) {
        error_bound = 8.0 * DDGI_FIRST_NATIVE_F32_EPS;
        path = 2u; // stable native +Z fallback
        return vec3(0.0, 0.0, 1.0);
    }
    if (!exact_axis && abs(d - 1.0) <= d_error) {
        error_bound = 3.4e38;
        path = 0u; // unit-circle conditioning is not certified
        return vec3(0.0);
    }
    float z2 = max(0.0, 1.0 - d);
    float z = sqrt(z2);
    float z_error = exact_axis ? 4.0 * DDGI_FIRST_NATIVE_F32_EPS :
        d_error / max(2.0 * z, 1.0e-5);
    error_bound = min(1.0, ddgiFirstNativeGamma(8u) + z_error +
                              4.0 * DDGI_FIRST_NATIVE_F32_EPS);
    uint signs = (color_t_sign >> 24u) & 0xffu;
    float n_sign = float(signs & 0x02u) - 1.0;
    path = exact_axis ? 3u : 1u;
    return vec3(fx, fy, z * n_sign);
}

bool ddgiFirstNativeNormalize(vec3 value, float input_error,
                              out vec3 unit_value, out float output_error) {
    float len = DDGI_FIRST_NATIVE_LENGTH(value);
    if (!ddgiFirstFiniteVec3(value) || !ddgiFirstFinite(len) || len <= 1.0e-12)
        return false;
    float safe = len - input_error;
    if (safe <= 1.0e-6) return false;
    unit_value = normalize(value);
    output_error = min(1.0, input_error / safe + ddgiFirstNativeGamma(8u));
    return ddgiFirstFiniteVec3(unit_value) && ddgiFirstFinite(output_error);
}

bool ddgiFirstNativeIdentity(mat4 m) {
    for (uint c = 0u; c < 4u; ++c) {
        for (uint r = 0u; r < 4u; ++r) {
            float expected = (c == r) ? 1.0 : 0.0;
            if (!ddgiFirstFinite(m[c][r]) || m[c][r] != expected) return false;
        }
    }
    return true;
}

bool ddgiFirstNativeLoadPosition(uint id, out vec3 position) {
    uint count = first_native_positions.positions.length() / 3u;
    if (id >= count) return false;
    position = vec3(first_native_positions.positions[id * 3u + 0u],
                    first_native_positions.positions[id * 3u + 1u],
                    first_native_positions.positions[id * 3u + 2u]);
    return ddgiFirstFiniteVec3(position);
}

bool ddgiFirstNativeLoadElement(uint id, out uint color_sign,
                                out uint packed_normal, out uint packed_tangent,
                                out vec2 uv) {
    uint count = first_native_elements.elements.length() / 6u;
    if (id >= count) return false;
    uint base = id * 6u;
    color_sign = first_native_elements.elements[base + 0u];
    packed_normal = first_native_elements.elements[base + 1u];
    packed_tangent = first_native_elements.elements[base + 2u];
    uv = vec2(uintBitsToFloat(first_native_elements.elements[base + 4u]),
              1.0 - uintBitsToFloat(first_native_elements.elements[base + 5u]));
    return ddgiFirstNativeFinite4(vec4(uv, 0.0, 0.0));
}

bool ddgiFirstNativeWorldFrame(mat4 world, vec3 object_n, float object_n_error,
                               vec3 object_t, float object_t_error,
                               out vec3 world_n, out float n_error,
                               out vec3 world_t, out float t_error) {
    mat3 normal_matrix = mat3(world[0].xyz, world[1].xyz, world[2].xyz);
    vec3 n_raw = normal_matrix * object_n;
    vec3 t_raw = normal_matrix * object_t;
    if (!ddgiFirstNativeNormalize(n_raw,
                                  object_n_error + ddgiFirstNativeGamma(8u),
                                  world_n, n_error) ||
        !ddgiFirstNativeNormalize(t_raw,
                                  object_t_error + ddgiFirstNativeGamma(8u),
                                  world_t, t_error)) return false;
    float d = dot(world_t, world_n);
    if (!ddgiFirstFinite(d)) return false;
    float dot_error = n_error + t_error + ddgiFirstNativeGamma(6u);
    if (abs(abs(d) - 0.999) <= dot_error) return false;
    if (abs(d) > 0.999) {
        // The fallback-up branch is itself a sign/conditioning decision.  A
        // normal whose y pole is within its propagated uncertainty cannot be
        // certified by comparing only the nominal value.
        if (abs(abs(world_n.y) - 0.999) <=
            n_error + ddgiFirstNativeGamma(6u)) return false;
#if defined(DDGI_FIRST_NATIVE_PRIMARY_FRAME) && DDGI_FIRST_NATIVE_PRIMARY_FRAME
        // Private diagnostic variant matches actual WGSL select(upY,upX,cond).
        vec3 up = (abs(world_n.y) < 0.999) ? vec3(1.0, 0.0, 0.0) :
                                              vec3(0.0, 1.0, 0.0);
#else
        vec3 up = (abs(world_n.y) < 0.999) ? vec3(0.0, 1.0, 0.0) :
                                              vec3(1.0, 0.0, 0.0);
#endif
        vec3 rebuilt = cross(up, world_n);
        if (!ddgiFirstNativeNormalize(rebuilt, n_error + ddgiFirstNativeGamma(6u),
                                      world_t, t_error)) return false;
    } else {
        vec3 projected = world_t - d * world_n;
        float projected_error = t_error + abs(d) * n_error +
                                dot_error + ddgiFirstNativeGamma(8u);
        if (!ddgiFirstNativeNormalize(projected, projected_error,
                                      world_t, t_error)) return false;
    }
    return true;
}

void ddgiFirstNativeClear(inout DDGIFirstNativeNormalResult result) {
    result.status = DDGI_FIRST_NATIVE_UNKNOWN;
    result.status_bits = 0u;
    result.parent_status = 0u;
    result.material_id = 0xffffffffu;
    result.material_flags = 0xffffffffu;
    result.instance_id = 0xffffffffu;
    result.source_status = 0u;
    result.point_guard = 0u;
    result.instance_guard = 0u;
    result.path_code = 0u;
    result.vertex_ids = uvec3(0xffffffffu);
    result.world_point = vec3(0.0);
    result.bary_native_n = vec3(0.0);
    result.bary_native_length = 0.0;
    result.ray_native_n = vec3(0.0);
    result.ray_geom_n = vec3(0.0);
    result.geom_length = 0.0;
    result.vertex_n0 = vec3(0.0);
    result.vertex_n1 = vec3(0.0);
    result.vertex_n2 = vec3(0.0);
    result.packed_n0 = 0u;
    result.packed_n1 = 0u;
    result.packed_n2 = 0u;
    result.raw_geom_n = vec3(0.0);
    result.flip_geom = 0.0;
    result.uv = vec2(0.0);
    result.scaled_uv = vec2(0.0);
    result.tangent = vec3(0.0);
    result.normal_layer = 0xffffffffu;
    result.normal_sample = vec4(0.0);
    result.texel00 = vec4(0.0);
    result.texel10 = vec4(0.0);
    result.texel01 = vec4(0.0);
    result.texel11 = vec4(0.0);
    result.mapped_n = vec3(0.0);
    result.tangent_normal = vec3(0.0);
    result.normal_scale = 0.0;
    result.base_texel = vec2(0.0);
    result.flip_native = 0.0;
    result.n_bound = 3.4e38;
    result.t_bound = 3.4e38;
    result.mapped_bound = 3.4e38;
    result.before_hn = vec3(0.0);
    result.after_hn = vec3(0.0);
    result.old_hn_valid = 0.0;
}

// Reconstruct the native vertex/normal-map source selected by the parent hit.
// The result carries all fields needed by the 24-record witness, including
// bounds that the host replays independently.  No first-surface distance is
// changed here.
DDGIFirstNativeNormalResult ddgiFirstNativeEvaluate(
    DDGIFirstSurfaceHit first, vec3 probe_pos, vec3 ray_dir,
    vec3 old_hn, bool old_hn_valid) {
    DDGIFirstNativeNormalResult result;
    ddgiFirstNativeClear(result);
    result.parent_status = first.status;
    // Preserve even an invalid incumbent's raw xyz; Finalize may ignore it,
    // but a rejected candidate must still record the actual unchanged HN.
    result.before_hn = old_hn;
    result.after_hn = result.before_hn;
    result.old_hn_valid = old_hn_valid ? 1.0 : 0.0;
    if (old_hn_valid) {
        result.status = DDGI_FIRST_NATIVE_OLD_HN;
        result.status_bits = DDGI_FIRST_NATIVE_BIT_OLD_HN;
        return result;
    }
    if (first.status != 1u) {
        result.status = DDGI_FIRST_NATIVE_PARENT_REJECT;
        result.status_bits = DDGI_FIRST_NATIVE_BIT_PARENT;
        return result;
    }
    if (first.flags != 0u || !ddgiFirstFinite(first.t) || first.t <= 2.0e-5) {
        result.status = DDGI_FIRST_NATIVE_MATERIAL;
        result.status_bits = DDGI_FIRST_NATIVE_BIT_MATERIAL;
        return result;
    }
    result.status_bits = 0u;
#if defined(DDGI_FIRST_NATIVE_DIAGNOSTIC_CONTINUE_BOUNDS) && DDGI_FIRST_NATIVE_DIAGNOSTIC_CONTINUE_BOUNDS
    bool diagnostic_bound_rejected = false;
#endif

    float tri_f = first.triangle;
    if (!ddgiFirstFinite(tri_f) || tri_f < 0.0 || tri_f != floor(tri_f)) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_TRIANGLE;
        return result;
    }
    uint tri_id = uint(tri_f);
    // The trace child binds the compact 16-byte uvec4 mapping.  The
    // read-only first-record proof binds the already-certified 32-byte
    // analytic-rho mapping (map0 = native ids/instance, map1 = material and
    // source metadata).  Keep the incumbent expression and ABI byte-for-byte
    // on the default path; only the proof opts into this adapter.
#if defined(DDGI_FIRST_NATIVE_NORMAL_MAPPING_STRIDE32) && DDGI_FIRST_NATIVE_NORMAL_MAPPING_STRIDE32
    const uint mappingBase = tri_id * 2u;
    if (mappingBase + 1u >= first_native_triangle_source.triangle_source.length()) {
#else
    if (tri_id >= first_native_triangle_source.triangle_source.length()) {
#endif
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_TRIANGLE;
        return result;
    }
#if defined(DDGI_FIRST_NATIVE_NORMAL_MAPPING_STRIDE32) && DDGI_FIRST_NATIVE_NORMAL_MAPPING_STRIDE32
    uvec4 source = first_native_triangle_source.triangle_source[mappingBase];
    uvec4 mapping_meta = first_native_triangle_source.triangle_source[mappingBase + 1u];
#else
    uvec4 source = first_native_triangle_source.triangle_source[tri_id];
#endif
    result.vertex_ids = source.xyz;
    result.instance_id = source.w;
    result.source_status = 1u;

    uint pos_count = first_native_positions.positions.length() / 3u;
    uint element_count = first_native_elements.elements.length() / 6u;
    uint instance_count = first_native_instances.instances.length();
    if (any(greaterThanEqual(source.xyz, uvec3(pos_count))) ||
        any(greaterThanEqual(source.xyz, uvec3(element_count))) ||
        source.w >= instance_count) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_SOURCE;
        return result;
    }
    DDGIFirstNativeInstance instance = first_native_instances.instances[source.w];
    result.instance_id = source.w;
    result.instance_guard = 0u;
    if (!ddgiFirstNativeIdentity(instance.world_matrix)) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_IDENTITY;
        result.instance_guard = 1u;
        return result;
    }
    result.instance_guard = 1u;

    // source_mesh is the first-surface scene mesh index.  The native mapping
    // stores the runtime instance index in source.w; duplicate mesh assets
    // may legitimately use different instance indices, so these identities
    // must not be conflated in the child guard.
    if (!ddgiFirstFinite(first.source_mesh) || first.source_mesh < 0.0 ||
        first.source_mesh != floor(first.source_mesh)) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_SOURCE;
        return result;
    }
#if defined(DDGI_FIRST_NATIVE_NORMAL_MAPPING_STRIDE32) && DDGI_FIRST_NATIVE_NORMAL_MAPPING_STRIDE32
    // map1.y is the scene mesh index carried by the 32-byte mapping.  It is
    // checked against the actual BVH triangle payload, while map1.x/z and
    // map1.w are checked against the resident material table below.  A
    // mismatch leaves this row UNKNOWN instead of borrowing a nearby source.
    if (mapping_meta.y != uint(first.source_mesh)) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_IDENTITY;
        return result;
    }
#endif
    vec3 p0, p1, p2;
    if (!ddgiFirstNativeLoadPosition(source.x, p0) ||
        !ddgiFirstNativeLoadPosition(source.y, p1) ||
        !ddgiFirstNativeLoadPosition(source.z, p2)) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_SOURCE;
        return result;
    }
    vec3 source_e1 = p1 - p0;
    vec3 source_e2 = p2 - p0;
    if (!ddgiFirstFiniteVec3(first.source_a) ||
        !ddgiFirstFiniteVec3(first.source_e1) ||
        !ddgiFirstFiniteVec3(first.source_e2) ||
        max(max(DDGI_FIRST_NATIVE_LENGTH(first.source_a - p0), DDGI_FIRST_NATIVE_LENGTH(first.source_e1 - source_e1)),
            DDGI_FIRST_NATIVE_LENGTH(first.source_e2 - source_e2)) > DDGI_FIRST_NATIVE_NORMAL_GATE) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_SOURCE;
        return result;
    }

    vec3 bary = first.bary;
    if (!ddgiFirstFiniteVec3(bary) || any(lessThan(bary, vec3(-2.0e-5))) ||
        any(greaterThan(bary, vec3(1.0 + 2.0e-5))) ||
        abs(bary.x + bary.y + bary.z - 1.0) > 2.0e-5) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_BARY;
        return result;
    }
    vec3 point = p0 + bary.y * source_e1 + bary.z * source_e2;
    vec3 ray_point = probe_pos + ray_dir * first.t;
    result.world_point = point;
    result.point_guard = 1u;
    if (!ddgiFirstFiniteVec3(point) || !ddgiFirstFiniteVec3(ray_point) ||
        DDGI_FIRST_NATIVE_LENGTH(point - ray_point) > DDGI_FIRST_NATIVE_NORMAL_GATE) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_POINT;
        result.point_guard = 0u;
        return result;
    }

    uint c0, c1, c2, pn0, pn1, pn2, pt0, pt1, pt2;
    vec2 uv0, uv1, uv2;
    if (!ddgiFirstNativeLoadElement(source.x, c0, pn0, pt0, uv0) ||
        !ddgiFirstNativeLoadElement(source.y, c1, pn1, pt1, uv1) ||
        !ddgiFirstNativeLoadElement(source.z, c2, pn2, pt2, uv2)) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_SOURCE;
        return result;
    }
    uint material_count = first_native_materials.material_words.length() / 12u;
    uint material_id = instance.meta0.y;
#if defined(DDGI_FIRST_NATIVE_NORMAL_MAPPING_STRIDE32) && DDGI_FIRST_NATIVE_NORMAL_MAPPING_STRIDE32
    if (mapping_meta.x != material_id || mapping_meta.z != material_id ||
        mapping_meta.w != 0u) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_MATERIAL;
        return result;
    }
#endif
    if (material_id >= material_count) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_MATERIAL;
        return result;
    }
    uint material_base = material_id * 12u;
    uint material_flags = first_native_materials.material_words[material_base + 11u];
    result.material_id = material_id;
    result.material_flags = material_flags;
    result.normal_layer = first_native_materials.material_words[material_base + 1u];
    result.normal_scale = uintBitsToFloat(first_native_materials.material_words[material_base + 8u]);
    vec2 uv_scale = vec2(uintBitsToFloat(first_native_materials.material_words[material_base + 9u]),
                         uintBitsToFloat(first_native_materials.material_words[material_base + 10u]));
    if (material_flags != 0u || first.flags != 0u ||
        !ddgiFirstFinite(result.normal_scale) || !ddgiFirstNativeFinite4(vec4(uv_scale, 0.0, 0.0))) {
        result.status = DDGI_FIRST_NATIVE_MATERIAL;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_MATERIAL;
        return result;
    }

    float eb0, eb1, eb2, et0, et1, et2;
    uint pb0, pb1, pb2, ptb0, ptb1, ptb2;
    vec3 on0 = ddgiFirstNativeUnpack(pn0, c0, eb0, pb0);
    vec3 on1 = ddgiFirstNativeUnpack(pn1, c1, eb1, pb1);
    vec3 on2 = ddgiFirstNativeUnpack(pn2, c2, eb2, pb2);
    vec3 ot0 = ddgiFirstNativeUnpack(pt0, c0, et0, ptb0);
    vec3 ot1 = ddgiFirstNativeUnpack(pt1, c1, et1, ptb1);
    vec3 ot2 = ddgiFirstNativeUnpack(pt2, c2, et2, ptb2);
    if (pb0 == 0u || pb1 == 0u || pb2 == 0u || ptb0 == 0u || ptb1 == 0u || ptb2 == 0u) {
        result.status = DDGI_FIRST_NATIVE_CONDITIONING;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_NORMAL | DDGI_FIRST_NATIVE_BIT_TANGENT;
        return result;
    }
    vec3 wn0, wn1, wn2, wt0, wt1, wt2;
    float en0, en1, en2, etn0, etn1, etn2;
    if (!ddgiFirstNativeWorldFrame(instance.world_matrix, on0, eb0, ot0, et0, wn0, en0, wt0, etn0) ||
        !ddgiFirstNativeWorldFrame(instance.world_matrix, on1, eb1, ot1, et1, wn1, en1, wt1, etn1) ||
        !ddgiFirstNativeWorldFrame(instance.world_matrix, on2, eb2, ot2, et2, wn2, en2, wt2, etn2)) {
        result.status = DDGI_FIRST_NATIVE_CONDITIONING;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_NORMAL | DDGI_FIRST_NATIVE_BIT_TANGENT;
        return result;
    }
    result.vertex_n0 = wn0;
    result.vertex_n1 = wn1;
    result.vertex_n2 = wn2;
    result.packed_n0 = pn0;
    result.packed_n1 = pn1;
    result.packed_n2 = pn2;
    vec3 native_raw = bary.x * wn0 + bary.y * wn1 + bary.z * wn2;
    float native_error = bary.x * en0 + bary.y * en1 + bary.z * en2 +
                         ddgiFirstNativeGamma(8u);
    vec3 native_n;
    float native_bound;
    if (!ddgiFirstNativeNormalize(native_raw, native_error, native_n, native_bound)) {
        result.status = DDGI_FIRST_NATIVE_CONDITIONING;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_NORMAL;
        return result;
    }
    vec3 tangent_raw = bary.x * wt0 + bary.y * wt1 + bary.z * wt2;
    float tangent_error = bary.x * etn0 + bary.y * etn1 + bary.z * etn2 +
                          ddgiFirstNativeGamma(8u);
    vec3 tangent;
    float tangent_bound;
    if (!ddgiFirstNativeNormalize(tangent_raw, tangent_error, tangent, tangent_bound)) {
        result.status = DDGI_FIRST_NATIVE_CONDITIONING;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_TANGENT;
        return result;
    }
    vec3 edge1 = p1 - p0;
    vec3 edge2 = p2 - p0;
    vec3 geom_raw = cross(edge1, edge2);
    float geom_length = DDGI_FIRST_NATIVE_LENGTH(geom_raw);
    if (!ddgiFirstFiniteVec3(geom_raw) || !ddgiFirstFinite(geom_length) || geom_length <= 1.0e-12) {
        result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_SOURCE;
        return result;
    }
    vec3 geom_n;
    float geom_bound;
    float edge_scale = DDGI_FIRST_NATIVE_LENGTH(edge1) * DDGI_FIRST_NATIVE_LENGTH(edge2) +
                       DDGI_FIRST_NATIVE_LENGTH(edge1) + DDGI_FIRST_NATIVE_LENGTH(edge2);
    float geom_input_error = ddgiFirstNativeGamma(8u) *
                             max(edge_scale, 1.0e-6);
    if (!ddgiFirstNativeNormalize(geom_raw, geom_input_error,
                                  geom_n, geom_bound)) {
        result.status = DDGI_FIRST_NATIVE_CONDITIONING;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_NORMAL;
        return result;
    }
    // Preserve the existing absolute geometry/prelength gate.  A relative
    // triangle normal bound is insufficient for a large cross product whose
    // source point/edge arithmetic already exceeds the 2e-5 evidence budget.
    if (geom_input_error > DDGI_FIRST_NATIVE_NORMAL_GATE ||
        geom_bound > DDGI_FIRST_NATIVE_NORMAL_GATE) {
        result.status = DDGI_FIRST_NATIVE_CONDITIONING;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_NORMAL;
#if defined(DDGI_FIRST_NATIVE_DIAGNOSTIC_CONTINUE_BOUNDS) && DDGI_FIRST_NATIVE_DIAGNOSTIC_CONTINUE_BOUNDS
        // Private proof output only: keep the original rejection and its
        // bound, but record later native/TBN/texture values for independent
        // conditioning analysis. Source/point/material/branch guards still
        // return immediately; this is not an alternate acceptance budget.
        diagnostic_bound_rejected = true;
#else
        return result;
#endif
    }
    float geom_dot = dot(geom_n, ray_dir);
    float native_dot = dot(native_n, ray_dir);
    float facing_error = native_bound + geom_bound +
                         ddgiFirstNativeGamma(8u);
    if (!ddgiFirstFinite(geom_dot) || !ddgiFirstFinite(native_dot) ||
        abs(geom_dot) <= facing_error || abs(native_dot) <= facing_error) {
        result.status = DDGI_FIRST_NATIVE_CONDITIONING;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_CONDITIONING;
        return result;
    }
    float flip_native = native_dot > 0.0 ? -1.0 : 1.0;
    float flip_geom = geom_dot > 0.0 ? -1.0 : 1.0;
    vec3 ray_native = native_n * flip_native;
    vec3 ray_geom = geom_n * flip_geom;
    float align_dot = dot(ray_native, ray_geom);
    float align_error = native_bound + geom_bound +
                        ddgiFirstNativeGamma(8u);
    if (!ddgiFirstFinite(align_dot) || abs(align_dot) <= align_error) {
        result.status = DDGI_FIRST_NATIVE_CONDITIONING;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_CONDITIONING;
        return result;
    }
    if (align_dot < 0.0) ray_native = -ray_native;
    vec3 bitangent_raw = cross(ray_native, tangent);
    float bitangent_error = native_bound + tangent_bound + ddgiFirstNativeGamma(8u);
    vec3 bitangent;
    float bitangent_bound;
    if (!ddgiFirstNativeNormalize(bitangent_raw, bitangent_error,
                                  bitangent, bitangent_bound)) {
        result.status = DDGI_FIRST_NATIVE_CONDITIONING;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_CONDITIONING;
        return result;
    }

    result.bary_native_n = native_n;
    result.bary_native_length = DDGI_FIRST_NATIVE_LENGTH(native_raw);
    result.ray_native_n = ray_native;
    result.ray_geom_n = ray_geom;
    result.geom_length = geom_length;
    result.raw_geom_n = geom_n;
    result.flip_geom = flip_geom;
    result.tangent = tangent;
    result.uv = bary.x * uv0 + bary.y * uv1 + bary.z * uv2;
    result.scaled_uv = fract(result.uv * uv_scale);
    result.flip_native = flip_native;
    result.n_bound = native_bound;
    result.t_bound = tangent_bound;
    result.mapped_n = ray_native;
    result.mapped_bound = native_bound;
    result.path_code = 0u;

    ivec3 extent = textureSize(sampler2DArray(first_native_normal_textures,
                                               first_native_normal_sampler), 0);
    if (result.normal_layer != 0xffffffffu) {
        if (result.normal_layer >= uint(extent.z) || extent.x <= 0 || extent.y <= 0) {
            result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
            result.status_bits |= DDGI_FIRST_NATIVE_BIT_TEXTURE;
            return result;
        }
        result.normal_sample = textureLod(
            sampler2DArray(first_native_normal_textures, first_native_normal_sampler),
            vec3(result.scaled_uv, float(result.normal_layer)), 0.0);
        if (!ddgiFirstNativeFinite4(result.normal_sample)) {
            result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
            result.status_bits |= DDGI_FIRST_NATIVE_BIT_TEXTURE;
            return result;
        }
        vec2 texel_pos = result.scaled_uv * vec2(extent.xy) - vec2(0.5);
        ivec2 base = ivec2(floor(texel_pos));
        vec2 frac_pos = texel_pos - vec2(base);
        ivec2 p00 = ivec2((base.x % extent.x + extent.x) % extent.x,
                          (base.y % extent.y + extent.y) % extent.y);
        ivec2 p10 = ivec2((base.x + 1) % extent.x,
                          (base.y % extent.y + extent.y) % extent.y);
        ivec2 p01 = ivec2((base.x % extent.x + extent.x) % extent.x,
                          (base.y + 1) % extent.y);
        ivec2 p11 = ivec2((base.x + 1) % extent.x,
                          (base.y + 1) % extent.y);
        result.texel00 = texelFetch(sampler2DArray(first_native_normal_textures, first_native_normal_sampler),
                                    ivec3(p00, int(result.normal_layer)), 0);
        result.texel10 = texelFetch(sampler2DArray(first_native_normal_textures, first_native_normal_sampler),
                                    ivec3(p10, int(result.normal_layer)), 0);
        result.texel01 = texelFetch(sampler2DArray(first_native_normal_textures, first_native_normal_sampler),
                                    ivec3(p01, int(result.normal_layer)), 0);
        result.texel11 = texelFetch(sampler2DArray(first_native_normal_textures, first_native_normal_sampler),
                                    ivec3(p11, int(result.normal_layer)), 0);
        if (!ddgiFirstNativeFinite4(result.texel00) || !ddgiFirstNativeFinite4(result.texel10) ||
            !ddgiFirstNativeFinite4(result.texel01) || !ddgiFirstNativeFinite4(result.texel11)) {
            result.status = DDGI_FIRST_NATIVE_SOURCE_GUARD;
            result.status_bits |= DDGI_FIRST_NATIVE_BIT_TEXTURE;
            return result;
        }
        result.base_texel = vec2(base);
        float sample_length = DDGI_FIRST_NATIVE_LENGTH(result.normal_sample.xyz);
        if (!ddgiFirstFinite(sample_length) ||
            abs(sample_length - 0.1) <= ddgiFirstNativeGamma(4u)) {
            result.status = DDGI_FIRST_NATIVE_CONDITIONING;
            result.status_bits |= DDGI_FIRST_NATIVE_BIT_TEXTURE;
            return result;
        }
        if (sample_length > 0.1) {
            result.path_code = 2u;
            result.tangent_normal = result.normal_sample.xyz * 2.0 - 1.0;
            vec3 map_raw = tangent * result.tangent_normal.x +
                           bitangent * result.tangent_normal.y +
                           ray_native * result.tangent_normal.z;
            float map_error = tangent_bound * abs(result.tangent_normal.x) +
                              bitangent_bound * abs(result.tangent_normal.y) +
                              native_bound * abs(result.tangent_normal.z) +
                              ddgiFirstNativeGamma(8u);
            if (!ddgiFirstNativeNormalize(map_raw, map_error,
                                          result.mapped_n, result.mapped_bound)) {
                result.status = DDGI_FIRST_NATIVE_CONDITIONING;
                result.status_bits |= DDGI_FIRST_NATIVE_BIT_CONDITIONING;
                return result;
            }
        } else {
            result.path_code = 1u;
            result.tangent_normal = result.normal_sample.xyz * 2.0 - 1.0;
            result.base_texel = vec2(base);
        }
    } else {
        result.base_texel = vec2(-1.0);
    }
    if (!ddgiFirstFiniteVec3(result.mapped_n) || result.n_bound > DDGI_FIRST_NATIVE_NORMAL_GATE ||
        result.t_bound > DDGI_FIRST_NATIVE_NORMAL_GATE ||
        result.mapped_bound > DDGI_FIRST_NATIVE_NORMAL_GATE) {
        result.status = DDGI_FIRST_NATIVE_CONDITIONING;
        result.status_bits |= DDGI_FIRST_NATIVE_BIT_CONDITIONING;
        return result;
    }
    // Only the accepted branch changes HN.  UNKNOWN/conditioning rows keep
    // `after_hn == before_hn`, so the witness cannot be mistaken for a
    // speculative replacement.
#if defined(DDGI_FIRST_NATIVE_DIAGNOSTIC_CONTINUE_BOUNDS) && DDGI_FIRST_NATIVE_DIAGNOSTIC_CONTINUE_BOUNDS
    if (diagnostic_bound_rejected) {
        result.status = DDGI_FIRST_NATIVE_CONDITIONING;
        result.after_hn = result.before_hn;
        return result;
    }
#endif
    result.after_hn = result.mapped_n;
    result.status = DDGI_FIRST_NATIVE_ACCEPTED;
    result.status_bits |= DDGI_FIRST_NATIVE_BIT_ACCEPTED;
    return result;
}

void ddgiFirstNativeWriteAudit(uint global_ray_idx, uint probe_idx, uint ray_id,
                               uint frame_index, DDGIFirstNativeNormalResult result) {
    uint base = global_ray_idx * 24u;
    if (base + 23u >= first_native_audit.audit_records.length()) return;
    for (uint i = 0u; i < 24u; ++i) first_native_audit.audit_records[base + i] = vec4(0.0);
    uint stamp = ((frame_index & 0xffffu) << 16u) | (probe_idx + 1u);
    first_native_audit.audit_records[base + 0u] =
        vec4(result.world_point, uintBitsToFloat(stamp));
    first_native_audit.audit_records[base + 1u] =
        vec4(result.bary_native_n, result.bary_native_length);
    first_native_audit.audit_records[base + 2u] =
        vec4(result.ray_native_n, float(ray_id + 1u));
    first_native_audit.audit_records[base + 3u] =
        vec4(result.ray_geom_n, result.geom_length);
    first_native_audit.audit_records[base + 4u] =
        vec4(result.vertex_n0, uintBitsToFloat(result.packed_n0));
    first_native_audit.audit_records[base + 5u] =
        vec4(result.vertex_n1, uintBitsToFloat(result.packed_n1));
    first_native_audit.audit_records[base + 6u] =
        vec4(result.vertex_n2, uintBitsToFloat(result.packed_n2));
    first_native_audit.audit_records[base + 7u] =
        vec4(uintBitsToFloat(result.material_id), uintBitsToFloat(result.material_flags),
             uintBitsToFloat(result.instance_id), uintBitsToFloat(result.source_status));
    first_native_audit.audit_records[base + 8u] =
        vec4(uintBitsToFloat(result.vertex_ids.x), uintBitsToFloat(result.vertex_ids.y),
             uintBitsToFloat(result.vertex_ids.z), uintBitsToFloat(global_ray_idx));
    first_native_audit.audit_records[base + 9u] =
        vec4(result.raw_geom_n, result.flip_geom);
    first_native_audit.audit_records[base + 10u] =
        vec4(result.uv, result.scaled_uv);
    first_native_audit.audit_records[base + 11u] =
        vec4(result.tangent, uintBitsToFloat(result.normal_layer));
    first_native_audit.audit_records[base + 12u] = result.normal_sample;
    first_native_audit.audit_records[base + 13u] = result.texel00;
    first_native_audit.audit_records[base + 14u] = result.texel10;
    first_native_audit.audit_records[base + 15u] = result.texel01;
    first_native_audit.audit_records[base + 16u] = result.texel11;
    first_native_audit.audit_records[base + 17u] =
        vec4(result.mapped_n, uintBitsToFloat(result.path_code));
    first_native_audit.audit_records[base + 18u] =
        vec4(result.tangent_normal, result.normal_scale);
    first_native_audit.audit_records[base + 19u] =
        vec4(result.base_texel, result.flip_native, uintBitsToFloat(result.normal_layer));
    first_native_audit.audit_records[base + 20u] =
        vec4(result.n_bound, result.t_bound, result.mapped_bound,
             uintBitsToFloat(result.status_bits));
    first_native_audit.audit_records[base + 21u] =
        vec4(result.before_hn, result.old_hn_valid);
    first_native_audit.audit_records[base + 22u] =
        vec4(result.after_hn, uintBitsToFloat(result.status));
    first_native_audit.audit_records[base + 23u] =
        vec4(uintBitsToFloat(result.parent_status), uintBitsToFloat(result.material_flags),
             uintBitsToFloat(result.point_guard), uintBitsToFloat(result.instance_guard));
}

#endif // LUMEN_DDGI_FIRST_NATIVE_NORMAL_GLSL
