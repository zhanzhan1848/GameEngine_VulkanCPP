#version 450 core
// SDFVisualization.frag — fullscreen GlobalSDF ray-march visualization.
//
// Entry point: main.  Pairs with Forward/Blit.vert (procedural fullscreen
// triangle, outUv in [0,2]×[0,2]).
//
// For each pixel we reconstruct the world-space view ray via an inverse
// view-projection unproject, then sphere-trace it against the 3 GlobalSDF
// cascades.
//
// IMPORTANT: cascade textures are R32_Float Texture3D — an UNFILTERABLE
// format on Metal/MoltenVK. Using a combined sampler with linear filtering
// causes a GPU device-loss. We therefore use samplerless texture3D +
// texelFetch, and do trilinear interpolation manually in the shader.
//
// Bindings (flat — see SDFVisualizationModule.cpp):
//   binding 0 = SampledImage (texture3D)  sdf_cascade_0   (R32F Texture3D)
//   binding 1 = SampledImage (texture3D)  sdf_cascade_1
//   binding 2 = SampledImage (texture3D)  sdf_cascade_2
//   binding 3 = UniformBuffer              SDFVizUniforms

#extension GL_EXT_samplerless_texture_functions : enable

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec4 outColor;

// ---------------------------------------------------------------------------
// Bindings (samplerless — no sampler needed)
// ---------------------------------------------------------------------------
layout(set = 0, binding = 0) uniform texture3D sdf_cascade_0;
layout(set = 0, binding = 1) uniform texture3D sdf_cascade_1;
layout(set = 0, binding = 2) uniform texture3D sdf_cascade_2;

// std140 UBO. Layout MUST match SDFVizUniforms in SDFVisualizationModule.cpp.
layout(set = 0, binding = 3, std140) uniform SDFVizUniforms {
    vec4  SdfOrigins[3];          //   0  xyz = cascade origin
    vec4  SdfExtents[3];          //  48  xyz = cascade extent (world units)
    vec4  SdfVoxelSizes[3];       //  96  x = voxel size
    uvec4 SdfResolutionsAndCount; // 144  xyz = per-cascade res, w = cascade count
    vec4  CameraPosAndMaxDist;    // 160  xyz = camera pos, w = max trace dist
    vec4  Pad0;                   // 176  (layout compat)
    vec4  Pad1;                   // 192
    vec4  Pad2;                   // 208
    vec4  LightDirAndIntensity;   // 224  xyz = light dir, w = intensity
    mat4  InvViewProj;            // 240  world = InvViewProj * ndc
} uniforms;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
const uint MAX_STEPS = 3u;
const float SURF_DIST = 0.01;
const float HIT_FACTOR = 0.9;

// ---------------------------------------------------------------------------
// Nearest-neighbor fetch from a cascade at normalized uvw [0,1].
// Single texelFetch — minimal GPU cost. R32Float is unfilterable so no
// hardware filtering is available anyway.
// ---------------------------------------------------------------------------
float fetchCascade(int cascade, vec3 uvw) {
    if (cascade < 0 || cascade >= int(uniforms.SdfResolutionsAndCount.w))
        return 1e9;

    float res = float(uniforms.SdfResolutionsAndCount[cascade]);
    if (res <= 0.0) return 1e9;

    // Guard against NaN/Inf from upstream math — clamp BEFORE int conversion
    // because ivec3(NaN) is undefined behavior and crashes Metal GPUs.
    vec3 clamped = clamp(uvw, vec3(0.0), vec3(1.0));
    if (any(isnan(clamped)) || any(isinf(clamped))) return 1e9;
    vec3 coord = clamped * (res - 1.0);
    ivec3 ic = ivec3(clamp(coord, vec3(0.0), vec3(res - 1.0)));

    if (cascade == 0) return texelFetch(sdf_cascade_0, ic, 0).r;
    if (cascade == 1) return texelFetch(sdf_cascade_1, ic, 0).r;
    return texelFetch(sdf_cascade_2, ic, 0).r;
}

// Sample cascade i at world position p. Returns distance; large if outside.
float sampleSDF(int cascade, vec3 p) {
    vec3 origin = uniforms.SdfOrigins[cascade].xyz;
    vec3 extent = uniforms.SdfExtents[cascade].xyz;
    if (extent.x <= 0.0 || extent.y <= 0.0 || extent.z <= 0.0)
        return 1e9;

    vec3 local = (p - origin) / extent;   // [0,1]
    // NaN guard: if p is NaN/Inf, local is NaN and the comparisons below
    // silently pass through (NaN comparisons are always false).
    if (any(isnan(local)) || any(isinf(local))) return 1e9;
    if (local.x < 0.0 || local.x > 1.0 ||
        local.y < 0.0 || local.y > 1.0 ||
        local.z < 0.0 || local.z > 1.0)
        return 1e9;                        // outside this cascade

    return fetchCascade(cascade, local);
}

// Sample the best (finest) cascade available at p.
float mapSDF(vec3 p) {
    float d = 1e9;
    uint count = uniforms.SdfResolutionsAndCount.w;
    for (int c = 0; c < int(count); ++c) {
        float dc = sampleSDF(c, p);
        d = min(d, dc);
    }
    return d;
}

// Normal via central differences — only sample cascade 0 for speed.
vec3 estimateNormal(vec3 p) {
    float voxel  = uniforms.SdfVoxelSizes[0].x;
    vec3  extent0 = uniforms.SdfExtents[0].xyz;
    float e = max(voxel * 2.0, length(extent0) * 0.002);
    vec2 h = vec2(1.0, -1.0) * 0.5 * e;
    return normalize(
        h.xyy * sampleSDF(0, p + h.xyy) +
        h.yyx * sampleSDF(0, p + h.yyx) +
        h.yxy * sampleSDF(0, p + h.yxy) +
        h.xxx * sampleSDF(0, p + h.xxx)
    );
}

vec3 shade(vec3 normal, vec3 baseColor, vec3 lightDir, float intensity) {
    float diff = max(dot(normal, -lightDir), 0.0);
    vec3 ambient = baseColor * 0.25;
    return ambient + baseColor * diff * intensity;
}

vec3 depthHeat(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 c0 = vec3(1.0, 0.0, 0.0);
    vec3 c1 = vec3(1.0, 1.0, 0.0);
    vec3 c2 = vec3(0.0, 1.0, 0.0);
    vec3 c3 = vec3(0.0, 1.0, 1.0);
    vec3 c4 = vec3(0.0, 0.0, 1.0);
    if (t < 0.25)       return mix(c0, c1, t / 0.25);
    else if (t < 0.50)  return mix(c1, c2, (t - 0.25) / 0.25);
    else if (t < 0.75)  return mix(c2, c3, (t - 0.50) / 0.25);
    else                return mix(c3, c4, (t - 0.75) / 0.25);
}

void main() {
    vec3 camPos   = uniforms.CameraPosAndMaxDist.xyz;
    float maxDist = uniforms.CameraPosAndMaxDist.w;

    // Blit.vert emits UV in [0,2]; convert to NDC [-1,1].
    vec2 ndc = inUv * 2.0 - 1.0;

    // Unproject two points to get the world-space view ray.
    vec4 nearH = uniforms.InvViewProj * vec4(ndc, 0.0, 1.0);
    vec4 farH  = uniforms.InvViewProj * vec4(ndc, 1.0, 1.0);
    // Guard against division by zero / NaN from InvViewProj.
    if (abs(nearH.w) < 1e-6 || abs(farH.w) < 1e-6) {
        outColor = vec4(0.5, 0.0, 0.0, 1.0); // diagnostic red
        return;
    }
    vec3 nearPos = nearH.xyz / nearH.w;
    vec3 farPos  = farH.xyz  / farH.w;
    vec3 rayDir = normalize(farPos - nearPos);
    if (any(isnan(rayDir)) || dot(rayDir, rayDir) < 1e-12) {
        outColor = vec4(0.5, 0.0, 0.0, 1.0);
        return;
    }

    // ----- Sphere tracing -----
    float t = 0.0;
    bool hit = false;
    for (uint i = 0u; i < MAX_STEPS; ++i) {
        vec3 p = camPos + rayDir * t;
        float d = mapSDF(p);
        // Guard against NaN distance from corrupt SDF reads.
        if (isnan(d) || isinf(d)) break;
        if (d < SURF_DIST) { hit = true; break; }
        if (d > 1e8) break;          // left all cascades
        t += d * HIT_FACTOR;
        if (t > maxDist) break;
    }

    if (!hit) {
        float skyT = clamp(rayDir.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 sky = mix(vec3(0.05, 0.05, 0.08), vec3(0.25, 0.28, 0.35), skyT);
        outColor = vec4(sky, 1.0);
        return;
    }

    vec3 hitPos = camPos + rayDir * t;
    vec3 normal = estimateNormal(hitPos);
    if (any(isnan(normal)) || dot(normal, normal) < 0.0001)
        normal = vec3(0.0, 1.0, 0.0);

    vec3 lightDir = normalize(uniforms.LightDirAndIntensity.xyz);
    float lightI  = uniforms.LightDirAndIntensity.w;

    vec3 normalColor = normal * 0.5 + 0.5;
    vec3 lit = shade(normal, normalColor * 0.6, lightDir, lightI);

    float depthT = t / maxDist;
    vec3 heat = depthHeat(depthT) * 0.35;

    outColor = vec4(lit + heat, 1.0);
}
