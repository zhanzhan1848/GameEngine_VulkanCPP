/**
 * @file ScreenProbeSpatialFilter.metal
 * @brief Screen Probe GI - Bilateral spatial filter on probe SH coefficients
 *
 * Cross-pattern 5-probe bilateral blur: center + 4 cardinal neighbors.
 * Smooths probe grid boundaries while preserving edges at depth discontinuities.
 *
 * Buffer strategy:
 *   Reads from inputSH (independent temporal scratch)
 *   Writes to outputSH (current frame history slot)
 *   Gather subsequently reads from outputSH
 *
 * Measured mode additionally validates normal agreement and scene visibility.
 *
 * Dispatch: (totalProbes + 63) / 64, 1, 1
 * ThreadGroupSize: (64, 1, 1)
 */

#include <metal_stdlib>
#include "GeometryFirstHit.metal"
using namespace metal;

struct SpatialConstants {
    uint  totalProbes;
    uint  gridW;
    float sigma;       // bilateral spatial sigma (world-space)
    uint flags;       // bit0 measured, bit1 current geometry ready
    uint4 geometryCounts;
};

static_assert(sizeof(SpatialConstants)==32, "Metal spatial constants ABI");

static bool measuredSpatialConnection(uint center, uint neighbor, float3 p, float3 q,
    constant SpatialConstants& params, device const float4* normals,
    device const GeometryNode* top, device const GeometryNode* bottom,
    device const GeometryTriangle* triangles, device const GeometryInstance* instances) {
    if ((params.flags & 2u)==0u) return false;
    float3 n=normals[center].xyz, m=normals[neighbor].xyz;
    if (!all(isfinite(n)) || !all(isfinite(m)) || dot(n,n)<0.25f || dot(m,m)<0.25f) return false;
    n=normalize(n); m=normalize(m);
    if (dot(n,m)<0.5f) return false;
    const float3 a=p+n*0.01f, b=q+m*0.01f, delta=b-a;
    const float dist=length(delta);
    if (!isfinite(dist) || dist<=0.002f) return false;
    GeometryHit hit;
    // Raw segment result: a nonzero near distance is intentional here. Only
    // proven miss permits interpolation; hits and incomplete queries reject.
    return geometryFirstHit(a,delta/dist,0.001f,dist-0.001f,
        top,bottom,triangles,instances,params.geometryCounts,hit)==0;
}

kernel void screen_probe_spatial_filter(
    // Input SH coefficients (4 float4 per probe, interleaved)
    device const float4* inputSH      [[buffer(0)]],

    // Output filtered SH (4 float4 per probe, interleaved)
    device float4*       outputSH     [[buffer(1)]],

    // Probe positions for bilateral weighting
    device const float4* probePos     [[buffer(2)]],

    // Constants
    constant SpatialConstants& params [[buffer(3)]],

    device const float4* probeNormals [[buffer(4)]],
    device const GeometryNode* geometryTop [[buffer(5)]],
    device const GeometryNode* geometryBottom [[buffer(6)]],
    device const GeometryTriangle* geometryTriangles [[buffer(7)]],
    device const GeometryInstance* geometryInstances [[buffer(8)]],
    uint global_id [[thread_position_in_grid]])
{
    if (global_id >= params.totalProbes) return;
    const bool measured=(params.flags & 1u)!=0u;

    uint gx = global_id % params.gridW;
    uint gy = global_id / params.gridW;

    // Read center probe position
    float4 centerPosData = probePos[global_id];

    // If center probe is inactive, output zeros
    if (centerPosData.w <= 0.0f || (measured && !all(isfinite(centerPosData)))) {
        uint base = global_id * 4;
        outputSH[base + 0] = float4(0.0f);
        outputSH[base + 1] = float4(0.0f);
        outputSH[base + 2] = float4(0.0f);
        outputSH[base + 3] = float4(0.0f);
        return;
    }

    // Read center SH
    uint centerBase = global_id * 4;
    float3 centerSH[4];
    for (int b = 0; b < 4; b++) {
        centerSH[b] = inputSH[centerBase + b].rgb;
    }

    bool finiteCenter=true;
    if (measured) for (uint b=0;b<4;++b) finiteCenter &= all(isfinite(inputSH[centerBase+b]));
    // Check if center SH is valid
    if (inputSH[centerBase + 0].w <= 0.0f || !finiteCenter) {
        // Invalid center: output zeros
        outputSH[centerBase + 0] = float4(0.0f);
        outputSH[centerBase + 1] = float4(0.0f);
        outputSH[centerBase + 2] = float4(0.0f);
        outputSH[centerBase + 3] = float4(0.0f);
        return;
    }

    float3 centerPos = centerPosData.xyz;
    float invTwoSigmaSq = 1.0f / (2.0f * params.sigma * params.sigma);

    // Cardinal neighbor offsets (cross pattern)
    int2 offsets[4] = {
        int2(-1, 0),  // left
        int2( 1, 0),  // right
        int2( 0,-1),  // up
        int2( 0, 1),  // down
    };

    // Accumulate filtered SH + confidence
    // Center-weighted: center weight = 2.0 (strong self-preservation, light neighbor influence)
    float3 filteredSH[4] = { centerSH[0] * 2.0f, centerSH[1] * 2.0f, centerSH[2] * 2.0f, centerSH[3] * 2.0f };
    float totalWeight = 2.0f;  // center weight = 2.0
    float centerConf = inputSH[centerBase + 0].w;
    float filteredConf = centerConf * 2.0f;  // center weight = 2.0

    for (int n = 0; n < 4; n++) {
        int nx = int(gx) + offsets[n].x;
        int ny = int(gy) + offsets[n].y;

        // Bounds check
        if (nx < 0 || nx >= int(params.gridW) || ny < 0) continue;
        if (uint(ny) * params.gridW + uint(nx) >= params.totalProbes) continue;

        uint nidx = uint(ny) * params.gridW + uint(nx);

        // Check neighbor active
        float4 neighborPosData = probePos[nidx];
        if (neighborPosData.w <= 0.0f || (measured && !all(isfinite(neighborPosData)))) continue;

        // Check neighbor SH valid
        uint neighborBase = nidx * 4;
        if (inputSH[neighborBase + 0].w <= 0.0f) continue;

        if (measured) {
            bool finiteNeighbor=true;
            for (uint b=0;b<4;++b) finiteNeighbor &= all(isfinite(inputSH[neighborBase+b]));
            if (!finiteNeighbor || !measuredSpatialConnection(global_id,nidx,centerPos,neighborPosData.xyz,
                params,probeNormals,geometryTop,geometryBottom,geometryTriangles,geometryInstances)) continue;
        }

        // Bilateral weight based on world-space distance
        float dist2 = distance_squared(centerPos, neighborPosData.xyz);
        float w = exp(-dist2 * invTwoSigmaSq);

        // Read and accumulate neighbor SH
        for (int b = 0; b < 4; b++) {
            filteredSH[b] += inputSH[neighborBase + b].rgb * w;
        }
        // Accumulate confidence from neighbor
        filteredConf += inputSH[neighborBase + 0].w * w;
        totalWeight += w;
    }

    // Normalize and write output
    float invWeight = 1.0f / totalWeight;
    uint outBase = global_id * 4;
    outputSH[outBase + 0] = float4(filteredSH[0] * invWeight, filteredConf * invWeight);
    outputSH[outBase + 1] = float4(filteredSH[1] * invWeight, 1.0f);
    outputSH[outBase + 2] = float4(filteredSH[2] * invWeight, 1.0f);
    outputSH[outBase + 3] = float4(filteredSH[3] * invWeight, 1.0f);
}
