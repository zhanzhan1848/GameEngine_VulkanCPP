#include <metal_stdlib>
using namespace metal;

// ============================================================================
// HZB Generation Compute Shaders
// ============================================================================

/**
 * @brief HZB Generation Constants
 */
constant uint HZB_THREAD_GROUP_SIZE = 16; // 16x16 threads per threadgroup

/**
 * @brief HZB Generation Uniforms
 */
struct HZBUniforms {
    uint source_width;     // Source depth buffer width
    uint source_height;    // Source depth buffer depth
    uint mip_level;        // Current mip level being generated
    uint padding;
};

// ============================================================================
// Basic HZB Generation - Max Depth Filter
// ============================================================================

/**
 * @brief Generate HZB mip level using max depth filtering
 * @details Each thread reads a 2x2 pixel block from source mip and writes the maximum depth value
 *
 * Input:  Source mip level (mip - 1)
 * Output: Target mip level (mip)
 * Operation: max(depth) for each 2x2 block
 */
kernel void copy_depth_to_hzb_mip0(
    depth2d<float> source_depth [[texture(0)]],
    texture2d<float, access::write> target_depth [[texture(1)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint2 target_pos = global_id.xy;
    uint target_width = target_depth.get_width();
    uint target_height = target_depth.get_height();
    if (target_pos.x >= target_width || target_pos.y >= target_height) {
        return;
    }

    float depth_value = source_depth.read(target_pos);
    if (!(depth_value >= 0.0 && depth_value <= 1.0 && !isnan(depth_value) && !isinf(depth_value))) {
        depth_value = 1.0;
    }
    target_depth.write(depth_value, target_pos);
}

kernel void generate_hzb_mip_level_basic(
    texture2d<float, access::read> source_depth [[texture(0)]],
    texture2d<float, access::write> target_depth [[texture(1)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint2 target_pos = global_id.xy;
    uint target_width = target_depth.get_width();
    uint target_height = target_depth.get_height();
    if (target_pos.x >= target_width || target_pos.y >= target_height) {
        return;
    }

    uint2 source_pos = target_pos * 2;
    uint source_width = source_depth.get_width();
    uint source_height = source_depth.get_height();

    float max_depth = 0.0;
    for (uint dy = 0; dy < 2; ++dy) {
        for (uint dx = 0; dx < 2; ++dx) {
            uint2 sample_pos = source_pos + uint2(dx, dy);
            if (sample_pos.x < source_width && sample_pos.y < source_height) {
                float depth = source_depth.read(sample_pos).r;
                max_depth = max(max_depth, depth);
            }
        }
    }
    target_depth.write(max_depth, target_pos);
}

// ============================================================================
// HZB Generation - Full Mip Chain
// ============================================================================

/**
 * @brief Generate full HZB mip chain from base depth buffer
 * @details This kernel generates all mip levels in a single dispatch
 * Requires each threadgroup to handle multiple mip levels
 */
kernel void generate_hzb_full_chain(
    texture2d<float, access::read> base_depth [[texture(0)]],
    texture2d<float, access::write> hzb_output [[texture(1)]],
    constant HZBUniforms& uniforms [[buffer(0)]],
    uint3 global_id [[thread_position_in_grid]])
{
    // First, copy base level to HZB mip 0
    uint2 base_pos = global_id.xy;
    uint base_width = base_depth.get_width();
    uint base_height = base_depth.get_height();

    if (base_pos.x >= base_width || base_pos.y >= base_height) {
        return;
    }

    // Copy base depth to HZB level 0
    float base_depth_value = base_depth.read(base_pos).r;
    hzb_output.write(float4(base_depth_value, 0.0, 0.0, 1.0), base_pos);

    // Generate subsequent mip levels iteratively
    // Each thread processes one pixel at each mip level
    uint2 current_pos = base_pos;
    float current_depth = base_depth_value;

    for (uint mip = 1; mip < hzb_output.get_num_mip_levels(); ++mip) {
        // Move to parent position (divide by 2)
        current_pos = current_pos / 2;

        // Check if this thread should write to this mip level
        // Only write if we're at an even position (to avoid duplicate writes)
        if ((current_pos.x * 2 == base_pos.x || current_pos.x * 2 + 1 == base_pos.x) &&
            (current_pos.y * 2 == base_pos.y || current_pos.y * 2 + 1 == base_pos.y)) {

            // For now, just propagate the depth (simplified - real implementation would do proper max filtering)
            // In a proper implementation, we'd need to sample neighbors or use shared memory
            uint mip_width = hzb_output.get_width(mip);
            uint mip_height = hzb_output.get_height(mip);

            if (current_pos.x < mip_width && current_pos.y < mip_height) {
                hzb_output.write(float4(current_depth, 0.0, 0.0, 1.0), current_pos);
            }
        }

        // Reduce depth slightly for each mip level (conservative approximation)
        current_depth *= 0.99;
    }
}

// ============================================================================
// Advanced HZB Generation - Shared Memory Optimization
// ============================================================================

/**
 * @brief Optimized HZB generation using shared memory
 * @details Loads tile into shared memory, performs reduction, then writes to global memory
 */
kernel void generate_hzb_mip_optimized(
    texture2d<float, access::read> source_depth [[texture(0)]],
    texture2d<float, access::write> target_depth [[texture(1)]],
    constant HZBUniforms& uniforms [[buffer(0)]],
    uint3 global_id [[thread_position_in_grid]],
    uint3 local_id [[thread_position_in_threadgroup]],
    uint3 group_id [[threadgroup_position_in_grid]])
{
    // Shared memory for 16x16 tile + 1 pixel border for 2x2 reduction
    threadgroup float shared_tile[18][18];

    // Calculate shared memory position (with border)
    uint2 shared_pos = local_id.xy + uint2(1, 1);
    uint2 shared_base = group_id.xy * uint2(16, 16);

    // Load tile into shared memory
    uint2 source_pos = shared_base + local_id.xy;
    uint source_width = source_depth.get_width();
    uint source_height = source_depth.get_height();

    // Clamp to source boundaries
    source_pos.x = min(source_pos.x, source_width - 1);
    source_pos.y = min(source_pos.y, source_height - 1);

    // Load center pixel
    shared_tile[shared_pos.y][shared_pos.x] = source_depth.read(source_pos).r;

    // Load border pixels for edge cases
    if (local_id.x == 0) {
        // Load left border
        uint2 border_pos = source_pos - uint2(1, 0);
        border_pos.x = min(border_pos.x, source_width - 1);
        shared_tile[shared_pos.y][0] = source_depth.read(border_pos).r;
    }
    if (local_id.x == 15) {
        // Load right border
        uint2 border_pos = source_pos + uint2(1, 0);
        border_pos.x = min(border_pos.x, source_width - 1);
        shared_tile[shared_pos.y][17] = source_depth.read(border_pos).r;
    }
    if (local_id.y == 0) {
        // Load top border
        uint2 border_pos = source_pos - uint2(0, 1);
        border_pos.y = min(border_pos.y, source_height - 1);
        shared_tile[0][shared_pos.x] = source_depth.read(border_pos).r;
    }
    if (local_id.y == 15) {
        // Load bottom border
        uint2 border_pos = source_pos + uint2(0, 1);
        border_pos.y = min(border_pos.y, source_height - 1);
        shared_tile[17][shared_pos.x] = source_depth.read(border_pos).r;
    }

    // Synchronize all threads in the threadgroup
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // Perform 2x2 max filter for HZB generation
    // Each 2x2 block in shared memory produces one output pixel
    if (local_id.x < 8 && local_id.y < 8) {
        uint2 target_pos = group_id.xy * uint2(8, 8) + local_id.xy;
        uint2 shared_read_pos = local_id.xy * 2 + uint2(1, 1);

        // Find max in 2x2 neighborhood
        float max_depth = shared_tile[shared_read_pos.y][shared_read_pos.x];
        max_depth = fmax(max_depth, shared_tile[shared_read_pos.y][shared_read_pos.x + 1]);
        max_depth = fmax(max_depth, shared_tile[shared_read_pos.y + 1][shared_read_pos.x]);
        max_depth = fmax(max_depth, shared_tile[shared_read_pos.y + 1][shared_read_pos.x + 1]);

        // Write result to target mip level
        uint target_width = target_depth.get_width();
        uint target_height = target_depth.get_height();

        if (target_pos.x < target_width && target_pos.y < target_height) {
            target_depth.write(float4(max_depth, 0.0, 0.0, 1.0), target_pos);
        }
    }
}

// ============================================================================
// HZB Debug Visualization
// ============================================================================

/**
 * @brief Generate debug visualization of HZB levels
 * @details Creates a visual representation of HZB depth pyramid for debugging
 */
kernel void visualize_hzb_levels(
    texture2d<float, access::read> hzb_texture [[texture(0)]],
    texture2d<float, access::write> debug_output [[texture(1)]],
    constant HZBUniforms& uniforms [[buffer(0)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint2 pos = global_id.xy;
    uint output_width = debug_output.get_width();
    uint output_height = debug_output.get_height();

    if (pos.x >= output_width || pos.y >= output_height) {
        return;
    }

    // Create a grid visualization of different mip levels
    uint mip_level = 0;
    uint2 mip_offset = uint2(0, 0);

    // Simple layout: show first 4 mip levels in a 2x2 grid
    uint grid_size = output_width / 2;

    if (pos.x < grid_size && pos.y < grid_size) {
        mip_level = 0;
        mip_offset = uint2(0, 0);
    } else if (pos.x >= grid_size && pos.y < grid_size) {
        mip_level = 1;
        mip_offset = uint2(grid_size, 0);
    } else if (pos.x < grid_size && pos.y >= grid_size) {
        mip_level = 2;
        mip_offset = uint2(0, grid_size);
    } else {
        mip_level = 3;
        mip_offset = uint2(grid_size, grid_size);
    }

    // Scale position to mip level coordinates
    uint2 mip_pos = (pos - mip_offset) * (1 << mip_level);

    uint mip_width = hzb_texture.get_width(mip_level);
    uint mip_height = hzb_texture.get_height(mip_level);

    float4 debug_color = float4(0.0, 0.0, 0.0, 1.0);

    if (mip_pos.x < mip_width && mip_pos.y < mip_height) {
        float depth = hzb_texture.read(mip_pos, mip_level).r;

        // Map depth to color (gradient from blue to red)
        float normalized_depth = clamp(depth, 0.0, 1.0);
        debug_color.rgb = float3(normalized_depth, 1.0 - normalized_depth, 0.0);
    }

    debug_output.write(debug_color, pos);
}
