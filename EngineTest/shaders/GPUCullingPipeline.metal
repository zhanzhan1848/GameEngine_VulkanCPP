#include <metal_stdlib>
using namespace metal;

// ============================================================================
// Data Structures matching GPU memory layout
// ============================================================================

// Instance Data (144 bytes) - matches RenderSceneSnapshot::InstanceData
struct InstanceData {
    float4x4 world_matrix;
    float4x4 inverse_world_matrix;
    uint geometry_id;
    uint material_id;
    uint cluster_start;
    uint cluster_count;
};

// Cluster Reference (16 bytes) - matches RenderSceneSnapshot::ClusterRef
struct ClusterRef {
    uint geometry_id;
    uint cluster_index;
    uint padding[2];
};

// Bounding Sphere for culling
struct BoundingSphere {
    float3 center;
    float radius;
};

// Culling Uniforms - camera and configuration data
struct CullingUniforms {
    float4x4 view_matrix;
    float4x4 projection_matrix;
    float4x4 view_projection_matrix;
    float3 camera_position;
    float near_plane;
    float far_plane;
    uint frame_index;
    uint enable_occlusion_culling;
    uint enable_lod_selection;
    uint enable_small_object_culling;
    float small_object_threshold;
    float lod_bias;
    uint max_lod_levels;
    uint instance_count;
    uint cluster_count;
    uint padding[2];
};

// Intermediate visibility results (progressive filtering)
struct InstanceVisibility {
    uint is_visible;
    uint instance_index;
    uint lod_level;
    uint distance_rank;
};

struct ClusterVisibility {
    uint is_visible;
    uint cluster_index;
    uint instance_index;
    uint lod_level;
    uint frame_index;
    uint padding[3]; // Align to 32 bytes
};

// Frustum Planes for culling
struct FrustumPlanes {
    float4 planes[6]; // left, right, top, bottom, near, far
};

// ============================================================================
// Helper Functions
// ============================================================================

// ============================================================================
// Constants
// ============================================================================
constant uint MAX_CLUSTERS_PER_INSTANCE = 256;
constant uint MAX_INSTANCES = 100000;
constant uint MAX_CLUSTERS = MAX_INSTANCES * MAX_CLUSTERS_PER_INSTANCE;

// Extract frustum planes from view-projection matrix
FrustumPlanes extract_frustum_planes(float4x4 vp_matrix) {
    FrustumPlanes frustum;

    // Left plane
    frustum.planes[0] = normalize(float4(vp_matrix[3][0] + vp_matrix[0][0],
                                          vp_matrix[3][1] + vp_matrix[0][1],
                                          vp_matrix[3][2] + vp_matrix[0][2],
                                          vp_matrix[3][3] + vp_matrix[0][3]));

    // Right plane
    frustum.planes[1] = normalize(float4(vp_matrix[3][0] - vp_matrix[0][0],
                                          vp_matrix[3][1] - vp_matrix[0][1],
                                          vp_matrix[3][2] - vp_matrix[0][2],
                                          vp_matrix[3][3] - vp_matrix[0][3]));

    // Bottom plane
    frustum.planes[2] = normalize(float4(vp_matrix[3][0] + vp_matrix[1][0],
                                          vp_matrix[3][1] + vp_matrix[1][1],
                                          vp_matrix[3][2] + vp_matrix[1][2],
                                          vp_matrix[3][3] + vp_matrix[1][3]));

    // Top plane
    frustum.planes[3] = normalize(float4(vp_matrix[3][0] - vp_matrix[1][0],
                                          vp_matrix[3][1] - vp_matrix[1][1],
                                          vp_matrix[3][2] - vp_matrix[1][2],
                                          vp_matrix[3][3] - vp_matrix[1][3]));

    // Near plane
    frustum.planes[4] = normalize(float4(vp_matrix[3][0] + vp_matrix[2][0],
                                          vp_matrix[3][1] + vp_matrix[2][1],
                                          vp_matrix[3][2] + vp_matrix[2][2],
                                          vp_matrix[3][3] + vp_matrix[2][3]));

    // Far plane
    frustum.planes[5] = normalize(float4(vp_matrix[3][0] - vp_matrix[2][0],
                                          vp_matrix[3][1] - vp_matrix[2][1],
                                          vp_matrix[3][2] - vp_matrix[2][2],
                                          vp_matrix[3][3] - vp_matrix[2][3]));

    return frustum;
}

// Test sphere against frustum planes
bool test_sphere_frustum(BoundingSphere sphere, FrustumPlanes frustum) {
    for (int i = 0; i < 6; i++) {
        float4 plane = frustum.planes[i];
        float distance = dot(float4(sphere.center, 1.0), plane);
        if (distance < -sphere.radius) {
            return false;
        }
    }
    return true;
}

// Calculate screen space size for small object culling
float calculate_screen_size(float3 world_position, float bounding_radius,
                           float3 camera_position, float4x4 projection_matrix) {
    float distance = length(world_position - camera_position);
    if (distance < 0.001) return 1.0; // Avoid division by zero

    float screen_size = bounding_radius / distance;

    // Adjust by field of view (approximate)
    float fov_factor = 2.0 * abs(projection_matrix[1][1]); // 2.0 * tan(fov/2)
    screen_size *= fov_factor;

    return screen_size;
}

// Select LOD level based on distance and screen space error
uint select_lod_level(float distance, float screen_space_error, float lod_bias, uint max_lod_levels) {
    float threshold = lod_bias;

    for (uint lod = 0; lod < max_lod_levels - 1; lod++) {
        if (screen_space_error < threshold) {
            return lod;
        }
        threshold *= 2.0; // Each LOD level allows 2x more error
    }

    return max_lod_levels - 1;
}

// ============================================================================
// Stage 0: Reset Counter
// ============================================================================

kernel void stage0_reset_counter(
    device atomic_uint* visible_counter [[buffer(6)]],
    uint3 global_id [[thread_position_in_grid]])
{
    if (global_id.x == 0) {
        atomic_store_explicit(visible_counter, 0, memory_order_relaxed);
    }
}

// ============================================================================
// Stage 1: Instance-Level Frustum Culling
// ============================================================================

kernel void stage1_instance_frustum_culling(
    device const InstanceData* instances [[buffer(0)]],
    device const BoundingSphere* instance_bounds [[buffer(1)]],
    device InstanceVisibility* instance_visibility [[buffer(2)]],
    constant CullingUniforms& uniforms [[buffer(3)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint instance_id = global_id.x;

    if (instance_id >= uniforms.instance_count || instance_id >= 10000) { // Safety limit
        return;
    }

    // DISABLE CULLING: Make everything visible for stable debugging
    bool visible = true;

    // Write results
    instance_visibility[instance_id].is_visible = visible ? 1 : 0;
    instance_visibility[instance_id].instance_index = instance_id;
    instance_visibility[instance_id].lod_level = 0;
    instance_visibility[instance_id].distance_rank = 0;
}

// ============================================================================
// Stage 2: Distance & Small Object Culling
// ============================================================================

kernel void stage2_distance_small_object_culling(
    device const InstanceData* instances [[buffer(0)]],
    device const BoundingSphere* instance_bounds [[buffer(1)]],
    device InstanceVisibility* instance_visibility [[buffer(2)]],
    constant CullingUniforms& uniforms [[buffer(3)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint instance_id = global_id.x;

    if (instance_id >= uniforms.instance_count || instance_id >= 10000) {
        return;
    }

    // Skip if already culled
    if (instance_visibility[instance_id].is_visible == 0) {
        return;
    }

    // TEMPORARY: Skip all culling for debugging
    // Just calculate distance rank for LOD selection
    device const InstanceData& instance = instances[instance_id];
    device const BoundingSphere& bounds = instance_bounds[instance_id];

    // Transform bounds to world space
    float4 world_center_4 = instance.world_matrix * float4(bounds.center, 1.0);
    float3 world_center = world_center_4.xyz;

    // Calculate distance to camera
    float distance = length(world_center - uniforms.camera_position);

    // Calculate distance rank for LOD selection
    instance_visibility[instance_id].distance_rank = (uint)(distance * 10.0); // Quantize for sorting
}

// ============================================================================
// Stage 3: LOD Selection
// ============================================================================

kernel void stage3_lod_selection(
    device const InstanceData* instances [[buffer(0)]],
    device const BoundingSphere* instance_bounds [[buffer(1)]],
    device InstanceVisibility* instance_visibility [[buffer(2)]],
    constant CullingUniforms& uniforms [[buffer(3)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint instance_id = global_id.x;

    if (instance_id >= uniforms.instance_count || instance_id >= 10000) {
        return;
    }

    if (!uniforms.enable_lod_selection) {
        instance_visibility[instance_id].lod_level = 0;
        return;
    }

    // Skip if already culled
    if (instance_visibility[instance_id].is_visible == 0) {
        return;
    }

    device const InstanceData& instance = instances[instance_id];
    device const BoundingSphere& bounds = instance_bounds[instance_id];

    // Transform bounds to world space
    float4 world_center_4 = instance.world_matrix * float4(bounds.center, 1.0);
    float3 world_center = world_center_4.xyz;

    // Calculate distance and screen space error
    float distance = length(world_center - uniforms.camera_position);
    float screen_space_error = 100.0 / max(distance, 0.001); // Simplified SSE

    // Select LOD level
    uint lod_level = select_lod_level(distance, screen_space_error,
                                     uniforms.lod_bias, uniforms.max_lod_levels);

    instance_visibility[instance_id].lod_level = lod_level;
}

// ============================================================================
// Stage 4: Cluster-Level Expansion & Culling
// ============================================================================

kernel void stage4_cluster_expansion(
    device const InstanceVisibility* instance_visibility [[buffer(2)]],
    device const ClusterRef* cluster_refs [[buffer(4)]],
    device const InstanceData* instances [[buffer(0)]],
    device ClusterVisibility* cluster_visibility [[buffer(5)]],
    constant CullingUniforms& uniforms [[buffer(3)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint instance_id = global_id.x;

    if (instance_id >= uniforms.instance_count || instance_id >= MAX_INSTANCES) {
        return;
    }

    // Skip if instance is culled
    if (instance_visibility[instance_id].is_visible == 0) {
        return;
    }

    device const InstanceData& instance = instances[instance_id];
    uint lod_level = instance_visibility[instance_id].lod_level;

    // Expand instance to clusters
    uint cluster_start = instance.cluster_start;
    uint cluster_count = instance.cluster_count;

    for (uint i = 0; i < cluster_count; i++) {
        uint cluster_idx = cluster_start + i;
        uint visibility_idx = instance_id * MAX_CLUSTERS_PER_INSTANCE + i; // Max 256 clusters per instance

        if (visibility_idx >= MAX_CLUSTERS) break; // Safety limit

        // Mark as visible for now - occlusion culling can disable later if needed
        // Use stable cluster index for consistent visibility across frames
        cluster_visibility[visibility_idx].is_visible = 1;
        cluster_visibility[visibility_idx].cluster_index = cluster_idx;
        cluster_visibility[visibility_idx].instance_index = instance_id;
        cluster_visibility[visibility_idx].lod_level = lod_level;
        cluster_visibility[visibility_idx].frame_index = uniforms.frame_index;
    }
}

// ============================================================================
// Stage 5: Occlusion Culling (HZB) - Placeholder for now
// ============================================================================

kernel void stage5_occlusion_culling(
    device const ClusterVisibility* cluster_visibility [[buffer(5)]],
    texture2d<float> hzb_texture [[texture(0)]],
    constant CullingUniforms& uniforms [[buffer(3)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint cluster_idx = global_id.x;

    if (cluster_idx >= MAX_CLUSTERS) { // Safety limit
        return;
    }

    // Skip if already culled or occlusion culling disabled
    if (cluster_visibility[cluster_idx].is_visible == 0 || !uniforms.enable_occlusion_culling) {
        return;
    }

    // TODO: Implement HZB occlusion culling
    // This requires proper HZB texture generation and sampling
    // For now, this is a placeholder that doesn't modify the visibility data
}

// ============================================================================
// Stage 6: Visible List Compaction
// ============================================================================

kernel void stage6_compact_visible_list(
    device const InstanceData* instances [[buffer(0)]],
    device const InstanceVisibility* instance_visibility [[buffer(2)]],
    device const ClusterVisibility* cluster_visibility [[buffer(5)]],
    device atomic_uint* visible_counter [[buffer(6)]],
    device uint* visible_cluster_list [[buffer(7)]],
    constant CullingUniforms& uniforms [[buffer(3)]],
    uint3 global_id [[thread_position_in_grid]])
{
    // Process the full visibility array range (instance_count * 256)
    // This handles the sparse distribution of clusters across instances
    uint instance_id = global_id.x / MAX_CLUSTERS_PER_INSTANCE;  // Which instance
    uint cluster_offset = global_id.x % MAX_CLUSTERS_PER_INSTANCE;  // Which cluster within instance
    uint visibility_idx = global_id.x;

    if (visibility_idx >= MAX_CLUSTERS) { // Safety limit
        return;
    }

    // Check instance bounds
    if (instance_id >= uniforms.instance_count) {
        return;
    }

    // 1. Check if instance is visible
    // This is CRITICAL because if the instance is culled, Stage 4 doesn't run,
    // so cluster_visibility buffer contains STALE data from previous frames.
    if (instance_visibility[instance_id].is_visible == 0) {
        return;
    }

    // 2. Check if this is a valid cluster slot
    // This is CRITICAL because Stage 4 only writes to slots < cluster_count.
    // Slots >= cluster_count contain UNINITIALIZED GARBAGE.
    device const InstanceData& instance = instances[instance_id];
    if (cluster_offset >= instance.cluster_count) {
        return;
    }

    // 3. Check cluster visibility (e.g. from Occlusion Culling)
    // AND check frame index to prevent reading stale data from previous frames (Crucial for flickering fix)
    if (cluster_visibility[visibility_idx].is_visible == 0 ||
        cluster_visibility[visibility_idx].frame_index != uniforms.frame_index) {
        return;
    }

    // Atomic increment to get position in visible list
    uint list_idx = atomic_fetch_add_explicit(visible_counter, 1, memory_order_relaxed);

    if (list_idx < MAX_CLUSTERS) { // Safety limit
        // Store the actual cluster index for rendering
        // Use stable cluster_index instead of visibility_idx to prevent flickering
        uint cluster_index = cluster_visibility[visibility_idx].cluster_index;
        visible_cluster_list[list_idx] = cluster_index;
    }
}

// ============================================================================
// Stage 7: Build Indirect Draw Commands
// ============================================================================

struct IndirectDrawCommand {
    uint vertex_count;
    uint instance_count;
    uint first_vertex;
    uint first_instance;
};

kernel void stage7_build_indirect_commands(
    device const atomic_uint* visible_counter [[buffer(6)]],
    device const uint* visible_cluster_list [[buffer(7)]],
    device IndirectDrawCommand* indirect_commands [[buffer(8)]],
    constant CullingUniforms& uniforms [[buffer(3)]],
    uint3 global_id [[thread_position_in_grid]])
{
    // Only first thread builds the command
    if (global_id.x != 0) {
        return;
    }

    uint visible_count = atomic_load_explicit(visible_counter, memory_order_relaxed);

    // Safety clamp to prevent out of bounds indirect draw
    // This is CRITICAL for preventing GPU hangs
    if (visible_count > MAX_CLUSTERS) {
        visible_count = MAX_CLUSTERS;
    }
    
    // Also clamp to 0 if something went wrong
    if (visible_count < 0) visible_count = 0;

    // Build single indirect command for all visible clusters
    IndirectDrawCommand cmd;
    cmd.vertex_count = 384 * 3; // Max triangles per meshlet * 3 vertices
    cmd.instance_count = visible_count;
    cmd.first_vertex = 0;
    cmd.first_instance = 0;

    indirect_commands[0] = cmd;
}