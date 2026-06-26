// AnalyticSDF.metal — ISDFDataProvider implementation for analytic primitives.
// Writes length(worldPos - center) - radius to a cascade's R16_Float 3D texture.
// Used by AnalyticSDFProvider as the simplest Strategy implementation:
// no geometry source required, just a CPU-side shape description.
//
// Dispatched once per cascade per frame by AnalyticSDFProvider::DispatchCascade.
// Reads cascade.origin / voxel_size from params CB; worldPos computed as
// origin + (voxel_id + 0.5) * voxel_size so texel centers match what
// GlobalSDF::DebugFill computes on the CPU (mirror the math exactly).

#include <metal_stdlib>
using namespace metal;

// Must match AnalyticSDFParamsCB in AnalyticSDFProvider.cpp.
// Use packed_float3 (12B) so the Metal struct layout matches the C++ CB
// exactly: a regular float3 has 16-byte alignment in Metal structs, which
// would push radius to offset 16 and origin/voxel_size past the 32-byte CB.
struct AnalyticSDFParams {
    packed_float3 center;       // offset 0-11
    float         radius;       // offset 12-15
    packed_float3 origin;       // offset 16-27 (cascade origin for this frame)
    float         voxel_size;   // offset 28-31
};

kernel void analytic_sdf_fill(
    texture3d<float, access::write> sdfTexture [[texture(0)]],
    constant AnalyticSDFParams& params         [[buffer(0)]],
    uint3 tid [[thread_position_in_grid]])
{
    uint3 dims = uint3(sdfTexture.get_width(),
                       sdfTexture.get_height(),
                       sdfTexture.get_depth());

    if (any(tid >= dims)) return;

    // Match DebugFill convention: voxel CENTER write. SurfaceNets samples with
    // linear filtering so the half-voxel offset becomes a small smoothing.
    float3 worldPos = params.origin + (float3(tid) + 0.5f) * params.voxel_size;
    float3 d = worldPos - params.center;
    float dist = length(d) - params.radius;

    // R16_Float texture — hardware converts float4.r to half on write.
    // Matches the write pattern used by GlobalSDFVoxelization.metal.
    sdfTexture.write(float4(dist, 0.0f, 0.0f, 0.0f), tid);
}
