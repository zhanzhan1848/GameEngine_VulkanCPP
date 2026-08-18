// ShadowMoments.metal — VSM moment writer, Metal counterpart of
// Vulkan/shaders/Nanite/ShadowMoments.frag. Entry point: shadow_moments_fs.
// Pairs with ShadowDepth.metal's shadow_depth_vs (position-only output).
#include <metal_stdlib>
using namespace metal;

struct MomentOut {
    float2 moments [[color(0)]];
};

fragment MomentOut shadow_moments_fs(float4 fragPosition [[position]]) {
    float z = fragPosition.z;  // Metal depth range [0,1], same as Vulkan
    MomentOut out;
    out.moments = float2(z, z * z);
    return out;
}
