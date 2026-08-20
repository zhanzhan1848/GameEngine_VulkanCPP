// TAA.metal — Metal compute port of Vulkan/shaders/Lumen/TAA.comp (hand-written GLSL).
// Entry point: taa_main. Self-contained (runtime source compilation has no
// include resolution — do not add #include here).
//
// Algorithm (identical to the GLSL version):
//   1. Reproject current pixel to history UV:
//        - When depth inputs are bound (depthValid): full matrix reprojection
//          currDepth + currInvViewProj → world → prevViewProj → prevUV, and a
//          disocclusion test against the previous frame's depth history
//          (3×3 closest-depth compare, DepthHistoryManager's R32 copy).
//        - Otherwise: velocity-based fallback.
//   2. Sample 3x3 neighborhood in current color, compute min/max AABB
//   3. Convert neighborhood AABB to YCoCg space, clip history to AABB
//   4. Blend: result = mix(history_clipped, current, alpha)
//      alpha = 0.1 (slow convergence for stability); 1.0 on first frame or
//      disocclusion (history rejected)
//
// Bindings (flat slots, matching the RHI descriptor set layout):
//   texture(0) = currentColorTex (this frame's jittered HDR)
//   texture(1) = historyColorTex (last frame's resolved HDR)
//   texture(2) = velocityTex     (per-pixel motion, NDC Y-up)
//   texture(3) = outputTex       (RGBA16F, write)
//   buffer(4)  = TAAGlobals
//   texture(5) = currDepthTex    (D32 GBuffer depth)
//   texture(6) = prevDepthTex    (R32F previous-frame depth copy)

#include <metal_stdlib>
using namespace metal;

struct TAAGlobals {
    float4 screenSize;       // xy = (width, height), zw = (1/width, 1/height)
    float  invHistoryValid;  // 1.0 if history uninitialized (first frame)
    uint32_t depthValid;     // 1.0 if depth reprojection inputs are bound
    uint32_t _pad1;
    uint32_t _pad2;
    float4x4 currInvViewProj;  // this frame's inverse view-projection
    float4x4 prevViewProj;     // previous frame's view-projection
};

// Manual bilinear via 4 reads — no sampler, mirrors the GLSL samplerless path.
float3 sampleBilinear(texture2d<float, access::read> tex, float2 uv, int2 dims) {
    float2 coord = uv * float2(dims) - 0.5;
    int2 base = int2(int(floor(coord.x)), int(floor(coord.y)));
    float2 frac_ = fract(coord);
    int2 maxCoord = int2(dims.x - 1, dims.y - 1);

    int2 c00 = clamp(base,                int2(0), maxCoord);
    int2 c10 = clamp(base + int2(1, 0),   int2(0), maxCoord);
    int2 c01 = clamp(base + int2(0, 1),   int2(0), maxCoord);
    int2 c11 = clamp(base + int2(1, 1),   int2(0), maxCoord);

    float3 h00 = tex.read(uint2(c00)).rgb;
    float3 h10 = tex.read(uint2(c10)).rgb;
    float3 h01 = tex.read(uint2(c01)).rgb;
    float3 h11 = tex.read(uint2(c11)).rgb;

    float fx = clamp(frac_.x, 0.0, 1.0);
    float fy = clamp(frac_.y, 0.0, 1.0);

    return h00 * (1.0 - fx) * (1.0 - fy)
         + h10 * fx * (1.0 - fy)
         + h01 * (1.0 - fx) * fy
         + h11 * fx * fy;
}

// RGB <-> YCoCg (better chroma separation for variance clipping).
float3 rgbToYCoCg(float3 c) {
    float y  = 0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
    float co = 0.5 * c.r - 0.5 * c.b;
    float cg = -0.25 * c.r + 0.5 * c.g - 0.25 * c.b;
    return float3(y, co, cg);
}

float3 yCoCgToRGB(float3 c) {
    float y  = c.x;
    float co = c.y;
    float cg = c.z;
    float tmp = y - cg;
    return float3(tmp + co, y + cg, tmp - co);
}

// Karis-style clip history to neighborhood AABB (in YCoCg space).
float3 clipToAABB(float3 aabbMin, float3 aabbMax, float3 p) {
    float3 center  = 0.5 * (aabbMin + aabbMax);
    float3 extents = 0.5 * (aabbMax - aabbMin);
    float3 v = p - center;
    float3 unit = v / max(extents, float3(0.0001));
    float3 absUnit = abs(unit);
    float maxComp = max(absUnit.x, max(absUnit.y, absUnit.z));
    if (maxComp <= 1.0) {
        return p;  // inside AABB
    }
    return center + v / maxComp;
}

kernel void taa_main(
    texture2d<float, access::read>  currentColorTex [[texture(0)]],
    texture2d<float, access::read>  historyColorTex [[texture(1)]],
    texture2d<float, access::read>  velocityTex     [[texture(2)]],
    texture2d<float, access::write> outputTex       [[texture(3)]],
    constant TAAGlobals& globals [[buffer(4)]],
    depth2d<float, access::read>   currDepthTex    [[texture(5)]],
    texture2d<float, access::read> prevDepthTex    [[texture(6)]],
    uint2 gid [[thread_position_in_grid]])
{
    int2 dims = int2(globals.screenSize.xy);
    if (gid.x >= (uint)dims.x || gid.y >= (uint)dims.y) return;

    int2 pixel = int2(gid);

    float3 currColor = currentColorTex.read(uint2(pixel)).rgb;
    float2 velocity  = velocityTex.read(uint2(pixel)).xy;

    // Velocity is clip-space NDC (Y up). Convert to UV-space delta (Y down).
    float2 currUV    = (float2(pixel) + 0.5) * globals.screenSize.zw;
    float2 historyUV = currUV - float2(velocity.x, -velocity.y) * 0.5;

    // Depth reprojection — mirrors DeferredLighting.metal's NDC conventions
    // (NDC Y-up, z in [0,1]) and rejects disoccluded history pixels.
    bool disoccluded = false;
    if (globals.depthValid > 0u) {
        float currDepth = currDepthTex.read(uint2(pixel));

        float3 ndc = float3(currUV.x * 2.0 - 1.0, 1.0 - currUV.y * 2.0, currDepth);
        float4 worldH = globals.currInvViewProj * float4(ndc, 1.0);
        float3 world = worldH.xyz / max(worldH.w, 1e-6);
        float4 prevClip = globals.prevViewProj * float4(world, 1.0);
        float2 prevNDC = prevClip.xy / prevClip.w;
        float expectedPrevDepth = prevClip.z / prevClip.w;

        historyUV = float2(prevNDC.x * 0.5 + 0.5, 0.5 - prevNDC.y * 0.5);

        // Disocclusion: 3×3 closest-depth compare against the prev-frame copy.
        int2 prevDims = int2(prevDepthTex.get_width(), prevDepthTex.get_height());
        int2 prevPixel = int2(historyUV * float2(prevDims));
        float closestPrev = 1.0;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int2 sp = clamp(prevPixel + int2(dx, dy), int2(0), prevDims - 1);
                closestPrev = min(closestPrev, prevDepthTex.read(uint2(sp)).r);
            }
        }
        disoccluded = expectedPrevDepth > closestPrev + max(closestPrev * 0.1, 0.002);
    }

    // Out-of-bounds or uninitialized history: keep current frame only.
    float3 historyColor = currColor;
    if (globals.invHistoryValid < 0.5 &&
        historyUV.x >= 0.0 && historyUV.x <= 1.0 &&
        historyUV.y >= 0.0 && historyUV.y <= 1.0) {
        int2 histDims = int2(historyColorTex.get_width(), historyColorTex.get_height());
        historyColor = sampleBilinear(historyColorTex, historyUV, histDims);
    }

    // 3x3 neighborhood of current color (for variance clipping).
    float3 neighborMin = currColor;
    float3 neighborMax = currColor;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int2 npixel = pixel + int2(dx, dy);
            if (npixel.x < 0 || npixel.x >= dims.x ||
                npixel.y < 0 || npixel.y >= dims.y) continue;
            float3 ncolor = currentColorTex.read(uint2(npixel)).rgb;
            neighborMin = min(neighborMin, ncolor);
            neighborMax = max(neighborMax, ncolor);
        }
    }

    // Convert AABB to YCoCg space, clip history.
    // Dilate the 3×3 min/max box by 25% around its center first — thin
    // high-frequency geometry otherwise flickers under sub-pixel jitter
    // (same fix as the GLSL version).
    float3 aabbMinYC     = rgbToYCoCg(neighborMin);
    float3 aabbMaxYC     = rgbToYCoCg(neighborMax);
    float3 aabbCenter    = 0.5 * (aabbMinYC + aabbMaxYC);
    float3 aabbExtents   = 0.5 * (aabbMaxYC - aabbMinYC) * 1.25;
    aabbMinYC = aabbCenter - aabbExtents;
    aabbMaxYC = aabbCenter + aabbExtents;
    float3 historyYC     = clipToAABB(aabbMinYC, aabbMaxYC, rgbToYCoCg(historyColor));
    float3 historyClipped = yCoCgToRGB(historyYC);

    // Blend — lower alpha for stability (slower convergence, less ghosting).
    // First frame or disoccluded pixel: force alpha=1.0 (history rejected).
    float alpha = 0.1;
    if (globals.invHistoryValid > 0.5 || disoccluded) {
        alpha = 1.0;
    }
    float3 result = mix(historyClipped, currColor, alpha);

    outputTex.write(float4(result, 1.0), uint2(pixel));
}
