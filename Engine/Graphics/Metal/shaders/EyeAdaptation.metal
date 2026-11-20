#include <metal_stdlib>
using namespace metal;

// Native counterpart of frozen EyeAdaptation.comp: one 16x16 workgroup,
// 4096 sparse samples, log mean and direction-dependent temporal adaptation.
struct AdaptParams {
    float rateUp, rateDown, minLogLum, maxLogLum;
    float brightClip, cutSnap;
    float2 pad;
};
static_assert(sizeof(AdaptParams) == 32, "Adaptation CPU/MSL ABI");
static_assert(__builtin_offsetof(AdaptParams, brightClip) == 16, "Adaptation clip ABI");

kernel void eye_adaptation(texture2d<float, access::read> hdrScene [[texture(0)]],
                           texture2d<float, access::read> previousState [[texture(1)]],
                           texture2d<float, access::write> newState [[texture(2)]],
                           constant AdaptParams& p [[buffer(3)]],
                           uint2 gid [[thread_position_in_threadgroup]],
                           uint lane [[thread_index_in_threadgroup]]) {
    threadgroup float logLum[256];
    const uint2 dims(hdrScene.get_width(), hdrScene.get_height());
    const uint2 tile = max(dims / 16u, uint2(1));
    const uint2 origin = gid * tile;
    float sum = 0;
    for (uint y = 0; y < 4; ++y) for (uint x = 0; x < 4; ++x) {
        const uint2 pixel = min(origin + uint2(x,y)*8u + tile/8u, dims-1u);
        const float3 color = hdrScene.read(pixel).rgb;
        const float lum = all(isfinite(color)) ? dot(max(color,float3(0)),float3(.2126,.7152,.0722)) : 0;
        sum += log2(max(min(lum,p.brightClip),1e-4f));
    }
    logLum[lane] = sum / 16;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint step = 128; step; step >>= 1) {
        if (lane < step) logLum[lane] += logLum[lane+step];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lane == 0) {
        float avg = logLum[0] / 256;
        float prev = previousState.read(uint2(0)).r;
        if (!(prev > -64 && prev < 64)) prev = 0;
        if (!(avg > -64 && avg < 64)) avg = prev;
        const float delta = avg-prev;
        float rate = delta>0 ? p.rateUp : p.rateDown;
        if (abs(delta)>p.cutSnap) rate = 1;
        const float adapted = clamp(mix(prev,avg,clamp(rate,0.0f,1.0f)),p.minLogLum,p.maxLogLum);
        newState.write(float4(adapted),uint2(0));
    }
}
