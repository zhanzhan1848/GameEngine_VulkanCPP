#include <metal_stdlib>
using namespace metal;

// GPU scatter compute shader for PCG point generation.
// Each thread processes one candidate grid cell, evaluates noise density,
// and writes accepted points to an output buffer via atomic counter.
//
// Buffer layout:
//   [0] output positions: float4(x, y, z, density) — N elements max
//   [1] atomic counter: uint — number of accepted points
//   [2] params: ScatterParams
//   [3] noise input: float2 → float lookup table (optional, for precomputed noise)

struct ScatterParams {
    float bounds_min_x, bounds_min_y, bounds_min_z;
    float bounds_max_x, bounds_max_y, bounds_max_z;
    uint grid_x, grid_z;
    float cell_x, cell_z;
    float y_position;
    uint max_output;
    uint seed;
    float density_threshold;
    uint use_noise_field;  // 0 = uniform, 1 = use on-device noise
};

// Simple hash for GPU random
uint pcg_hash(uint input) {
    uint state = input * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float hash_to_float(uint h) {
    return float(h) / 4294967295.0;
}

// Simple 2D value noise for GPU
float gpu_noise_2d(float x, float y, uint seed) {
    int ix = int(floor(x));
    int iy = int(floor(y));

    auto corner = [&](int cx, int cy) -> float {
        uint h = pcg_hash(seed + uint(cx) * 374761393u + uint(cy) * 668265263u);
        return hash_to_float(h);
    };

    float fx = x - float(ix);
    float fy = y - float(iy);
    fx = fx * fx * (3.0 - 2.0 * fx);
    fy = fy * fy * (3.0 - 2.0 * fy);

    float v00 = corner(ix, iy);
    float v10 = corner(ix + 1, iy);
    float v01 = corner(ix, iy + 1);
    float v11 = corner(ix + 1, iy + 1);

    float x0 = mix(v00, v10, fx);
    float x1 = mix(v01, v11, fx);
    return mix(x0, x1, fy);
}

// FBM noise on GPU
float gpu_fbm(float x, float y, uint octaves, float lacunarity, float persistence, uint seed) {
    float value = 0.0;
    float amplitude = 1.0;
    float frequency = 1.0;
    float max_amp = 0.0;

    for (uint i = 0; i < octaves; ++i) {
        value += amplitude * (gpu_noise_2d(x * frequency, y * frequency, seed + i) * 2.0 - 1.0);
        max_amp += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }

    return value / max_amp;
}

kernel void pcg_scatter(
    device float4* output_positions [[buffer(0)]],
    device atomic_uint* output_counter [[buffer(1)]],
    constant ScatterParams& params [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint idx = global_id.x;
    uint total_cells = params.grid_x * params.grid_z;
    if (idx >= total_cells) return;

    uint ix = idx % params.grid_x;
    uint iz = idx / params.grid_x;

    // Base grid position
    float base_x = params.bounds_min_x + (float(ix) + 0.5) * params.cell_x;
    float base_z = params.bounds_min_z + (float(iz) + 0.5) * params.cell_z;

    // Jitter
    uint h = pcg_hash(params.seed + idx);
    float jx = (hash_to_float(h) - 0.5) * params.cell_x * 0.9;
    h = pcg_hash(h);
    float jz = (hash_to_float(h) - 0.5) * params.cell_z * 0.9;

    float px = base_x + jx;
    float pz = base_z + jz;
    float py = params.y_position;

    // Density evaluation
    float density = 1.0;
    if (params.use_noise_field != 0) {
        float noise_val = gpu_fbm(px * 0.02, pz * 0.02, 4, 2.0, 0.5, params.seed);
        density = 0.5 + 0.5 * noise_val; // remap [-1,1] → [0,1]
    }

    // Probability test
    h = pcg_hash(h + idx);
    float threshold = hash_to_float(h);
    if (threshold >= density * params.density_threshold) return;

    // Atomic append
    uint out_idx = atomic_fetch_add_explicit(output_counter, 1u, memory_order_relaxed);
    if (out_idx < params.max_output) {
        output_positions[out_idx] = float4(px, py, pz, density);
    }
}
