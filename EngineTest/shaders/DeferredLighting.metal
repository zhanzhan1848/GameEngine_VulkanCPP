#include <metal_stdlib>
using namespace metal;

#define RHI_ENABLE_PBR
#include "RHIShaderCommon.metal"

// ================================================================================================
// Data Structures (Must match C++ Binding)
// ================================================================================================

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

struct SceneData {
    float4x4 model;
    float4 lightPos;
    float4 lightColor;
    float4 reflectionPlane;
    float4 reflectionPlane2;
    float4 reflectionPlane3;
    float4x4 previousModel;
    float4 viewPos;
    float4x4 shadowMatrix0;
    float4x4 shadowMatrix1;
    float2 jitter;
    float2 previousJitter;
    float2 padding;
};

struct ViewData {
    float4x4 viewProjection;
    float4x4 invViewProjection;
};

// ================================================================================================
// Vertex Shader
// ================================================================================================

vertex VertexOut vertexMain(uint vertexID [[vertex_id]]) {
    VertexOut out;
    // Full-screen triangle covering the screen
    float4 positions[3] = {
        float4(-1.0, -1.0, 0.0, 1.0), // Bottom-Left
        float4(-1.0,  3.0, 0.0, 1.0), // Top-Left (Extended)
        float4( 3.0, -1.0, 0.0, 1.0)  // Bottom-Right (Extended)
    };
    float2 uvs[3] = {
        float2(0.0, 1.0),  // Bottom-Left UV (Metal Texture: 0,0 is Top-Left)
        float2(0.0, -1.0), // Top-Left UV
        float2(2.0, 1.0)   // Bottom-Right UV
    };
    
    out.position = positions[vertexID];
    out.uv = uvs[vertexID];
    return out;
}

// ================================================================================================
// Shadow Helper
// ================================================================================================

// Diagnostic: read raw shadow map depth (no PCF, no comparison)
float ReadRawShadowDepth(float3 worldPos, float4x4 shadowMatrix, texture2d<float> shadowMap) {
    constexpr sampler shadowSampler(coord::normalized, filter::nearest, mip_filter::none, address::clamp_to_edge);
    float4 clipPos = shadowMatrix * float4(worldPos, 1.0);
    float3 shadowCoord = clipPos.xyz / clipPos.w;
    shadowCoord.x = shadowCoord.x * 0.5 + 0.5;
    shadowCoord.y = shadowCoord.y * -0.5 + 0.5;
    if (shadowCoord.x < 0.0 || shadowCoord.x > 1.0 ||
        shadowCoord.y < 0.0 || shadowCoord.y > 1.0 ||
        shadowCoord.z < 0.0 || shadowCoord.z > 1.0) {
        return -1.0;
    }
    return shadowMap.sample(shadowSampler, shadowCoord.xy).r;
}

float GetShadow(float3 worldPos, float3 normal, float3 lightDir, float4x4 shadowMatrix, texture2d<float> shadowMap, sampler passedSampler) {
    constexpr sampler shadowSampler(coord::normalized, filter::nearest, mip_filter::none, address::clamp_to_edge);

    float4 clipPos = shadowMatrix * float4(worldPos, 1.0);
    float3 shadowCoord = clipPos.xyz / clipPos.w;

    // NDC to UV (Metal Y is inverted: clipY=+1 → texture row 0 → UV.y=0)
    shadowCoord.x = shadowCoord.x * 0.5 + 0.5;
    shadowCoord.y = shadowCoord.y * -0.5 + 0.5;

    // Check bounds
    if (shadowCoord.x < 0.0 || shadowCoord.x > 1.0 ||
        shadowCoord.y < 0.0 || shadowCoord.y > 1.0 ||
        shadowCoord.z < 0.0 || shadowCoord.z > 1.0) {
        return -1.0; // Return -1 to indicate “Out of Bounds”
    }

    // PCF 3x3
    float shadow = 0.0;
    float currentDepth = shadowCoord.z;
    float NdotL = max(dot(normalize(normal), normalize(lightDir)), 0.0);
    float bias = 0.002 + (1.0 - NdotL) * 0.02;
    float2 texelSize = float2(1.0 / 2048.0, 1.0 / 2048.0);

    for(int x = -1; x <= 1; ++x) {
        for(int y = -1; y <= 1; ++y) {
            float pcfDepth = shadowMap.sample(shadowSampler, shadowCoord.xy + float2(x, y) * texelSize).r;
            shadow += (currentDepth - bias > pcfDepth) ? 0.0 : 1.0;
        }
    }

    return shadow / 9.0;
}

// ================================================================================================
// Fragment Shader (PBR Lighting)
// ================================================================================================

fragment float4 fragmentLighting_v3(
    VertexOut in [[stage_in]],
    constant ViewData& viewData [[buffer(0)]],
    constant SceneData& sceneData [[buffer(1)]],

    texture2d<float> albedoTex [[texture(2)]],
    texture2d<float> normalTex [[texture(3)]],
    texture2d<float> ormTex [[texture(4)]],
    depth2d<float> depthTex [[texture(5)]],
    texture2d<float> shadowMap0 [[texture(6)]],
    texture2d<float> shadowMap1 [[texture(7)]],
    texturecube<float> irradianceMap [[texture(8)]],
    texturecube<float> prefilterMap [[texture(9)]],
    texture2d<float> brdfLUT [[texture(10)]],
    texture2d<float> ssaoTex [[texture(13)]],

    sampler defaultSampler [[sampler(11)]],
    sampler brdfSampler [[sampler(12)]]
) {
    float2 uv = in.uv;

    // 1. Sample GBuffer
    float4 albedo = albedoTex.sample(defaultSampler, uv);
    float3 normal = normalTex.sample(defaultSampler, uv).xyz;
    normal = normal * 2.0 - 1.0; // Unpack [0,1] -> [-1,1]
    float depth = depthTex.sample(defaultSampler, uv);

    // Discard background pixels to preserve Skybox
    if (depth >= 1.0) {
        discard_fragment();
    }

    float4 orm = ormTex.sample(defaultSampler, uv);
    float roughness = orm.g;
    float metallic = orm.b;

    // Sample SSAO texture (R16_Float, 0=fully occluded, 1=unoccluded)
    // Fallback to 1.0 (no occlusion) if SSAO is not available
    float ssao = ssaoTex.sample(defaultSampler, uv).r;
    if (ssao <= 0.0) ssao = 1.0;
    // Moderate AO: power curve adds depth to occluded areas
    // pow(1.5) preserves some ambient base so DDGI indirect can build on it
    float ao = pow(ssao, 1.5);

    // 2. Reconstruct World Position
    // Metal NDC: X [-1, 1], Y [-1, 1], Z [0, 1]
    // The previous Y logic flipped the UV, but we need to ensure X is correct.
    // Actually, in Metal, UV (0,0) is Top-Left, (1,1) is Bottom-Right.
    // NDC (-1,-1) is Bottom-Left, (1,1) is Top-Right.
    // So:
    // NDC.x = UV.x * 2.0 - 1.0  (0 -> -1, 1 -> 1)
    // NDC.y = (1.0 - UV.y) * 2.0 - 1.0 (0 -> 1, 1 -> -1) -> Or simply: 1.0 - UV.y * 2.0
    float2 ndc;
    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = 1.0 - uv.y * 2.0;

    float z = depth;

    float4 clipPos = float4(ndc, z, 1.0);

    float4 worldPos4 = viewData.invViewProjection * clipPos;
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    // Calculate PBR Lighting
    float3 N = normalize(normal);
    float3 V = normalize(sceneData.viewPos.xyz - worldPos);
    float3 R = reflect(-V, N);

    // 3. Calculate Shadow (Cascade 0 first, then Cascade 1 if out of bounds)
    float3 L_shadow = normalize(sceneData.lightPos.xyz);
    float shadow = GetShadow(worldPos, N, L_shadow, sceneData.shadowMatrix0, shadowMap0, defaultSampler);
    if (shadow < 0.0) {
        shadow = GetShadow(worldPos, N, L_shadow, sceneData.shadowMatrix1, shadowMap1, defaultSampler);
        if (shadow < 0.0) {
            shadow = 1.0; // Outside all cascades, default to fully lit
        }
    }

    // F0 for Fresnel
    float3 F0 = float3(0.04); 
    F0 = mix(F0, albedo.rgb, metallic);

    float3 Lo = float3(0.0);

    // --- Direct Light (Directional) ---
    {
        float3 L;
        float attenuation = 1.0;

        if (sceneData.lightPos.w == 0.0) {
            // Directional Light
            // lightPos.xyz is the direction TO the light source
            L = normalize(sceneData.lightPos.xyz);
        } else {
            // Point Light
            float3 lightDir = sceneData.lightPos.xyz - worldPos;
            float distance = length(lightDir);
            L = normalize(lightDir);
            attenuation = 1.0 / (distance * distance); // Inverse square falloff
        }

        float3 H = normalize(V + L);
        
        float3 radiance = sceneData.lightColor.rgb * shadow * attenuation; 
        
        // Cook-Torrance BRDF
        float NDF = DistributionGGX(N, H, roughness);   
        float G   = GeometrySmith(N, V, L, roughness);      
        float3 F  = FresnelSchlick(max(dot(H, V), 0.0), F0);
        
        float3 numerator    = NDF * G * F; 
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        float3 specular = numerator / denominator;
        
        // kS is equal to Fresnel
        float3 kS = F;
        float3 kD = float3(1.0) - kS;
        kD *= 1.0 - metallic;	  

        float NdotL = max(dot(N, L), 0.0);        

        Lo += (kD * albedo.rgb / PI + specular) * radiance * NdotL;  
    }

    // --- Ambient Light (IBL) ---
    // IBL Diffuse (Irradiance)
    float3 kS = FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    float3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;
    
    float3 irradiance = irradianceMap.sample(defaultSampler, N).rgb;
    float3 diffuse = irradiance * albedo.rgb;
    
    // IBL Specular (Prefilter + BRDF)
    const float MAX_REFLECTION_LOD = 4.0;
    float3 prefilteredColor = prefilterMap.sample(defaultSampler, R, level(roughness * MAX_REFLECTION_LOD)).rgb;
    float2 brdf = brdfLUT.sample(brdfSampler, float2(max(dot(N, V), 0.0), roughness)).rg;
    float3 specular = prefilteredColor * (kS * brdf.x + brdf.y);
    
    // IBL ambient provides base fill light; DDGI adds directional indirect on top
    float iblIntensity = 0.3; 
    float3 ambient = (kD * diffuse + specular) * iblIntensity; // AO is in orm.r
    ambient *= ao; 
    
    float3 color = ambient + Lo;

    // NOTE: No tone mapping here — output is HDR linear to intermediate texture.
    // Tone mapping + gamma is applied by the final blit shader (fragmentBlitDDGI,
    // fragmentBlitComposite, or fragmentBlit) before writing to the backbuffer.
    
    return float4(color, 1.0);
}

// ================================================================================================
// Blit Shader
// ================================================================================================

fragment float4 fragmentBlit(
    VertexOut in [[stage_in]],
    texture2d<float> inputTex [[texture(0)]]
) {
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
    float3 color = inputTex.sample(s, in.uv).rgb;

    // Final output: tone map + gamma (fragmentLighting outputs HDR linear)
    color = color / (color + float3(1.0));       // Reinhard
    color = pow(color, float3(1.0 / 2.2));        // sRGB gamma

    return float4(color, 1.0);
}

// DDGI indirect lighting blit — final composite to backbuffer:
//   1. Sample G-Buffer (albedo, normal) for surface properties
//   2. Full SH0-SH3 directional evaluation with surface normal
//   3. DDGI depth-based visibility weighting (reduces light leaking)
//   4. Modulate by albedo (no /PI — DDGI irradiance already integrates hemisphere)
//   5. Add to HDR scene color
//   6. Apply Reinhard tone mapping + sRGB gamma (final output stage)
fragment float4 fragmentBlitDDGI(
    VertexOut in [[stage_in]],
    texture2d<float, access::sample> sceneColor    [[texture(0)]],
    depth2d<float, access::sample> depthTex        [[texture(1)]],
    texture3d<float, access::sample> irradianceTex  [[texture(2)]],
    texture2d<float, access::sample> albedoTex      [[texture(3)]],
    texture2d<float, access::sample> normalTex      [[texture(4)]],
    texture3d<float, access::sample> ddgiDepthTex   [[texture(5)]],
    constant float4x4& invViewProjection [[buffer(0)]],
    constant float4& probeOrigin_spacing [[buffer(1)]],
    constant float4& probeCounts_shCountF [[buffer(2)]]
) {
    constexpr sampler s2d(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);

    float2 uv = in.uv;
    float4 scene = sceneColor.sample(s2d, uv);
    float depth = depthTex.sample(s2d, uv);

    // Sky pixels
    if (depth >= 1.0f) return scene;

    // Reconstruct world position
    float2 ndc;
    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = (1.0 - uv.y) * 2.0 - 1.0;
    float4 worldPos4 = invViewProjection * float4(ndc, depth, 1.0);
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    // Sample G-Buffer
    float3 albedo = albedoTex.sample(s2d, uv).rgb;
    float3 normal = normalize(normalTex.sample(s2d, uv).xyz * 2.0 - 1.0);

    float3 probeOrigin = probeOrigin_spacing.xyz;
    float  probeSpacing = probeOrigin_spacing.w;
    uint3  probeCounts = uint3(uint(probeCounts_shCountF.x), uint(probeCounts_shCountF.y), uint(probeCounts_shCountF.z));
    uint   shCount = uint(probeCounts_shCountF.w);

    // Probe grid continuous coordinates
    float3 gridPos = (worldPos - probeOrigin) / probeSpacing;

    // Bounds check
    if (gridPos.x < 0.0f || gridPos.x >= float(probeCounts.x) ||
        gridPos.y < 0.0f || gridPos.y >= float(probeCounts.y) ||
        gridPos.z < 0.0f || gridPos.z >= float(probeCounts.z)) {
        return scene;
    }

    int3 baseProbe = int3(floor(gridPos));
    float3 fracPart = fract(gridPos);

    // SH constants (must match DDGIVolumeData.metal)
    const float SH_C0 = 0.282095f;
    const float SH_C1 = 0.488603f;

    float3 totalIrradiance = float3(0.0f);
    float  totalWeight = 0.0f;

    for (uint corner = 0; corner < 8; ++corner) {
        int3 offset = int3(
            (corner & 1u) ? 1 : 0,
            (corner & 2u) ? 1 : 0,
            (corner & 4u) ? 1 : 0
        );
        int3 probeCoord = baseProbe + offset;

        if (probeCoord.x < 0 || probeCoord.x >= int(probeCounts.x) ||
            probeCoord.y < 0 || probeCoord.y >= int(probeCounts.y) ||
            probeCoord.z < 0 || probeCoord.z >= int(probeCounts.z)) {
            continue;
        }

        // Probe world position
        float3 probeWorldPos = probeOrigin + float3(float(probeCoord.x), float(probeCoord.y), float(probeCoord.z)) * probeSpacing;

        // --- Visibility weighting ---
        float3 toProbe = probeWorldPos - worldPos;
        float  dist = length(toProbe);
        float3 dir  = toProbe / max(dist, 0.001f);

        // Backface weight: probe behind surface gets reduced weight
        float NoL = dot(normal, dir);
        float backfaceWeight = max(NoL + 1.0f, 0.0f) * 0.5f;

        // Distance weight
        float distWeight = 1.0f / max(dist * dist, 0.01f);

        // Depth validity from DDGI depth texture (8-direction depth check)
        float3 probeToSurf = -dir;
        uint octant = 0u;
        if (probeToSurf.x > 0.0f) octant |= 1u;
        if (probeToSurf.y > 0.0f) octant |= 2u;
        if (probeToSurf.z > 0.0f) octant |= 4u;
        uint depthTexelIdx = octant / 2u;
        uint3 depthCoord = uint3(probeCoord.x, probeCoord.y, probeCoord.z * 4 + depthTexelIdx);
        float storedDepth = 100.0f;
        if (depthCoord.z < probeCounts.z * 4) {
            float2 octantDepth = ddgiDepthTex.read(depthCoord).rg;
            storedDepth = (octant == depthTexelIdx * 2) ? octantDepth.x : octantDepth.y;
        }
        // Depth validity: storedDepth >= dist means the probe "sees through" to the surface.
        // Wide threshold (0.5 → 2.0) avoids false self-shadowing from SDF voxel precision.
        float depthValidity = smoothstep(0.5f, 2.0f, storedDepth / max(dist, 0.001f));

        // --- Trilinear weight ---
        float3 blendW;
        blendW.x = (corner & 1u) ? fracPart.x : (1.0f - fracPart.x);
        blendW.y = (corner & 2u) ? fracPart.y : (1.0f - fracPart.y);
        blendW.z = (corner & 4u) ? fracPart.z : (1.0f - fracPart.z);
        float trilinWeight = blendW.x * blendW.y * blendW.z;

        float weight = backfaceWeight * distWeight * depthValidity * trilinWeight;

        // --- Full SH evaluation (SH0-SH3) with surface normal ---
        float3 sh[4];
        for (uint i = 0; i < 4; ++i) {
            uint3 texCoord = uint3(probeCoord.x, probeCoord.y, probeCoord.z * shCount + i);
            sh[i] = irradianceTex.read(texCoord).rgb;
        }

        float3 probeIrradiance = float3(0.0f);
        probeIrradiance += sh[0] * SH_C0;
        probeIrradiance += sh[1] * (-SH_C1 * normal.y);
        probeIrradiance += sh[2] * ( SH_C1 * normal.z);
        probeIrradiance += sh[3] * ( SH_C1 * normal.x);
        probeIrradiance = max(probeIrradiance, float3(0.0f));

        totalIrradiance += probeIrradiance * weight;
        totalWeight += weight;
    }

    if (totalWeight > 0.0f) {
        totalIrradiance /= totalWeight;
    }

    // DDGI irradiance already includes hemisphere integral from SH projection+reconstruction.
    // Do NOT divide by PI here — that would make indirect light ~3x too dark.
    float3 indirect = totalIrradiance * albedo;

    // NaN protection: uninitialized probe textures can contain NaN.
    // scene + NaN = NaN wipes out the entire scene (black "missing polygons").
    if (any(isnan(indirect)) || any(isinf(indirect))) {
        indirect = float3(0.0f);
    }

    // Add DDGI indirect to scene
    float3 result = scene.rgb + indirect;

    // Tone mapping + gamma: the backbuffer is BGRA8, values >1.0 are lost without this.
    result = result / (result + float3(1.0));       // Reinhard
    result = pow(result, float3(1.0 / 2.2));        // sRGB gamma

    return float4(result, 1.0f);
}

// Diagnostic: read raw shadow depth from cascade (MUST be in separate function
// to avoid Metal compiler silently outputting RGBA=0 when sampling textures inline)
float3 DiagShadowDepth(float3 worldPos, float4x4 shadowMatrix, texture2d<float> shadowMap) {
    constexpr sampler diagSampler(coord::normalized, filter::nearest, mip_filter::none, address::clamp_to_edge);
    float4 clipPos = shadowMatrix * float4(worldPos, 1.0);
    float3 sc = clipPos.xyz / clipPos.w;
    sc.x = sc.x * 0.5 + 0.5;
    sc.y = sc.y * -0.5 + 0.5;

    if (sc.x < 0.0 || sc.x > 1.0 || sc.y < 0.0 || sc.y > 1.0 || sc.z < 0.0 || sc.z > 1.0) {
        return float3(-1.0);  // Out of bounds sentinel
    }

    float smDepth = shadowMap.sample(diagSampler, sc.xy).r;
    float diff = sc.z - smDepth;
    return float3(sc.z, smDepth, diff);
}

// Debug: sample shadow map using screen UV (no worldPos dependency)
float2 DiagShadowMapRaw(float2 screenUV, texture2d<float> sm0, texture2d<float> sm1) {
    constexpr sampler diagSampler(coord::normalized, filter::nearest, mip_filter::none, address::clamp_to_edge);
    float d0 = sm0.sample(diagSampler, screenUV).r;
    float d1 = sm1.sample(diagSampler, screenUV).r;
    return float2(d0, d1);
}

// ================================================================================================
// GPU-Driven Deferred PBR Lighting with DDGI Indirect
// ================================================================================================

// DDGI probe params (must match C++ DDGIProbeParamsCB layout)
struct DDGIProbeParams {
    float4 probeOrigin_spacing;   // xyz = probe origin, w = probe spacing
    float4 probeCounts_shCount;   // xyz = (Nx, Ny, Nz), w = 4 (SH coeff count)
};

// Sample DDGI irradiance via trilinear probe interpolation with SH evaluation.
// Returns indirect irradiance (incoming light, NOT modulated by albedo).
static float3 sampleDDGIIndirect(
    float3 worldPos,
    float3 normal,
    texture3d<float, access::sample> irradianceTex,
    constant DDGIProbeParams& params)
{
    float3 probeOrigin = params.probeOrigin_spacing.xyz;
    float  probeSpacing = params.probeOrigin_spacing.w;
    uint3  probeCounts = uint3(uint(params.probeCounts_shCount.x),
                               uint(params.probeCounts_shCount.y),
                               uint(params.probeCounts_shCount.z));
    uint   shCount = uint(params.probeCounts_shCount.w);

    // Probe grid continuous coordinates
    float3 gridPos = (worldPos - probeOrigin) / probeSpacing;

    // Bounds check — outside probe grid, no indirect light
    if (gridPos.x < 0.0f || gridPos.x >= float(probeCounts.x) ||
        gridPos.y < 0.0f || gridPos.y >= float(probeCounts.y) ||
        gridPos.z < 0.0f || gridPos.z >= float(probeCounts.z)) {
        return float3(0.0f);
    }

    int3 baseProbe = int3(floor(gridPos));
    float3 fracPart = fract(gridPos);

    // SH constants (must match DDGIVolumeData.metal)
    const float SH_C0 = 0.282095f;
    const float SH_C1 = 0.488603f;

    float3 totalIrradiance = float3(0.0f);
    float  totalWeight = 0.0f;

    for (uint corner = 0; corner < 8; ++corner) {
        int3 offset = int3(
            (corner & 1u) ? 1 : 0,
            (corner & 2u) ? 1 : 0,
            (corner & 4u) ? 1 : 0
        );
        int3 probeCoord = baseProbe + offset;

        if (probeCoord.x < 0 || probeCoord.x >= int(probeCounts.x) ||
            probeCoord.y < 0 || probeCoord.y >= int(probeCounts.y) ||
            probeCoord.z < 0 || probeCoord.z >= int(probeCounts.z)) {
            continue;
        }

        // Probe world position
        float3 probeWorldPos = probeOrigin + float3(float(probeCoord.x), float(probeCoord.y), float(probeCoord.z)) * probeSpacing;

        // Distance-based weight
        float3 toProbe = probeWorldPos - worldPos;
        float  dist = length(toProbe);
        float3 dir  = toProbe / max(dist, 0.001f);

        // Backface weight: reduce contribution from probes behind the surface
        float NoL = dot(normal, dir);
        float backfaceWeight = max(NoL + 1.0f, 0.0f) * 0.5f;

        // Distance weight
        float distWeight = 1.0f / max(dist * dist, 0.01f);

        // Trilinear weight
        float3 blendW;
        blendW.x = (corner & 1u) ? fracPart.x : (1.0f - fracPart.x);
        blendW.y = (corner & 2u) ? fracPart.y : (1.0f - fracPart.y);
        blendW.z = (corner & 4u) ? fracPart.z : (1.0f - fracPart.z);
        float trilinWeight = blendW.x * blendW.y * blendW.z;

        float weight = backfaceWeight * distWeight * trilinWeight;

        // SH evaluation: read 4 SH coefficients and evaluate with surface normal
        float3 sh[4];
        for (uint i = 0; i < 4; ++i) {
            uint3 texCoord = uint3(probeCoord.x, probeCoord.y, probeCoord.z * shCount + i);
            sh[i] = irradianceTex.read(texCoord).rgb;
        }

        float3 probeIrradiance = float3(0.0f);
        probeIrradiance += sh[0] * SH_C0;
        probeIrradiance += sh[1] * (-SH_C1 * normal.y);
        probeIrradiance += sh[2] * ( SH_C1 * normal.z);
        probeIrradiance += sh[3] * ( SH_C1 * normal.x);
        probeIrradiance = max(probeIrradiance, float3(0.0f));

        totalIrradiance += probeIrradiance * weight;
        totalWeight += weight;
    }

    if (totalWeight > 0.0f) {
        totalIrradiance /= totalWeight;
    }

    // Bitwise NaN/Inf guard (survives Metal -ffast-math)
    uint3 bits = as_type<uint3>(totalIrradiance);
    if (((bits.x & 0x7F800000u) == 0x7F800000u) ||
        ((bits.y & 0x7F800000u) == 0x7F800000u) ||
        ((bits.z & 0x7F800000u) == 0x7F800000u)) {
        totalIrradiance = float3(0.0f);
    }

    return totalIrradiance;
}

fragment float4 fragmentLighting_gpuDriven(
    VertexOut in [[stage_in]],
    constant ViewData& viewData [[buffer(0)]],
    constant SceneData& sceneData [[buffer(1)]],

    texture2d<float> albedoTex [[texture(2)]],
    texture2d<float> normalTex [[texture(3)]],
    texture2d<float> ormTex [[texture(4)]],
    depth2d<float> depthTex [[texture(5)]],
    texture2d<float> shadowMap0 [[texture(6)]],
    texture2d<float> shadowMap1 [[texture(7)]],
    texture2d<float> ssaoTex [[texture(9)]],

    sampler defaultSampler [[sampler(8)]]
) {
    constexpr sampler linearSampler(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
    // depth2d 必须用 nearest：linear 插值会在几何边界混合前景/背景深度，
    // 产生不存在的深度值 → worldPos 重建偏移 → shadow 比较翻转 → 闪烁
    constexpr sampler depthSampler(coord::normalized, filter::nearest, mip_filter::none, address::clamp_to_edge);

    float2 uv = in.uv;

    // 1. Sample GBuffer
    float4 albedo = albedoTex.sample(linearSampler, uv);
    float3 normal = normalTex.sample(linearSampler, uv).xyz;
    normal = normal * 2.0 - 1.0; // Unpack [0,1] -> [-1,1]
    float depth = depthTex.sample(depthSampler, uv);

    // Discard background pixels
    if (depth >= 1.0) {
        discard_fragment();
    }

    float4 orm = ormTex.sample(linearSampler, uv);
    float roughness = orm.g;
    float metallic = orm.b;

    // Sample SSAO texture (R16_Float, 0=fully occluded, 1=unoccluded)
    // Fallback to 1.0 (no occlusion) if SSAO is not available
    float ssao = ssaoTex.sample(linearSampler, uv).r;
    if (ssao <= 0.0) ssao = 1.0;
    // Moderate AO: power curve adds depth to occluded areas
    // pow(1.5) preserves some ambient base so DDGI indirect can build on it
    float ao = pow(ssao, 1.5);

    // 2. Reconstruct World Position
    float2 ndc;
    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = 1.0 - uv.y * 2.0;

    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos4 = viewData.invViewProjection * clipPos;
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    // 3. Calculate Shadow (Cascade 0 first, then Cascade 1 if out of bounds)
    float3 N = normalize(normal);
    float3 L = normalize(sceneData.lightPos.xyz);

    // Cascade selection: try cascade 0 first, fall back to cascade 1
    float shadow = GetShadow(worldPos, N, L, sceneData.shadowMatrix0, shadowMap0, defaultSampler);
    if (shadow < 0.0) {
        // Outside cascade 0 — try cascade 1
        shadow = GetShadow(worldPos, N, L, sceneData.shadowMatrix1, shadowMap1, defaultSampler);
        if (shadow < 0.0) {
            shadow = 1.0; // Outside both cascades, fully lit
        }
    }

    // 4. Direct Lighting (Cook-Torrance PBR + shadow)
    float3 V = normalize(sceneData.viewPos.xyz - worldPos);

    // F0 for Fresnel
    float3 F0 = float3(0.04);
    F0 = mix(F0, albedo.rgb, metallic);

    float3 Lo = float3(0.0);

    // --- Direct Light (Directional) ---
    {
        float3 H = normalize(V + L);
        float3 radiance = sceneData.lightColor.rgb * shadow;

        // Cook-Torrance BRDF
        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);
        float3 F  = FresnelSchlick(max(dot(H, V), 0.0), F0);

        float3 numerator    = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
        float3 specular = numerator / denominator;

        float3 kS = F;
        float3 kD = float3(1.0) - kS;
        kD *= 1.0 - metallic;

        float NdotL = max(dot(N, L), 0.0);
        Lo += (kD * albedo.rgb / PI + specular) * radiance * NdotL;
    }

    // 5. Ambient + SSAO + Shadow
    float3 ambient = albedo.rgb * 0.03;

    // Shadow darkens ambient too: in full shadow, ambient is reduced by ~70%
    float ambientShadow = mix(1.0, 0.05, 1.0 - shadow);
    float3 color = Lo + ambient * ao * ambientShadow; // 

    // NOTE: No tone mapping here — output is HDR linear to intermediate texture.
    // Final blit shader handles tone mapping + gamma before backbuffer write.
    return float4(color, 1.0);
}

// Composite blit: scene color + SSGI indirect lighting with PBR-correct composition
// SSGI output is irradiance (incoming indirect light from nearby surfaces).
// Correct PBR: L_out = L_direct + kD * irradiance * albedo / PI
// Since we don't have receiver albedo in this pass, we use a conservative
// intensity scale. Tone mapping prevents overexposure from combined lighting.
fragment float4 fragmentBlitComposite(
    VertexOut in [[stage_in]],
    texture2d<float> sceneColor [[texture(0)]],
    texture2d<float> ssgiColor  [[texture(1)]]
) {
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
    float4 scene = sceneColor.sample(s, in.uv);
    float4 ssgi  = ssgiColor.sample(s, in.uv);

    // Indirect irradiance scaled by conservative intensity factor.
    // This approximates kD * albedo / PI averaging ~0.1-0.3 for typical PBR materials.
    float ssgiIntensity = 0.3;
    float3 indirect = ssgi.rgb * ssgiIntensity;

    // Additive: direct + indirect (PBR rendering equation)
    float3 result = scene.rgb + indirect;

    // Reinhard tone mapping (handles HDR values from direct + indirect)
    result = result / (result + 1.0);

    // Gamma correction (linear -> sRGB)
    result = pow(result, float3(1.0 / 2.2));

    return float4(result, 1.0);
}