// SurfaceNets GPU — SDF variant (Phase 9.3b, Task 3, architectural split 2026-06-25).
//
// Single kernel fill_scalar_from_sdf runs Pass 0: writes the scalar volume
// buffer from GlobalSDF cascade textures, sampling once per grid vertex.
// Pass 1 (classify_cells, in SurfaceNetsGPU.metal) reads scalar[] as a pure
// buffer lookup with zero texture sampling, so passes 1-4 run byte-identical
// to 9.3a. The split is required because the previous combined
// classify_cells_sdf kernel sampled 8 texture3D values per cell thread —
// Apple Silicon's ~4-samples-per-thread budget returns NaN past the limit,
// and Metal fast-math assumes no NaN inputs so isnan()/x!=x sanitizers
// cannot fire.
//
// Self-contained (no #include) because GPUMesher.cpp's shader loader (LoadShaderSource,
// Engine/Graphics/PCG/GPU/GPUMesher.cpp:55-91) reads source as raw bytes and hands them
// to MetalShader::Initialize, which constructs MTL::CompileOptions with default values
// (Engine/Graphics/RHI/Platforms/Metal/MetalShader.cpp:61) and never calls
// setIncludeSearchPaths — includes cannot be resolved at runtime. Shared constants
// (SN_INVALID_ID, kCornerOffset) are redefined locally and MUST match
// SurfaceNetsGPU.metal exactly.

#include <metal_stdlib>

using namespace metal;

// Shared constants — must match SurfaceNetsGPU.metal:8 and :13 verbatim.
constant constexpr uint SN_INVALID_ID = 0xffffffffu;

constant constexpr int3 kCornerOffset[8] = {
    int3(0,0,0), int3(1,0,0), int3(1,0,1), int3(0,0,1),
    int3(0,1,0), int3(1,1,0), int3(1,1,1), int3(0,1,1),
};

// Extended uniforms (mirror SurfaceNetsUniforms + cascade data, see spec §6.2).
// IMPORTANT: the 9.3a SurfaceNetsUniforms fields must come first in the same
// order, so emit_vertices/emit_faces/write_indirect (which take the 9.3a struct)
// read correctly when given the same constant buffer.
struct SurfaceNetsSDFUniforms {
    // --- 9.3a base (offsets 0..60) ---
    uint  resolution;       // cells per axis
    uint  n;                // grid vertices per axis = resolution + 1
    uint  n2;               // n * n
    float voxel_x, voxel_y, voxel_z;
    float origin_x, origin_y, origin_z;
    float extent_x, extent_y, extent_z;
    float iso_value;
    uint  pad0, pad1, pad2;  // align to 16 bytes (matches 9.3a SurfaceNetsUniforms)
    // --- GlobalSDF cascades (offset 64+) ---
    float3 SdfOrigins[3];
    float  SdfVoxelSizes[3];
    float  SdfExtents[3];
    uint   SdfResolutions[3];
};

// Returns true if `p` is inside the cascade AABB [origin, origin + extent].
// Exclusive upper bound prevents double-sampling at cascade boundaries.
inline bool in_cascade(float3 p, float3 origin, float extent) {
    return all(p >= origin) && all(p < origin + float3(extent));
}

// Uses access::sample + filter::nearest for single-texel read via sampler
// hardware path (different silicon from access::read). If this path is stable
// but access::read fails past frame 55, the issue is the access::read hardware
// path for R16_Float textures.
inline float sample_global_sdf(
    constant SurfaceNetsSDFUniforms& u,
    texture3d<float, access::sample> t0,
    texture3d<float, access::sample> t1,
    texture3d<float, access::sample> t2,
    float3 world_pos)
{
    // Nearest filter — reads exactly 1 texel, no 2x2x2 footprint.
    constexpr sampler s(filter::nearest, address::clamp_to_edge, coord::normalized);

    if (in_cascade(world_pos, u.SdfOrigins[0], u.SdfExtents[0])) {
        float3 uvw = (world_pos - u.SdfOrigins[0]) / u.SdfExtents[0];
        return t0.sample(s, uvw).r;
    }
    if (in_cascade(world_pos, u.SdfOrigins[1], u.SdfExtents[1])) {
        float3 uvw = (world_pos - u.SdfOrigins[1]) / u.SdfExtents[1];
        return t1.sample(s, uvw).r;
    }
    if (in_cascade(world_pos, u.SdfOrigins[2], u.SdfExtents[2])) {
        float3 uvw = (world_pos - u.SdfOrigins[2]) / u.SdfExtents[2];
        return t2.sample(s, uvw).r;
    }
    return 1e6f;  // outside all cascades → solid
}

// Pass 0 (SDF variant): fill_scalar_from_sdf.
//
// One thread per grid VERTEX (n³ total). Each thread reads exactly ONE texel
// from the SDF texture3D via access::read + .read(uint3) — well below Apple
// Silicon's ~4 texel reads-per-thread budget. The previous design used
// access::sample with filter::linear, which reads 2x2x2 = 8 texels per sample
// op — 2x over budget — and returned NaN for ~92% of grid vertices past frame
// 55 once the GPU's in-flight queue stabilized. Splitting into Pass 0 + Pass 1
// eliminates:
//   (a) the over-sampling stress that caused frame 55+ NaN (linear filter
//       reading 8 texels exceeded the hardware budget)
//   (b) the WAW race on scalar[idx] (each grid vertex written by exactly
//       one thread instead of up to 8 cell threads sharing a vertex)
//
// After this pass, the 9.3a classify_cells kernel (in SurfaceNetsGPU.metal)
// reads scalar[] as a pure buffer lookup — no texture sampling — so passes
// 1-4 run byte-identical to the 9.3a path.
//
// Bindings:
//   buffer(0):  SurfaceNetsSDFUniforms uniforms
//   texture(0..2): GlobalSDF cascades (finest → coarsest)
//   buffer(1):  scalar_volume (f32[n³], write)
//
// Dispatch: n³ threads (threadgroup size 4×4×4 = 64, same as 9.3a passes).
kernel void fill_scalar_from_sdf(
    constant SurfaceNetsSDFUniforms& u  [[buffer(0)]],
    texture3d<float, access::sample> t0 [[texture(0)]],
    texture3d<float, access::sample> t1 [[texture(1)]],
    texture3d<float, access::sample> t2 [[texture(2)]],
    device float*                   scalar   [[buffer(1)]],
    uint3                           tid      [[thread_position_in_grid]])
{
    const uint n = u.n;
    if (any(tid >= uint3(n))) return;

    float3 world_pos = float3(
        u.origin_x + u.voxel_x * float(tid.x),
        u.origin_y + u.voxel_y * float(tid.y),
        u.origin_z + u.voxel_z * float(tid.z));

    // Sample the GlobalSDF cascade texture at this grid vertex's world position.
    // One sample per thread (nearest filter = single-texel read).
    float v = sample_global_sdf(u, t0, t1, t2, world_pos);

    // Defensive NaN guard (fast-math may eliminate this — see file header).
    if (isnan(v)) v = 1e6f;

    uint idx = tid.x + n * tid.y + n * n * tid.z;
    scalar[idx] = v;
}
