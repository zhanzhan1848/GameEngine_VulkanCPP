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

// ACES Filmic tone mapping
static float3 ACESFilm(float3 x) {
    float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
}

// Apply exposure + tone map + gamma
static float3 toneMap(float3 color) {
    float exposure = 1.2f;
    color *= exposure;
    color = ACESFilm(color);
    color = pow(color, float3(1.0f / 2.2f));
    return color;
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
    float2 jitter;
    float2 previousJitter;
    float  time;
    float  _timePad;
    float4 viewPos;
    float4x4 shadowMatrix0;
    float4x4 shadowMatrix1;
    // P4.2 (2026-09-15): C++ 侧扩到 4 级联（SceneData 544B）——Metal 端
    // 结构体必须同步扩容以保持 shadowParams/ambientLight 偏移一致。
    // Metal 光栅/采样路径暂只用级联 0/1（与 Vulkan 4 级联选择解耦）。
    float4x4 shadowMatrix2;
    float4x4 shadowMatrix3;
    // x = shadow mode (0 = pre-filtered R8 visibility, 1 = VSM moments +
    // Chebyshev), y = cascade 0 split distance, zw unused.
    float4 shadowParams;
    // rgb = scene ambient color, a = intensity. This remains active without
    // an HDR environment so base-color textures do not disappear in shadow.
    float4 ambientLight;
};

struct ViewData {
    float4x4 viewProjection;
    float4x4 invViewProjection;
};

// LightParameters / DirectionalLightParameters / ForwardLightBuffer come from
// RHIShaderCommon.metal (included above) and byte-match rhi::ForwardLightBuffer.

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
// Shadow Helper — reads pre-filtered visibility texture from ShadowFilter pass
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

// Sample pre-filtered shadow visibility (half-res, bilinear upsample)
float GetShadowVisibility(float2 uv, texture2d<float> shadowVisibility, sampler sm) {
    float v = shadowVisibility.sample(sm, uv).r;
    return saturate(v);
}

// VSM: project worldPos into the cascade's light clip space, sample the
// blurred (z, z²) moments, Chebyshev upper bound. Mirrors
// RHIShaderFunctions.metal ChebyshevUpperBound. NDC→UV flips Y
// (same convention as ReadRawShadowDepth above). Cascade selection is
// BOUNDS-based with fallback (0 → 1 → lit), matching the GLSL version —
// a distance split leaves a hard boundary at the cascade-0 ortho box edge.
float GetVSMShadowVisibility(constant SceneData& sceneData, float3 worldPos,
                             texture2d<float> moments0, texture2d<float> moments1) {
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);

    float4 clip0 = sceneData.shadowMatrix0 * float4(worldPos, 1.0);
    float3 sc0 = clip0.xyz / clip0.w;
    float2 uv0 = float2(sc0.x * 0.5 + 0.5, 0.5 - sc0.y * 0.5);
    bool inCascade0 = uv0.x >= 0.0 && uv0.x <= 1.0 && uv0.y >= 0.0 && uv0.y <= 1.0 &&
                      sc0.z >= 0.0 && sc0.z <= 1.0;
    if (inCascade0) {
        float2 moments = moments0.sample(s, uv0).xy;
        const float minVariance = 0.00002;
        if (sc0.z <= moments.x) return 1.0;
        float variance = moments.y - (moments.x * moments.x);
        variance = max(variance, minVariance);
        float d = sc0.z - moments.x;
        float pMax = variance / (variance + d * d);
        return smoothstep(0.05, 1.0, pMax);
    }

    float4 clip1 = sceneData.shadowMatrix1 * float4(worldPos, 1.0);
    float3 sc1 = clip1.xyz / clip1.w;
    float2 uv1 = float2(sc1.x * 0.5 + 0.5, 0.5 - sc1.y * 0.5);
    bool inCascade1 = uv1.x >= 0.0 && uv1.x <= 1.0 && uv1.y >= 0.0 && uv1.y <= 1.0 &&
                      sc1.z >= 0.0 && sc1.z <= 1.0;
    if (inCascade1) {
        float2 moments = moments1.sample(s, uv1).xy;
        const float minVariance = 0.00002;
        if (sc1.z <= moments.x) return 1.0;
        float variance = moments.y - (moments.x * moments.x);
        variance = max(variance, minVariance);
        float d = sc1.z - moments.x;
        float pMax = variance / (variance + d * d);
        return smoothstep(0.05, 1.0, pMax);
    }
    return 1.0;  // outside both shadow maps — lit
}

// ================================================================================================
// Fragment Shader (PBR Lighting)
// ================================================================================================

// Apple Silicon: texture sampling must be in separate functions
static float3 SampleAlbedo(float2 uv, texture2d<float> tex) {
    constexpr sampler s(coord::normalized, filter::nearest, mip_filter::none, address::clamp_to_edge);
    return tex.sample(s, uv).rgb;
}
static float3 SampleNormal(float2 uv, texture2d<float> tex) {
    constexpr sampler s(coord::normalized, filter::nearest, mip_filter::none, address::clamp_to_edge);
    return tex.sample(s, uv).rgb;
}
static float3 SampleORM(float2 uv, texture2d<float> tex) {
    constexpr sampler s(coord::normalized, filter::nearest, mip_filter::none, address::clamp_to_edge);
    return tex.sample(s, uv).rgb;
}
static float SampleDepth(float2 uv, texture2d<float> tex) {
    constexpr sampler s(coord::normalized, filter::nearest, mip_filter::none, address::clamp_to_edge);
    return tex.sample(s, uv).r;
}
static float SampleShadow(float2 uv, texture2d<float> tex) {
    constexpr sampler s(coord::normalized, filter::nearest, mip_filter::none, address::clamp_to_edge);
    return tex.sample(s, uv).r;
}
static float3 SampleIrradiance(float3 dir, texturecube<float> tex) {
    constexpr sampler s(filter::linear, mip_filter::linear, address::clamp_to_edge);
    return tex.sample(s, dir).rgb;
}
static float3 SamplePrefilter(float3 dir, float lod, texturecube<float> tex) {
    constexpr sampler s(filter::linear, mip_filter::linear, address::clamp_to_edge);
    return tex.sample(s, dir, level(lod)).rgb;
}
static float2 SampleBRDFLUT(float2 uv, texture2d<float> tex) {
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
    return tex.sample(s, uv).rg;
}

fragment float4 fragmentLighting_v3(
    VertexOut in [[stage_in]],
    constant ViewData& viewData [[buffer(0)]],
    constant SceneData& sceneData [[buffer(1)]],
    constant ForwardLightBuffer& lightBuffer [[buffer(13)]],

    texture2d<float> albedoTex [[texture(2)]],
    texture2d<float> normalTex [[texture(3)]],
    texture2d<float> ormTex [[texture(4)]],
    texture2d<float> depthTex [[texture(5)]],
    texture2d<float> shadowVisibility [[texture(6)]],
    texturecube<float> irradianceMap [[texture(8)]],
    texturecube<float> prefilterMap [[texture(9)]],
    texture2d<float> brdfLUT [[texture(10)]],
    texture2d<float> shadowMoments0 [[texture(13)]],
    texture2d<float> shadowMoments1 [[texture(14)]]
) {
    float2 uv = in.uv;

    float depth = SampleDepth(uv, depthTex);
    if (depth >= 1.0) return float4(0.0, 0.0, 0.0, 1.0);

    // Reconstruct world position from depth (Metal: NDC Z is [0,1], UV Y is top-left)
    float2 ndcXY = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 ndc = float4(ndcXY, depth, 1.0);
    float4 worldPosH = viewData.invViewProjection * ndc;
    float3 worldPos = worldPosH.xyz / worldPosH.w;

    // Sample GBuffer
    float3 albedo = SampleAlbedo(uv, albedoTex);
    float3 normalEnc = SampleNormal(uv, normalTex);
    float3 N = normalize(normalEnc * 2.0 - 1.0);
    float3 orm = SampleORM(uv, ormTex);
    float ao = orm.r;
    float roughness = max(orm.g, 0.04);
    float metallic = orm.b;

    // View and light directions
    float3 V = normalize(sceneData.viewPos.xyz - worldPos);
    float3 L = normalize(sceneData.lightPos.xyz);
    float3 H = normalize(V + L);

    // PBR: F0 (base reflectivity)
    float3 F0 = float3(0.04);
    F0 = mix(F0, albedo, metallic);

    // Direct lighting — Cook-Torrance BRDF
    float NdotL = max(dot(N, L), 0.0);

    // Specular
    float3 specBRDF = SpecularBRDF(N, V, L, roughness, F0);

    // Diffuse (Lambertian, energy-conserving)
    float3 kS = FresnelSchlick(max(dot(H, V), 0.0), F0);
    float3 kD = float3(1.0) - kS;
    kD *= (1.0 - metallic);

    float3 diffuse = kD * albedo / PI;

    // Direct light contribution.
    // T8.10h sync: DIRECT_INTENSITY 2.5→3.125 (+25%), the Vulkan
    // DeferredLighting.frag knob (sunlit p95 read as overcast). Scales SCREEN
    // direct only — camera light_color also drives probe rays.
    constexpr float DIRECT_INTENSITY = 3.125f;
    float3 Lo = (diffuse + specBRDF) * sceneData.lightColor.rgb * NdotL * DIRECT_INTENSITY;

    // Shadow — VSM moments + Chebyshev, or the legacy depth-compare path.
    float shadow = 1.0;
    if (sceneData.shadowParams.x > 0.5) {
        shadow = GetVSMShadowVisibility(sceneData, worldPos, shadowMoments0, shadowMoments1);
    } else {
        float4 shadowClip = sceneData.shadowMatrix0 * float4(worldPos, 1.0);
        float3 shadowCoord = shadowClip.xyz / shadowClip.w;
        shadowCoord.x = shadowCoord.x * 0.5 + 0.5;
        shadowCoord.y = shadowCoord.y * -0.5 + 0.5;
        if (shadowCoord.x >= 0.0 && shadowCoord.x <= 1.0 &&
            shadowCoord.y >= 0.0 && shadowCoord.y <= 1.0 &&
            shadowCoord.z >= 0.0 && shadowCoord.z <= 1.0) {
            float shadowDepth = SampleShadow(shadowCoord.xy, shadowVisibility);
            shadow = (shadowCoord.z - 0.005) > shadowDepth ? 0.3 : 1.0;
        }
    }
    Lo *= shadow;

    // IBL ambient
    float3 F_ibl = FresnelSchlickRoughness(max(dot(N, V), 0.0), F0, roughness);
    float3 kD_ibl = float3(1.0) - F_ibl;
    kD_ibl *= (1.0 - metallic);

    // Diffuse IBL
    float3 irradiance = SampleIrradiance(N, irradianceMap);
    float3 diffuse_ibl = kD_ibl * albedo * irradiance;

    // Specular IBL
    float3 R = reflect(-V, N);
    float MAX_REFLECTION_LOD = float(prefilterMap.get_num_mip_levels() - 1u);
    float3 prefilteredColor = SamplePrefilter(R, clamp(roughness, 0.0f, 1.0f) * MAX_REFLECTION_LOD, prefilterMap);
    float2 brdf = SampleBRDFLUT(float2(max(dot(N, V), 0.0), roughness), brdfLUT);
    float3 specular_ibl = prefilteredColor * (F_ibl * brdf.x + brdf.y);

    float3 scene_ambient = kD_ibl * albedo * sceneData.ambientLight.rgb
                         * sceneData.ambientLight.a * ao;
    float3 ambient = (diffuse_ibl + specular_ibl) * ao + scene_ambient;
    float3 color = Lo + ambient;

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
    color = toneMap(color);

    return float4(color, 1.0);
}

// Final display contract: TAA and ToneMapping have already run. Match the
// frozen Vulkan Blit.frag with one linear LDR sample and opaque output.
fragment float4 fragmentBlitLDR(
    VertexOut in [[stage_in]],
    texture2d<float> inputTex [[texture(0)]])
{
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none,
                        address::clamp_to_edge);
    return float4(inputTex.sample(s, in.uv).rgb, 1.0f);
}

fragment float4 fragmentBlitDDGI(
    VertexOut IN [[stage_in]],

    texture2d<float, access::sample> sceneColorTex  [[texture(0)]],
    depth2d<float, access::sample>   depthTex        [[texture(1)]],
    texture2d<float, access::sample> giIndirectTex   [[texture(2)]],
    texture2d<float, access::sample> albedoTex       [[texture(3)]],
    texture2d<float, access::sample> normalTex       [[texture(4)]],
    texture2d<float, access::sample> ssaoTex         [[texture(5)]],

    constant float4x4& invViewProj         [[buffer(0)]],
    constant float4&    probeOriginSpacing [[buffer(1)]],
    constant float4&    probeCountsSh      [[buffer(2)]]
)
{
    float2 uv = IN.uv;

    constexpr sampler s2d(coord::normalized, address::clamp_to_edge, filter::linear);
    constexpr sampler depthS(coord::normalized, address::clamp_to_edge, filter::nearest);

    // Scene color: RGB = direct lighting (HDR linear), A = shadow factor
    float4 sceneSample = sceneColorTex.sample(s2d, uv);
    float3 sceneColor = sceneSample.rgb;
    float directShadow = sceneSample.a;

    float depth = depthTex.sample(depthS, uv);

    // Background (sky) pass through
    if (depth >= 1.0f) {
        sceneColor = toneMap(sceneColor);
        return float4(sceneColor, 1.0f);
    }

    // GI indirect from half-res compute texture
    float3 indirect = giIndirectTex.sample(s2d, uv).rgb;

    // NaN guard
    uint3 bits = as_type<uint3>(indirect);
    if (((bits.x | bits.y | bits.z) & 0x7F800000u) == 0x7F800000u) {
        indirect = float3(0.0f);
    }

    // Fixed DDGI weight
    float ddgiWeight = 1.0f;

    // SSAO: modulate DDGI indirect with contact occlusion.
    // Use direct SSAO value (trace already applies power curve).
    float ssao = ssaoTex.sample(s2d, uv).r;
    if (ssao <= 0.0f) ssao = 1.0f;
    float ao = ssao;

    // Shadow on indirect: REMOVED — DDGI probes already contain visibility info.
    // Applying direct shadow to indirect light causes double-darkening artifacts.

    // Modulate indirect by albedo for diffuse response, with AO
    float4 albedo = albedoTex.sample(s2d, uv);
    float3 ddgiDiffuse = albedo.rgb * indirect * ddgiWeight * ao;

    // Final: direct + indirect
    float3 lit = sceneColor + ddgiDiffuse;
    lit = toneMap(lit);

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

// Frozen DeferredLighting direct-sun contract. Keep this local: the shared
// PBR library serves other passes with a different Smith remapping/clamp.
static float DeferredDistributionGGX(float3 n, float3 h, float roughness) {
    float a2 = roughness * roughness * roughness * roughness;
    float nh = max(dot(n, h), 0.f);
    float denominator = nh * nh * (a2 - 1.f) + 1.f;
    return a2 / (PI * denominator * denominator);
}

static float DeferredGeometrySmith(float3 n, float3 v, float3 l, float roughness) {
    float nv = max(dot(n, v), 0.f);
    float nl = max(dot(n, l), 0.f);
    // The frozen shader uses raw roughness as k, not (roughness + 1)^2 / 8.
    return (nv / (nv * (1.f - roughness) + roughness))
         * (nl / (nl * (1.f - roughness) + roughness));
}

static float3 DeferredSky(float3 direction, float3 sun, float3 color) {
    float3 sky = mix(float3(4.40f, 4.20f, 3.80f), float3(5.50f, 5.76f, 6.00f),
                     pow(saturate(direction.y), .45f));
    float3 reference = float3(1.f, .85f, .6f);
    float3 weights = float3(.2126f, .7152f, .0722f);
    float luminance = dot(color, weights);
    float3 halo = luminance > 1e-4f ? color * (dot(reference, weights) / luminance) : reference;
    return sky + halo * pow(saturate(dot(direction, sun)), 16.f) * 1.2f;
}

static float4 deferredDirectNative(
    VertexOut in,
    constant ViewData& viewData,
    constant SceneData& sceneData,

    texture2d<float> albedoTex,
    texture2d<float> normalTex,
    texture2d<float> ormTex,
    depth2d<float> depthTex,
    texture2d<float> shadowVisibility,
    texture2d<float> shadowMoments0,
    texture2d<float> shadowMoments1,

    sampler defaultSampler
) {
    constexpr sampler linearSampler(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
    constexpr sampler depthSampler(coord::normalized, filter::nearest, mip_filter::none, address::clamp_to_edge);

    float2 uv = in.uv;

    // 1. Sample GBuffer
    float4 albedo = albedoTex.sample(linearSampler, uv);
    float3 normal = normalTex.sample(linearSampler, uv).xyz;
    normal = normal * 2.0 - 1.0;
    float depth = depthTex.sample(depthSampler, uv);

    // Background follows the same sun-colored sky contract as frozen Vulkan.
    if (depth >= 1.f) {
        float2 ndc = float2(uv.x * 2.f - 1.f, 1.f - uv.y * 2.f);
        float4 farPosition = viewData.invViewProjection * float4(ndc, 1.f, 1.f);
        float3 direction = normalize(farPosition.xyz / max(farPosition.w, 1e-6f) - sceneData.viewPos.xyz);
        return float4(DeferredSky(direction, normalize(sceneData.lightPos.xyz), sceneData.lightColor.rgb), 1.f);
    }

    float4 orm = ormTex.sample(linearSampler, uv);
    // 2026-09-02: 读取真实 ORM——此前硬编码 roughness=0.5/metallic=0，高光
    // 按统一中等粗糙度计算，直射区大面积过曝死白（vs Vulkan frag 直射区
    // 有完整纹理层次）。与 DeferredLighting.frag 的 max(g,0.04) 约定一致。
    float roughness = max(orm.g, 0.04);
    float metallic = orm.b;

    // 2. Reconstruct World Position
    float2 ndc;
    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = 1.0 - uv.y * 2.0;

    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos4 = viewData.invViewProjection * clipPos;
    float3 worldPos = worldPos4.xyz / worldPos4.w;

    // 3. Shadow — VSM moments + Chebyshev, or pre-filtered visibility
    //    (half-res, bilinear upsample).
    float shadow;
    if (sceneData.shadowParams.x > 0.5) {
        shadow = GetVSMShadowVisibility(sceneData, worldPos, shadowMoments0, shadowMoments1);
    } else {
        shadow = GetShadowVisibility(uv, shadowVisibility, defaultSampler);
    }

    // 4. Direct Lighting (Cook-Torrance PBR + shadow)
    float3 N = normalize(normal);
    float3 L = normalize(sceneData.lightPos.xyz);
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
        float NDF = DeferredDistributionGGX(N, H, roughness);
        float G   = DeferredGeometrySmith(N, V, L, roughness);
        float3 F  = FresnelSchlick(max(dot(H, V), 0.0), F0);

        float3 numerator    = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001;
        float3 specular = numerator / denominator;

        float3 kS = F;
        float3 kD = float3(1.0) - kS;
        kD *= 1.0 - metallic;

        float NdotL = max(dot(N, L), 0.0);
        // 2026-09-05 校准参数：屏幕直射增益，与 Vulkan DeferredLighting.frag
        // 的同名旋钮同值（2.5）。GPU 驱动路径此前无此乘数——FULL_POST 的
        // ACES 下直射峰值 ~0.48 过不了感知门槛（画面呈纯环境光观感）。
        // legacy 直出时代该旋钮曾以 3.125 存在于测试副本（未同步入引擎）。
        constexpr float DIRECT_INTENSITY = 50.0f; // 2026-09-10 十七: 同步 Vulkan 20→50（C4 实验档）
        Lo += (kD * albedo.rgb / PI + specular) * radiance * NdotL * DIRECT_INTENSITY;
    }

    // 5. Ambient + SSAO + Shadow
    // DDGI replaces traditional ambient — set to zero.
    float3 ambient = float3(0.0f);

    // Shadow darkens ambient too: in full shadow, ambient is reduced by ~70%
    float ambientShadow = mix(1.0, 0.3, 1.0 - shadow);
    float3 color = Lo + ambient * ambientShadow;

    // Output: RGB = direct lighting (HDR linear), A = shadow factor for indirect modulation
    return float4(color, shadow);
}
fragment float4 fragmentLighting_gpuDriven(
    VertexOut in [[stage_in]],
    constant ViewData& viewData [[buffer(0)]],
    constant SceneData& sceneData [[buffer(1)]],

    texture2d<float> albedoTex [[texture(2)]],
    texture2d<float> normalTex [[texture(3)]],
    texture2d<float> ormTex [[texture(4)]],
    depth2d<float> depthTex [[texture(5)]],
    texture2d<float> shadowVisibility [[texture(6)]],
    texture2d<float> shadowMoments0 [[texture(13)]],
    texture2d<float> shadowMoments1 [[texture(14)]],

    sampler defaultSampler [[sampler(8)]]
) {
    return deferredDirectNative(in,viewData,sceneData,albedoTex,normalTex,ormTex,depthTex,shadowVisibility,shadowMoments0,shadowMoments1,defaultSampler);
}

// Native MRT keeps display IBL separate from direct radiance consumed by GI.
struct NativeDeferredEnvironment {float4 direct [[color(0)]]; float4 environment [[color(1)]];};
fragment NativeDeferredEnvironment fragmentLighting_gpuDrivenEnvironment(
    VertexOut in [[stage_in]],
    constant ViewData& viewData [[buffer(0)]],
    constant SceneData& sceneData [[buffer(1)]],

    texture2d<float> albedoTex [[texture(2)]],
    texture2d<float> normalTex [[texture(3)]],
    texture2d<float> ormTex [[texture(4)]],
    depth2d<float> depthTex [[texture(5)]],
    texture2d<float> shadowVisibility [[texture(6)]],
    texture2d<float> shadowMoments0 [[texture(13)]],
    texture2d<float> shadowMoments1 [[texture(14)]],

    texture2d<float> ssaoTex [[texture(7)]],
    texturecube<float> prefilterMap [[texture(11)]],
    texture2d<float> brdfLUT [[texture(12)]],
    sampler defaultSampler [[sampler(8)]]
) {
    NativeDeferredEnvironment out;
    out.direct=deferredDirectNative(in,viewData,sceneData,albedoTex,normalTex,ormTex,depthTex,shadowVisibility,shadowMoments0,shadowMoments1,defaultSampler);
    const float directShadow=out.direct.a;
    out.direct.a=1.f; // Frozen deferred MRT contract; shadow is not downstream coverage.
    out.environment=float4(0);
    constexpr sampler linear(coord::normalized,filter::linear,mip_filter::linear,address::clamp_to_edge);
    constexpr sampler nearest(coord::normalized,filter::nearest,mip_filter::none,address::clamp_to_edge);
    float depth=depthTex.sample(nearest,in.uv);
    if(depth>=1.f || sceneData.ambientLight.w<.5f)return out;
    float3 albedo=albedoTex.sample(linear,in.uv).rgb;
    float3 n=normalize(normalTex.sample(linear,in.uv).xyz*2.f-1.f);
    float3 orm=ormTex.sample(linear,in.uv).rgb;
    float4 world=viewData.invViewProjection*float4(in.uv.x*2.f-1.f,1.f-in.uv.y*2.f,depth,1.f);
    float3 v=normalize(sceneData.viewPos.xyz-world.xyz/max(world.w,1e-6f));
    float roughness=max(orm.g,.04f),nov=max(dot(n,v),0.f);
    float3 f0=mix(float3(.04f),albedo,orm.b);
    float3 f=f0+(max(float3(1.f-roughness),f0)-f0)*pow(clamp(1.f-nov,0.f,1.f),5.f);
    float lod=clamp(roughness,0.f,1.f)*float(max(prefilterMap.get_num_mip_levels(),1u)-1u);
    float3 prefiltered=prefilterMap.sample(linear,reflect(-v,n),level(lod)).rgb;
    float2 brdf=brdfLUT.sample(linear,float2(nov,roughness)).rg;
    float3 specular=prefiltered*(f*brdf.x+brdf.y)*.35f;
    // Measured mode has only material micro-occlusion here. Geometry is
    // evaluated later, and zero visibility must retain alpha ownership.
    if(sceneData.ambientLight.w>1.5f)specular*=clamp(orm.r,0.f,1.f);
    else {
        float ao=ssaoTex.sample(linear,in.uv).r;
        specular*=mix(.35f,1.f,directShadow)*(ao>0.f?ao:1.f);
    }
    out.environment=float4(specular,1.f);
    return out;
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

    // Tone map + gamma
    result = toneMap(result);

    return float4(result, 1.0);
}
// ================================================================================================
// Compute-based fusion: split into two passes to stay within Apple Silicon texture read limits.
// Each pass reads max 4 textures + 1 write (stable on Apple Silicon).
// ================================================================================================

// ================================================================================================
// Fragment-based Fusion: Apple Silicon TBDR optimized
// Reads multiple textures in fragment shader where tile cache handles bandwidth efficiently.
// ================================================================================================

// Native MSL irradiance composition matching frozen Vulkan FusionIndirect.
// The current Metal host binds no gain/mode UBO: unit display gain is used.
// With SPGI disabled its black fallback gives DDGI-only composition. When
// supplied, SPGI confidence replaces DDGI continuously, including dark samples.
fragment float4 fragmentFusionIndirect(
    VertexOut in [[stage_in]],
    texture2d<float> ssgiColor   [[texture(0)]],
    texture2d<float> ddgiColor   [[texture(1)]],
    texture2d<float> spgiColor   [[texture(2)]],
    texture2d<float> albedoTex   [[texture(3)]],
    texture2d<float> ssaoTex     [[texture(4)]])
{
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
    float4 ssgi4   = ssgiColor.sample(s, in.uv);
    float3 ddgi    = ddgiColor.sample(s, in.uv).rgb;
    float4 spgi4   = spgiColor.sample(s, in.uv);
    float3 albedo  = albedoTex.sample(s, in.uv).rgb;
    float ssgiHit = clamp(ssgi4.a, 0.0f, 30.0f);
    float ssgiConfidence = clamp(1.0f - (ssgiHit / 30.0f) * 0.7f, 0.3f, 1.0f);
    float spgiConfidence = clamp(spgi4.a, 0.0f, 0.9f);
    float3 gi = mix(ddgi, spgi4.rgb, spgiConfidence)
              + ssgi4.rgb * ssgiConfidence * mix(0.25f, 0.15f, spgiConfidence);
    // Irradiance E becomes diffuse radiance rho*E/pi. Probe visibility already
    // accounts for occlusion: no second SSAO multiplication or ambient floor.
    float3 indirect = clamp(albedo, float3(0.0f), float3(1.0f)) * gi * 0.31830988f;

    return float4(indirect, 1.0f);
}

// Fusion Pass 2 (full-res): scene + pre-combined indirect + DDGI glossy + volume scatter → tonemapped output
// T4.3: adds DDGI glossy specular (albedo/orm for F0 & roughness). Metal has no
// SSR pass — the roughness relay degrades to DDGI-glossy-only here.
fragment float4 fragmentFusion(
    VertexOut in [[stage_in]],
    texture2d<float> sceneColor    [[texture(0)]],
    texture2d<float> indirectColor [[texture(1)]],
    texture2d<float> volumeScatter [[texture(2)]],
    texture2d<float> ssrColor      [[texture(3)]],
    texture2d<float> ormTex        [[texture(4)]],
    texture2d<float> albedoTex     [[texture(5)]],
    texture2d<float> ddgiSpecular  [[texture(6)]])
{
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
    float3 scene   = sceneColor.sample(s, in.uv).rgb;
    float3 indirect = indirectColor.sample(s, in.uv).rgb;
    float4 vol     = volumeScatter.sample(s, in.uv);
    float3 albedo  = albedoTex.sample(s, in.uv).rgb;
    float4 ddgiSpec = ddgiSpecular.sample(s, in.uv);
    float4 orm     = ormTex.sample(s, in.uv);
    float4 ssr     = ssrColor.sample(s, in.uv);

    // T4.3 glossy: analytic split-sum (Karis mobile approximation of the
    // preintegrated DFG LUT — pure ALU, same formula on all backends).
    float roughness = clamp(orm.g, 0.04f, 1.0f);
    float metallic  = orm.b;
    float NoV = clamp(ddgiSpec.a, 0.05f, 1.0f);
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    float4 r = float4(roughness) * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28f * NoV)) * r.x + r.y;
    float2 ab = float2(-1.04f, 1.04f) * a004 + r.zw;
    float3 F0 = mix(float3(0.04f), albedo, metallic);
    float3 reflection = ddgiSpec.rgb * (F0 * ab.x + ab.y);
    // (1 - roughness²) suppresses the double count with the diffuse indirect —
    // the widened lobe converges to E(N) which FusionIndirect already adds.
    reflection *= (1.0f - roughness * roughness) * 0.75f;
    // Host slots 0..6 match FusionCompositeModule. SSR RGB already carries
    // its producer strength; alpha only relinquishes the glossy fallback.
    reflection = ssr.rgb + reflection * (1.0f - clamp(ssr.a, 0.0f, 1.0f));

    // Beer-Lambert: attenuate scene by transmittance, add scattered light.
    // T8.13 sync: output HDR linear — Vulkan FusionComposite.frag parity
    // ("tonemap is handled by FinalBlit to avoid double tonemap"). The chain
    // continues through TAA → Bloom → ToneMappingPass (the single ACES+gamma).
    // B10: indirect diffuse carries (1-F)·(1-metallic) — the direct chain's
    // kD=(1-kS) energy split was missing here, over-brightening diffuse GI
    // exactly where the specular lobe (sharing the same energy) peaks.
    float3 F = F0 + (float3(1.0f) - F0) * pow(1.0f - NoV, 5.0f);
    float kD = 1.0f - metallic;
    float3 result = (scene + indirect * kD * (float3(1.0f) - F) + reflection) * vol.a + vol.rgb;
    result = clamp(result, float3(0.0f), float3(64.0f));

    return float4(result, 1.0f);
}

// Pass 1: compute indirect lighting from GI sources (DDGI + SPGI + SSGI)
// Uses access::read + read() matching SSGI's stable pattern (not access::sample)
kernel void computeFusionIndirect(
    texture2d<float, access::read> ssgiColor   [[texture(0)]],
    texture2d<float, access::read> ddgiColor   [[texture(1)]],
    texture2d<float, access::read> spgiColor   [[texture(2)]],
    texture2d<float, access::read> albedoTex   [[texture(3)]],
    texture2d<float, access::write> indirectOut [[texture(4)]],
    uint2 gid [[thread_position_in_grid]])
{
    uint w = indirectOut.get_width();
    uint h = indirectOut.get_height();
    if (gid.x >= w || gid.y >= h) return;

    float4 ssgi4  = ssgiColor.read(gid);
    float3 albedo = albedoTex.read(gid).rgb;
    float4 spgi4  = spgiColor.read(gid);

    // DDGI is half-resolution: read at half pixel coords
    uint2 ddgiCoord = gid / 2u;
    uint ddgiW = ddgiColor.get_width();
    uint ddgiH = ddgiColor.get_height();
    float3 ddgi = (ddgiCoord.x < ddgiW && ddgiCoord.y < ddgiH)
        ? ddgiColor.read(ddgiCoord).rgb : float3(0.0f);

    float3 ssgi_irr = ssgi4.rgb;
    float  ssgi_hit = ssgi4.a;

    albedo = clamp(albedo, float3(0.0f), float3(1.0f));

    float ssgi_hit_norm = clamp(ssgi_hit, 0.0f, 30.0f) / 30.0f;
    float ssgi_conf = saturate(1.0f - ssgi_hit_norm * 0.7f);
    float spgi_w = saturate(spgi4.a * 2.0f);

    // Unified three-source blend (T2.3 — matches Vulkan FusionIndirect.frag)
    float3 indirect = albedo *
        (ddgi * 0.8f * (1.0f - spgi_w)
       + ssgi_irr * ssgi_conf * 0.2f * (1.0f - spgi_w)
       + spgi4.rgb * spgi_w * 1.25f) * 0.7f;

    indirectOut.write(float4(indirect, 1.0f), gid);
}

// Pass 2: composite direct + indirect + volume scatter, tone map to output
kernel void computeFusionComposite(
    texture2d<float, access::read> sceneColor  [[texture(0)]],
    texture2d<float, access::read> indirectTex [[texture(1)]],
    texture2d<float, access::read> ssaoTex     [[texture(2)]],
    texture2d<float, access::write>  outputTex   [[texture(3)]],
    texture2d<float, access::sample> volumeScatter [[texture(4)]],
    uint2 gid [[thread_position_in_grid]])
{
    uint w = outputTex.get_width();
    uint h = outputTex.get_height();
    if (gid.x >= w || gid.y >= h) return;

    float3 scene   = sceneColor.read(gid).rgb;
    float3 indirect = indirectTex.read(gid).rgb;

    // Volume scatter (half-res, bilinear sample at full-res UV)
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none, address::clamp_to_edge);
    float2 uv = (float2(gid) + 0.5f) / float2(w, h);
    float4 vol = volumeScatter.sample(s, uv);

    float3 result = (scene + indirect) * vol.a + vol.rgb;
    result = clamp(result, float3(0.0f), float3(64.0f));
    result = toneMap(result);

    outputTex.write(float4(result, 1.0f), gid);
}

// FinalBlit pass-through blit with FXAA (1x budget anti-aliasing). Samples the
// fusion module's already-tone-mapped LDR output — no second tonemap here.
fragment float4 fragmentBlitFXAA(
    VertexOut in [[stage_in]],
    texture2d<float> inputTex [[texture(0)]])
{
    constexpr sampler s(coord::normalized, filter::linear, mip_filter::none,
                        address::clamp_to_edge);
    float2 texel = 1.0f / float2(inputTex.get_width(), inputTex.get_height());
    float3 rgbM  = inputTex.sample(s, in.uv).rgb;
    float3 rgbNW = inputTex.sample(s, in.uv + texel * float2(-1.0f, -1.0f)).rgb;
    float3 rgbNE = inputTex.sample(s, in.uv + texel * float2( 1.0f, -1.0f)).rgb;
    float3 rgbSW = inputTex.sample(s, in.uv + texel * float2(-1.0f,  1.0f)).rgb;
    float3 rgbSE = inputTex.sample(s, in.uv + texel * float2( 1.0f,  1.0f)).rgb;

    constexpr float3 lumaWeights(0.299f, 0.587f, 0.114f);
    float lumaM  = dot(rgbM,  lumaWeights);
    float lumaNW = dot(rgbNW, lumaWeights);
    float lumaNE = dot(rgbNE, lumaWeights);
    float lumaSW = dot(rgbSW, lumaWeights);
    float lumaSE = dot(rgbSE, lumaWeights);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));
    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) *
                          (0.25f / 8.0f), 1.0f / 128.0f);
    float rcpDirMin = 1.0f / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, float2(-8.0f), float2(8.0f)) * texel;

    float3 rgbA = 0.5f * (
        inputTex.sample(s, in.uv + dir * (1.0f / 3.0f - 0.5f)).rgb +
        inputTex.sample(s, in.uv + dir * (2.0f / 3.0f - 0.5f)).rgb);
    float3 rgbB = rgbA * 0.5f + 0.25f * (
        inputTex.sample(s, in.uv + dir * -0.5f).rgb +
        inputTex.sample(s, in.uv + dir *  0.5f).rgb);
    float lumaB = dot(rgbB, lumaWeights);
    return float4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0f);
}

// Environment-aware native composition. Legacy fragmentFusion remains usable
// by applications that still bind the original seven-texture contract.
fragment float4 fragmentFusionEnvironment(
    VertexOut in [[stage_in]],
    texture2d<float> sceneColor [[texture(0)]],
    texture2d<float> indirectColor [[texture(1)]],
    texture2d<float> volumeScatter [[texture(2)]],
    texture2d<float> ssrColor [[texture(3)]],
    texture2d<float> ormTex [[texture(4)]],
    texture2d<float> albedoTex [[texture(5)]],
    texture2d<float> ddgiSpecular [[texture(6)]],
    texture2d<float> reflSpecular [[texture(8)]],
    texture2d<float> environmentSpecular [[texture(9)]],
    constant float4& environmentParams [[buffer(10)]],
    texture2d<float> environmentVisibility [[texture(11)]]) {
    constexpr sampler s(coord::normalized,filter::linear,mip_filter::none,address::clamp_to_edge);
    float3 scene=sceneColor.sample(s,in.uv).rgb;
    float3 indirect=indirectColor.sample(s,in.uv).rgb;
    float4 vol=volumeScatter.sample(s,in.uv),ssr=ssrColor.sample(s,in.uv),orm=ormTex.sample(s,in.uv);
    float3 albedo=albedoTex.sample(s,in.uv).rgb;
    float4 ddgi=ddgiSpecular.sample(s,in.uv),off=reflSpecular.sample(s,in.uv),environment=environmentSpecular.sample(s,in.uv);
    float roughness=clamp(orm.g,.04f,1.f),nov=clamp(ddgi.a,.05f,1.f);
    float4 r=roughness*float4(-1.f,-.0275f,-.572f,.022f)+float4(1.f,.0425f,1.04f,-.04f);
    float a004=min(r.x*r.x,exp2(-9.28f*nov))*r.x+r.y;
    float2 ab=float2(-1.04f,1.04f)*a004+r.zw;
    float3 f0=mix(float3(.04f),albedo,orm.b);
    float3 glossy=ddgi.rgb*(f0*ab.x+ab.y)*(1.f-roughness*roughness)*.75f;
    float ssrW=clamp(ssr.a,0.f,1.f),offW=clamp(off.a,0.f,1.f);
    float environmentW=environmentParams.x*clamp(environment.a,0.f,1.f);
    float visibility=environmentParams.w>.5f?clamp(environmentVisibility.sample(s,in.uv).r,0.f,1.f):1.f;
    environment.rgb*=visibility;
    float3 fallback=mix(glossy,environment.rgb*environmentParams.y,environmentW);
    float3 reflection=ssr.rgb+off.rgb*offW*(1.f-ssrW)+fallback*(1.f-ssrW)*(1.f-offW);
    if(environmentParams.z>.5f){indirect=float3(0);reflection=environment.rgb*environmentParams.y*environmentW;}
    float3 f=f0+(float3(1)-f0)*pow(1.f-nov,5.f);
    float3 result=(scene+indirect*(1.f-orm.b)*(float3(1)-f)+reflection)*vol.a+vol.rgb;
    return float4(clamp(result,float3(0),float3(64)),1);
}
