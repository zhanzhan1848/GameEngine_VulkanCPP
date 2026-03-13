#include <metal_stdlib>
using namespace metal;

// Cluster Binning Shader
// Groups visible clusters into spatial bins for optimal rendering

struct ClusterData {
    uint cluster_id;
    float3 center;
    float radius;
    uint primitive_count;
    uint vertex_offset;
    uint index_offset;
};

struct BinningConfig {
    uint bin_size;
    uint max_bins;
    uint max_clusters_per_bin;
    uint enable_spatial_sorting;
};

struct BinData {
    volatile atomic_uint cluster_count;
    uint clusters[256]; // Fixed size for simplicity
};

// Kernel arguments
kernel void cluster_binning_kernel(
    device const ClusterData* clusters [[buffer(0)]],
    constant BinningConfig& config [[buffer(1)]],
    device BinData* bins [[buffer(2)]],
    device atomic_uint* bin_counter [[buffer(3)]],
    uint global_id [[thread_position_in_grid]])
{
    if (global_id >= 10000) return; // Max clusters check

    ClusterData cluster = clusters[global_id];

    // Calculate screen space position
    float4 clip_pos = float4(cluster.center, 1.0f);
    // TODO: Apply view-projection transform

    // Calculate bin index
    uint2 screen_pos = uint2(clip_pos.x, clip_pos.y);
    uint bin_index = (screen_pos.y / config.bin_size) * (1920 / config.bin_size) +
                     (screen_pos.x / config.bin_size);

    if (bin_index >= config.max_bins) return;

    // Atomic increment cluster count for this bin
    uint cluster_slot = atomic_fetch_add_explicit(&bins[bin_index].cluster_count, 1, memory_order_relaxed);

    if (cluster_slot < config.max_clusters_per_bin) {
        bins[bin_index].clusters[cluster_slot] = cluster.cluster_id;
    }
}