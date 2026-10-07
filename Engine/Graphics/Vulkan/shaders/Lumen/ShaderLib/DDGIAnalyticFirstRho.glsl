#ifndef LUMEN_DDGI_ANALYTIC_FIRST_RHO_GLSL
#define LUMEN_DDGI_ANALYTIC_FIRST_RHO_GLSL

// Candidate-only material proof.  The normal application path is limited to
// analytic fallback source-11/source-14; diagnostic mode 7 also audits the
// actual SC-selected ray without applying any result.  This helper never
// changes the SDF distance, hit normal, relocation, card query, screen lookup,
// history probe lookup, or ray direction.

const uint DDGI_RHO_STATUS_NONE = 0u;
const uint DDGI_RHO_STATUS_PATH = 1u;
const uint DDGI_RHO_STATUS_SOURCE = 2u;
const uint DDGI_RHO_STATUS_BVH = 3u;
const uint DDGI_RHO_STATUS_FLAGS = 4u;
const uint DDGI_RHO_STATUS_MAPPING = 5u;
const uint DDGI_RHO_STATUS_POINT = 6u;
const uint DDGI_RHO_STATUS_MATERIAL = 7u;
const uint DDGI_RHO_STATUS_UV = 8u;
const uint DDGI_RHO_STATUS_TEXTURE = 9u;
const uint DDGI_RHO_STATUS_ALPHA = 10u;
const uint DDGI_RHO_STATUS_ACCEPTED = 11u;
const uint DDGI_RHO_STATUS_CONTROL = 12u;

const float DDGI_RHO_RESIDUAL_GATE = 2.0e-5;

struct DDGIAnalyticFirstRhoResult {
    uint status;
    uint material_id;
    uint mapped_material;
    uint cluster_material;
    uint instance_id;
    uint material_flags;
    uint triangle_flags;
    float first_t;
    float triangle_id;
    uint first_status;
    float first_residual;
    float residual;
    vec3 bary;
    vec3 old_rho;
    vec3 sample_rho;
    vec4 sample_rgba;
    vec3 tint;
    vec2 uv;
    vec2 uv_scale;
    vec4 texel00;
    vec4 texel10;
    vec4 texel01;
    vec4 texel11;
};

void ddgiAnalyticFirstRhoClear(out DDGIAnalyticFirstRhoResult r) {
    r.status = DDGI_RHO_STATUS_NONE;
    r.material_id = 0xffffffffu;
    r.mapped_material = 0xffffffffu;
    r.cluster_material = 0xffffffffu;
    r.instance_id = 0xffffffffu;
    r.material_flags = 0xffffffffu;
    r.triangle_flags = 0xffffffffu;
    r.first_t = -1.0;
    r.triangle_id = -1.0;
    r.first_status = 0u;
    r.first_residual = 3.4e38;
    r.residual = 3.4e38;
    r.bary = vec3(0.0);
    r.old_rho = vec3(0.0);
    r.sample_rho = vec3(0.0);
    r.sample_rgba = vec4(0.0);
    r.tint = vec3(0.0);
    r.uv = vec2(0.0);
    r.uv_scale = vec2(0.0);
    r.texel00 = vec4(0.0);
    r.texel10 = vec4(0.0);
    r.texel01 = vec4(0.0);
    r.texel11 = vec4(0.0);
}

bool ddgiAnalyticFirstRhoFinite4(vec4 v) {
    return !any(isnan(v)) && !any(isinf(v));
}

bool ddgiAnalyticFirstRhoIdentity(mat4 m) {
    for (uint c = 0u; c < 4u; ++c)
        for (uint r = 0u; r < 4u; ++r)
            if (!ddgiFirstFinite(m[c][r]) ||
                m[c][r] != ((c == r) ? 1.0 : 0.0)) return false;
    return true;
}

bool ddgiAnalyticFirstRhoPosition(uint id, out vec3 p) {
    uint count = analytic_rho_positions.positions.length() / 3u;
    if (id >= count) return false;
    p = vec3(analytic_rho_positions.positions[id * 3u + 0u],
             analytic_rho_positions.positions[id * 3u + 1u],
             analytic_rho_positions.positions[id * 3u + 2u]);
    return ddgiFirstFiniteVec3(p);
}

bool ddgiAnalyticFirstRhoElement(uint id, out vec2 uv) {
    uint count = analytic_rho_elements.elements.length() / 6u;
    if (id >= count) return false;
    uint base = id * 6u;
    // The native element stream stores V down; preserve the actual loader
    // convention used by GPUDrivenDraw before applying the material UV scale.
    uv = vec2(uintBitsToFloat(analytic_rho_elements.elements[base + 4u]),
              1.0 - uintBitsToFloat(analytic_rho_elements.elements[base + 5u]));
    return ddgiFirstFiniteVec3(vec3(uv, 0.0));
}

DDGIAnalyticFirstRhoResult ddgiAnalyticFirstRhoEvaluate(
    DDGIFirstSurfaceHit first, vec3 ray_origin, vec3 ray_dir,
    float original_distance, vec3 old_rho, bool apply_rho) {
    DDGIAnalyticFirstRhoResult r;
    ddgiAnalyticFirstRhoClear(r);
    r.old_rho = old_rho;
    r.first_t = first.t;
    r.triangle_id = first.triangle;
    r.first_status = first.status;
    r.first_residual = first.residual;
    r.bary = first.bary;
    r.triangle_flags = first.flags;

    if (first.status != 1u || !ddgiFirstFinite(first.t) ||
        first.t <= 2.0e-5 || first.t >= volume.RayMaxDistance - 2.0e-5) {
        r.status = DDGI_RHO_STATUS_BVH;
        r.first_t = first.t;
        r.bary = first.bary;
        r.triangle_flags = first.flags;
        return r;
    }
    if (first.flags != 0u) {
        r.status = DDGI_RHO_STATUS_FLAGS;
        r.first_t = first.t;
        r.bary = first.bary;
        r.triangle_flags = first.flags;
        return r;
    }
    if (!ddgiFirstFinite(first.triangle) || first.triangle < 0.0 ||
        first.triangle != floor(first.triangle)) {
        r.status = DDGI_RHO_STATUS_MAPPING;
        return r;
    }
    uint triangle_id = uint(first.triangle);
    r.triangle_id = float(triangle_id);
    if (triangle_id * 2u + 1u >= analytic_rho_mapping.mapping_words.length()) {
        r.status = DDGI_RHO_STATUS_MAPPING;
        return r;
    }
    uvec4 map0 = analytic_rho_mapping.mapping_words[triangle_id * 2u + 0u];
    uvec4 map1 = analytic_rho_mapping.mapping_words[triangle_id * 2u + 1u];
    r.mapped_material = map1.x;
    r.cluster_material = map1.z;
    r.instance_id = map0.w;
    if (map1.x != map1.z || map1.w != 0u ||
        r.instance_id >= analytic_rho_instances.instances.length()) {
        r.status = DDGI_RHO_STATUS_MAPPING;
        return r;
    }
    DDGIAnalyticRhoInstance instance = analytic_rho_instances.instances[r.instance_id];
    if (!ddgiAnalyticFirstRhoIdentity(instance.world_matrix)) {
        r.status = DDGI_RHO_STATUS_MAPPING;
        return r;
    }
    if (!ddgiFirstFiniteVec3(first.bary) ||
        any(lessThan(first.bary, vec3(-2.0e-5))) ||
        any(greaterThan(first.bary, vec3(1.0 + 2.0e-5))) ||
        abs(first.bary.x + first.bary.y + first.bary.z - 1.0) > 2.0e-5) {
        r.status = DDGI_RHO_STATUS_POINT;
        return r;
    }
    vec3 p0, p1, p2;
    if (!ddgiAnalyticFirstRhoPosition(map0.x, p0) ||
        !ddgiAnalyticFirstRhoPosition(map0.y, p1) ||
        !ddgiAnalyticFirstRhoPosition(map0.z, p2)) {
        r.status = DDGI_RHO_STATUS_POINT;
        return r;
    }
    vec3 native_point = first.bary.x * p0 + first.bary.y * p1 + first.bary.z * p2;
    vec3 ray_point = ray_origin + ray_dir * first.t;
    float native_residual = length(native_point - ray_point);
    r.first_t = first.t;
    r.bary = first.bary;
    vec3 native_terms = abs(first.bary.x * p0) + abs(first.bary.y * p1) +
                        abs(first.bary.z * p2) + abs(ray_origin) + abs(ray_dir * first.t);
    float native_bound = native_residual +
        4.0 * 1.1920928955078125e-7 * length(native_terms);
    // Independent fixed point guards. Preserve the original SDF distance;
    // matching that distance to the first triangle would reject the mechanism
    // under investigation and is not a material eligibility condition.
    r.residual = max(native_bound, first.residual);
    if (!ddgiFirstFinite(native_residual) || !ddgiFirstFinite(r.residual) ||
        r.residual > DDGI_RHO_RESIDUAL_GATE) {
        r.status = DDGI_RHO_STATUS_POINT;
        return r;
    }

    uint material_id = instance.meta0.y;
    r.material_id = material_id;
    if (material_id != map1.x || material_id != map1.z ||
        material_id * 12u + 11u >= analytic_rho_materials.material_words.length()) {
        r.status = DDGI_RHO_STATUS_MAPPING;
        return r;
    }
    uint material_base = material_id * 12u;
    uint material_flags = analytic_rho_materials.material_words[material_base + 11u];
    r.material_flags = material_flags;
    if (material_flags != 0u) {
        r.status = DDGI_RHO_STATUS_MATERIAL;
        return r;
    }
    vec2 uv0, uv1, uv2;
    if (!ddgiAnalyticFirstRhoElement(map0.x, uv0) ||
        !ddgiAnalyticFirstRhoElement(map0.y, uv1) ||
        !ddgiAnalyticFirstRhoElement(map0.z, uv2)) {
        r.status = DDGI_RHO_STATUS_UV;
        return r;
    }
    r.uv = first.bary.x * uv0 + first.bary.y * uv1 + first.bary.z * uv2;
    r.uv_scale = vec2(
        uintBitsToFloat(analytic_rho_materials.material_words[material_base + 9u]),
        uintBitsToFloat(analytic_rho_materials.material_words[material_base + 10u]));
    if (!ddgiFirstFiniteVec3(vec3(r.uv, 0.0)) ||
        !ddgiFirstFiniteVec3(vec3(r.uv_scale, 0.0)) ||
        any(notEqual(r.uv_scale, vec2(1.0)))) {
        r.status = DDGI_RHO_STATUS_UV;
        return r;
    }
    vec2 wrapped_uv = fract(r.uv * r.uv_scale);
    uint layer = analytic_rho_materials.material_words[material_base + 0u];
    ivec3 extent = textureSize(
        sampler2DArray(analytic_rho_albedo, analytic_rho_sampler), 0);
    if (extent.x <= 0 || extent.y <= 0 || extent.z <= 0 || layer >= uint(extent.z)) {
        r.status = DDGI_RHO_STATUS_TEXTURE;
        return r;
    }
    r.sample_rgba = textureLod(
        sampler2DArray(analytic_rho_albedo, analytic_rho_sampler),
        vec3(wrapped_uv, float(layer)), 0.0);
    if (!ddgiAnalyticFirstRhoFinite4(r.sample_rgba)) {
        r.status = DDGI_RHO_STATUS_TEXTURE;
        return r;
    }
    if (r.sample_rgba.a != 1.0) {
        r.status = DDGI_RHO_STATUS_ALPHA;
        return r;
    }
    r.tint = vec3(uintBitsToFloat(analytic_rho_materials.material_words[material_base + 3u]),
                  uintBitsToFloat(analytic_rho_materials.material_words[material_base + 4u]),
                  uintBitsToFloat(analytic_rho_materials.material_words[material_base + 5u]));
    if (!ddgiFirstFiniteVec3(r.tint)) {
        r.status = DDGI_RHO_STATUS_MATERIAL;
        return r;
    }
    r.sample_rho = clamp(r.sample_rgba.rgb * r.tint, vec3(0.0), vec3(1.0));
    // Quad witness only when the DUMP2 source-audit latch is active. Formal
    // candidate rendering samples the material once and never writes this tail.
    if (volume.Albedo.w > 0.5) {
        vec2 pixel = wrapped_uv * vec2(extent.xy) - 0.5;
        ivec2 base = ivec2(floor(pixel));
        ivec2 size = extent.xy;
        ivec2 p00 = ((base % size) + size) % size;
        ivec2 p10 = (((base + ivec2(1,0)) % size) + size) % size;
        ivec2 p01 = (((base + ivec2(0,1)) % size) + size) % size;
        ivec2 p11 = (((base + ivec2(1,1)) % size) + size) % size;
        r.texel00 = texelFetch(sampler2DArray(analytic_rho_albedo, analytic_rho_sampler), ivec3(p00, int(layer)), 0);
        r.texel10 = texelFetch(sampler2DArray(analytic_rho_albedo, analytic_rho_sampler), ivec3(p10, int(layer)), 0);
        r.texel01 = texelFetch(sampler2DArray(analytic_rho_albedo, analytic_rho_sampler), ivec3(p01, int(layer)), 0);
        r.texel11 = texelFetch(sampler2DArray(analytic_rho_albedo, analytic_rho_sampler), ivec3(p11, int(layer)), 0);
    }
    r.status = apply_rho ? DDGI_RHO_STATUS_ACCEPTED : DDGI_RHO_STATUS_CONTROL;
    return r;
}

#endif
