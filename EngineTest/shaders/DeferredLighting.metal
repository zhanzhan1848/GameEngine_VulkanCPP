#include <metal_stdlib>
using namespace metal;

#define RHI_ENABLE_PBR
#include "RHIShaderCommon.metal"
// DDGI v2 sampling — inlined from DDGISample.metal + DDGIVolumeData.metal
// (EngineTest shaders don't have access to Engine/Graphics include paths)
constant float _DDGI_SH_C0   = 0.282095f;
constant float _DDGI_SH_C1   = 0.488603f;
constant float _DDGI_SH_C2_0 = 1.092548f;
constant float _DDGI_SH_C2_1 = 0.315392f;
constant float _DDGI_SH_C2_2 = 0.546274f;

static void _ddgiShEvaluate(float3 d, thread float* out) {
    float x=d.x, y=d.y, z=d.z;
    float x2=x*x, y2=y*y, z2=z*z;
    out[0]= _DDGI_SH_C0;
    out[1]=-_DDGI_SH_C1*y;  out[2]= _DDGI_SH_C1*z;  out[3]=-_DDGI_SH_C1*x;
    out[4]= _DDGI_SH_C2_0*y*x; out[5]=-_DDGI_SH_C2_0*y*z;
    out[6]= _DDGI_SH_C2_1*(3.0f*z2-1.0f);
    out[7]=-_DDGI_SH_C2_0*x*z; out[8]= _DDGI_SH_C2_2*(x2-y2);
}

static float3 _ddgiShDot(thread const float3* c, float3 d) {
    float b[9]; _ddgiShEvaluate(d, b);
    float3 r(0.0f);
    for (uint i=0; i<4u; ++i) r += c[i]*b[i]; // Only L0+L1 (4 coeff) for stability
    return r;
}

static void _ddgiTetrahedral(float3 gp, uint3 gd, thread uint* pi, thread float* bw) {
    uint3 b0=clamp(uint3(floor(gp)),uint3(0u),gd-1u), b1=min(b0+1u,gd-1u);
    uint p[8];
    p[0]=b0.x+b0.y*gd.x+b0.z*gd.x*gd.y; p[1]=b1.x+b0.y*gd.x+b0.z*gd.x*gd.y;
    p[2]=b0.x+b1.y*gd.x+b0.z*gd.x*gd.y; p[3]=b1.x+b1.y*gd.x+b0.z*gd.x*gd.y;
    p[4]=b0.x+b0.y*gd.x+b1.z*gd.x*gd.y; p[5]=b1.x+b0.y*gd.x+b1.z*gd.x*gd.y;
    p[6]=b0.x+b1.y*gd.x+b1.z*gd.x*gd.y; p[7]=b1.x+b1.y*gd.x+b1.z*gd.x*gd.y;
    float fx=fract(gp.x), fy=fract(gp.y), fz=fract(gp.z);
    if      (fx>=fy&&fy>=fz) { pi[0]=p[0];pi[1]=p[1];pi[2]=p[3];pi[3]=p[7]; bw[0]=1-fx;bw[1]=fx-fy;bw[2]=fy-fz;bw[3]=fz; }
    else if (fx>=fz&&fz>=fy) { pi[0]=p[0];pi[1]=p[1];pi[2]=p[5];pi[3]=p[7]; bw[0]=1-fx;bw[1]=fx-fz;bw[2]=fz-fy;bw[3]=fy; }
    else if (fy>=fx&&fx>=fz) { pi[0]=p[0];pi[1]=p[2];pi[2]=p[3];pi[3]=p[7]; bw[0]=1-fy;bw[1]=fy-fx;bw[2]=fx-fz;bw[3]=fz; }
    else if (fy>=fz&&fz>=fx) { pi[0]=p[0];pi[1]=p[2];pi[2]=p[6];pi[3]=p[7]; bw[0]=1-fy;bw[1]=fy-fz;bw[2]=fz-fx;bw[3]=fx; }
    else if (fz>=fx&&fx>=fy) { pi[0]=p[0];pi[1]=p[4];pi[2]=p[5];pi[3]=p[7]; bw[0]=1-fz;bw[1]=fz-fx;bw[2]=fx-fy;bw[3]=fy; }
    else                     { pi[0]=p[0];pi[1]=p[4];pi[2]=p[6];pi[3]=p[7]; bw[0]=1-fz;bw[1]=fz-fy;bw[2]=fy-fx;bw[3]=fx; }
    for(uint i=0;i<4u;++i) bw[i]=max(bw[i],0.0f);
}

static float3 _ddgiSampleIrradiance(float3 wp, float3 n, float3 origin, float spacing, uint3 counts, device const float3* buf) {
    float3 gp=(wp-origin)/spacing;
    float3 gm=float3(float(counts.x-1u),float(counts.y-1u),float(counts.z-1u));
    if(any(gp<0.0f)||any(gp>gm)) return float3(0.0f);

    // Nearest probe only (1 probe × 4 float3 = 4 reads, well within Apple Silicon fragment limits)
    uint3 pc = clamp(uint3(round(gp)), uint3(0u), counts - 1u);
    uint probeIdx = pc.x + pc.y * counts.x + pc.z * counts.x * counts.y;
    uint base = probeIdx * 9u;
    float3 sh[4];
    for (uint i = 0; i < 4u; ++i) sh[i] = buf[base + i];

    float3 nn = normalize(n);
    float3 r = _ddgiShDot(sh, nn);
    return max(r, float3(0.0f));
}

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

fragment float4 fragmentBlitDDGI(
    VertexOut IN [[stage_in]],

    texture2d<float, access::sample> sceneColorTex  [[texture(0)]],
    depth2d<float, access::sample>   depthTex        [[texture(1)]],
    // texture(2) = half-res GI indirect (set by compute pass)
    texture2d<float, access::sample> giIndirectTex   [[texture(2)]],
    texture2d<float, access::sample> albedoTex       [[texture(3)]],
    texture2d<float, access::sample> normalTex       [[texture(4)]],
    // texture(5) unused (ddgiDepth now handled by compute)

    constant float4x4& invViewProj         [[buffer(0)]],
    constant float4&    probeOriginSpacing [[buffer(1)]],
    constant float4&    probeCountsSh      [[buffer(2)]]
    // NO buffer(3) — irradiance buffer now read by compute, not fragment
)
{
    float2 uv = IN.uv;

    constexpr sampler s2d(coord::normalized, address::clamp_to_edge, filter::linear);
    constexpr sampler depthS(coord::normalized, address::clamp_to_edge, filter::nearest);
    float depth = depthTex.sample(depthS, uv);

    if (depth >= 1.0f) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    // Scene color = direct lighting (from deferred pass)
    float3 sceneColor = sceneColorTex.sample(s2d, uv).rgb;

    // GI indirect from half-res compute texture
    float3 indirect = giIndirectTex.sample(s2d, uv).rgb;

    // NaN guard
    uint3 bits = as_type<uint3>(indirect);
    if (((bits.x | bits.y | bits.z) & 0x7F800000u) == 0x7F800000u) {
        indirect = float3(0.0f);
    }

    // Modulate indirect by albedo for diffuse response
    float4 albedo = albedoTex.sample(s2d, uv);
    float3 ddgiDiffuse = albedo.rgb * indirect * 0.5f;

    // Final: direct + indirect
    float3 lit = sceneColor + ddgiDiffuse;

    // Tone map + gamma
    lit = lit / (lit + float3(1.0f));
    lit = pow(lit, float3(1.0f / 2.2f));

    return float4(lit, 1.0f);
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