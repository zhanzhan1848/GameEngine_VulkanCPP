#include <metal_stdlib>
using namespace metal;

struct FluidParams {
    float4 CameraPos;
    float4 ScreenParams;
    float4 InvViewProj[4];
    float4 ViewProj[4];
    float4 FluidParams1;
    float4 FluidParams2;
    float4 FluidParams3;
    float4 DepthParams;
};

// ============================================================================
// Octahedral decoding
// ============================================================================

static float3 octDecode(float2 enc) {
    float2 n = enc * 2.0 - 1.0;
    float3 v = float3(n.x, n.y, 1.0 - abs(n.x) - abs(n.y));
    if (v.z < 0.0) {
        v.xy = (1.0 - abs(v.yx)) * select(float2(-1.0), float2(1.0), v.xy >= 0.0);
    }
    return normalize(v);
}

// ============================================================================
// Fluid Shade — Fresnel + refraction + GGX specular
// ============================================================================

static float schlickFresnel(float cosTheta, float F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

static float ggxDistribution(float NdotH, float roughness) {
    float a2 = roughness * roughness;
    a2 = a2 * a2;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (3.14159 * d * d + 1e-6);
}

kernel void fluid_shade(
    texture2d<float, access::sample>  smoothed_depth  [[texture(0)]],
    texture2d<float, access::sample>  fluid_normal    [[texture(1)]],
    texture2d<float, access::sample>  fluid_thickness [[texture(2)]],
    texture2d<float, access::sample>  scene_color     [[texture(3)]],
    texture2d<float, access::write>   output_color    [[texture(4)]],
    constant FluidParams& params                      [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= uint(params.ScreenParams.x) ||
        gid.y >= uint(params.ScreenParams.y)) return;

    constexpr sampler s(coord::pixel, filter::nearest, address::clamp_to_edge);
    constexpr sampler bilinear(coord::normalized, filter::linear, address::clamp_to_edge);

    float2 uv = (float2(gid) + 0.5) / params.ScreenParams.xy;

    float depth = smoothed_depth.sample(s, float2(gid)).r;
    if (depth <= 0.0) return;

    float4 normalEnc = fluid_normal.sample(s, float2(gid));
    if (normalEnc.a < 0.5) return;

    float thickness = fluid_thickness.sample(s, float2(gid)).r;
    float ior = params.FluidParams1.w;
    float absorption = params.FluidParams1.z;

    // Decode normal
    float3 normal = octDecode(normalEnc.xy);

    // View direction
    float3 viewDir = float3(0.0, 0.0, 1.0); // Simplified for screen-space

    // Fresnel
    float cosTheta = max(dot(normal, viewDir), 0.0);
    float F0 = pow((ior - 1.0) / (ior + 1.0), 2.0);
    float fresnel = schlickFresnel(cosTheta, F0);

    // Refraction: offset UV by normal xy
    float refractScale = 0.03;
    float2 refractUV = uv + normal.xy * refractScale;
    refractUV = clamp(refractUV, 0.0, 1.0);
    float3 refractedColor = scene_color.sample(bilinear, refractUV).rgb;

    // Specular (simplified GGX with directional light)
    float3 lightDir = normalize(float3(params.FluidParams2.z, params.FluidParams2.w, params.FluidParams3.x));
    float3 lightColor = float3(params.FluidParams3.y, params.FluidParams3.z, params.FluidParams3.w);
    float3 halfVec = normalize(lightDir + viewDir);
    float NdotH = max(dot(normal, halfVec), 0.0);
    float specular = ggxDistribution(NdotH, 0.1) * fresnel;

    // Absorption via thickness (Beer-Lambert)
    float alpha = 1.0 - exp(-absorption * max(thickness, 0.0));

    // Tint (water-like blue-green)
    float3 fluidTint = float3(0.85, 0.92, 0.98);

    // Composite
    float3 color = refractedColor * fluidTint * (1.0 - fresnel) + lightColor * specular;
    output_color.write(float4(color, alpha), gid);
}
