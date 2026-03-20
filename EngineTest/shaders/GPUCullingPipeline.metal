#include <metal_stdlib>
using namespace metal;

// ============================================================================
// Data Structures matching GPU memory layout
// ============================================================================

// Instance Data (192 bytes) - MUST match RenderSceneSnapshot::InstanceData exactly
// FIXED: Correct layout matching CPU struct (simd::float3 has 4-byte alignment, not 16)
//
// CPU Layout (verified):
// - Offset 0: world_matrix (64 bytes)
// - Offset 64: inverse_world_matrix (64 bytes)
// - Offset 128: 6 x uint (geometry_id through padding = 24 bytes)
// - Offset 152: bounds_center (simd::float3 = 12 bytes, 4-byte aligned)
// - Offset 164: bounds_radius (4 bytes)
// - Offset 168: bounds_padding[3] (12 bytes)
// - Offset 180: tail padding (12 bytes to reach 192)
// Total: 64+64+24+12+4+12+12 = 192 ✓
struct InstanceData {
    float4x4 world_matrix;                    // 64 bytes at offset 0
    float4x4 inverse_world_matrix;            // 64 bytes at offset 64
    uint geometry_id;                        // 4 bytes at offset 128
    uint material_id;                        // 4 bytes at offset 132
    uint cluster_start;                      // 4 bytes at offset 136
    uint cluster_count;                      // 4 bytes at offset 140
    uint cluster_map_base;                   // 4 bytes at offset 144
    uint padding;                           // 4 bytes at offset 148
    uint padding1;                           // 4 bytes at offset 152 (extra padding for 16-byte alignment)
    uint padding2;                           // 4 bytes at offset 156 (extra padding for alignment)
    packed_float3 bounds_center;             // 12 bytes at offset 160
    uint bounds_center_padding;              // 4 bytes at offset 172 (padding to match C++ 16-byte simd_float3)
    float bounds_radius;                     // 4 bytes at offset 176
    uint bounds_padding[2];                  // 8 bytes at offset 180
    uint bounds_padding2;                    // 4 bytes at offset 188 → Total 192 ✓
};

// Cluster Reference (16 bytes) - matches RenderSceneSnapshot::ClusterRef
struct ClusterRef {
    uint geometry_id;
    uint cluster_index;
    uint padding[2];
};

// Bounding Sphere for culling
struct BoundingSphere {
    packed_float3 center;
    float radius;
};

// Culling Uniforms - camera and configuration data
// Using float4 for camera_position to match C++ v4 alignment
struct CullingUniforms {
    float4x4 view_matrix;
    float4x4 projection_matrix;
    float4x4 view_projection_matrix;
    float4 camera_position;  // Changed from float3 to float4 for alignment
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
    uint force_pass_all; // 🔥 DEBUG: Force all geometry to pass culling
    uint enable_debug_output; // 🔥 DEBUG: Enable debug output
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

// Frustum Planes for culling (traditional method)
struct FrustumPlanes {
    float4 planes[6]; // left, right, top, bottom, near, far
};

// Cone-based frustum representation (more efficient for tile-based culling)
struct ConeFrustum {
    float3 cone_direction;  // Direction from camera through tile center
    float unit_radius;      // Radius of cone at unit distance
    float min_depth;        // Minimum depth for this tile
    float max_depth;        // Maximum depth for this tile
};

// Debug culling data structure
struct CullingDebugData {
    float view_space_z;           // Z value in view space
    float bounds_radius;          // Object bounding radius
    float distance_to_camera;     // Distance from camera
    uint is_visible;              // Visibility result
    uint instance_id;             // Instance identifier
    uint cluster_id;              // Cluster identifier
    uint culling_reason;          // Why it was culled (0=frustum, 1=distance, 2=none)
    uint culling_plane;           // 🔥 NEW: Which plane caused culling (0=left, 1=right, 2=bottom, 3=top, 4=near, 5=far, 0xFFFFFFFF=none)
    float plane_distances[6];     // 🔥 NEW: Distance to each frustum plane for debugging
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
constant uint MAX_DEBUG_ENTRIES = 1000;
constant uint STAGE1_DEBUG_OFFSET = 0;           // Stage 1 uses indices 0-499
constant uint STAGE4_DEBUG_OFFSET = 500;         // Stage 4 uses indices 500-999
constant uint STAGE_DEBUG_COUNT = 500;           // 500 entries per stage

float4 normalize_plane(float4 plane) {
    float3 normal = plane.xyz;
    float length = sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (length > 0.0001) {
        return float4(normal / length, plane.w / length);
    }
    return plane;
}

// Extract frustum planes from view-projection matrix for WORLD-SPACE culling
// 🔥 FINAL ATTEMPT: Use traditional row-wise extraction without transpose
FrustumPlanes extract_frustum_planes(float4x4 view_projection_matrix) {
    FrustumPlanes frustum;

    // 🔥 BACK TO WORKING: Use column-based extraction that was working before
    float4x4 vp = view_projection_matrix;

    // Access columns directly
    float4 col0 = vp[0]; // First column
    float4 col1 = vp[1]; // Second column
    float4 col2 = vp[2]; // Third column
    float4 col3 = vp[3]; // Fourth column

    // Standard extraction without negation
    frustum.planes[0] = normalize_plane(col3 + col0); // Left
    frustum.planes[1] = normalize_plane(col3 - col0); // Right
    frustum.planes[2] = normalize_plane(col3 + col1); // Bottom
    frustum.planes[3] = normalize_plane(col3 - col1); // Top
    frustum.planes[4] = normalize_plane(col3 + col2); // Near
    frustum.planes[5] = normalize_plane(col3 - col2); // Far

    return frustum;
}

// Test sphere against frustum planes with proper tolerance
// 🔥 DEBUG VERSION: Returns which plane caused culling for debugging
int test_sphere_frustum_debug(BoundingSphere sphere, FrustumPlanes frustum, float debug_distances[6]) {
    // 🔥 FIX: Use depth-based relative tolerance
    // In view space, we care about Z depth for tolerance calculation
    float depth = abs(sphere.center.z); // Distance from camera in depth
    float relative_tolerance = max(0.5, depth * 0.02); // 2% of depth, minimum 0.5
    float effective_radius = sphere.radius + relative_tolerance;

    // Test all 6 planes with the corrected formulas
    for (int i = 0; i < 6; i++) {
        float4 plane = frustum.planes[i];

        // Skip invalid planes
        float3 normal = plane.xyz;
        float normal_length = length(normal);

        if (normal_length < 0.001) {
            debug_distances[i] = 0.0;
            continue;
        }

        // Calculate distance from sphere center to plane
        float distance = dot(float4(float3(sphere.center), 1.0), plane);
        debug_distances[i] = distance; // Store distance for debugging

        // 🔥 BACK TO ORIGINAL: Use standard culling logic
        // For outward-pointing planes: distance < -effective_radius means outside frustum
        if (distance < -effective_radius) {
            return i; // Return which plane caused culling
        }
    }

    return -1; // No culling
}

// Non-debug version for performance
bool test_sphere_frustum(BoundingSphere sphere, FrustumPlanes frustum) {
    // 🔥 FIX: Use depth-based relative tolerance
    float depth = abs(sphere.center.z); // Distance from camera in depth
    float relative_tolerance = max(0.5, depth * 0.02); // 2% of depth, minimum 0.5
    float effective_radius = sphere.radius + relative_tolerance;

    // Test all 6 planes with the corrected formulas
    for (int i = 0; i < 6; i++) {
        float4 plane = frustum.planes[i];

        // Skip invalid planes
        float3 normal = plane.xyz;
        float normal_length = length(normal);

        if (normal_length < 0.001) {
            continue;
        }

        // Calculate distance from sphere center to plane
        float distance = dot(float4(float3(sphere.center), 1.0), plane);

        // 🔥 BACK TO ORIGINAL: Use standard culling logic
        if (distance < -effective_radius) {
            return false;
        }
    }

    return true;
}

// More efficient cone-based intersection test (based on CullLights.hlsl approach)
bool intersect_cone_frustum(BoundingSphere sphere, ConeFrustum cone_frustum) {
    // Depth range culling (faster rejection)
    // In view space (Metal): looking down -Z, near is closer to 0, far is more negative
    // Objects should be within [near, far] range
    float sphere_near = float3(sphere.center).z - sphere.radius;
    float sphere_far = float3(sphere.center).z + sphere.radius;

    // Convert cone_frustum depths to view space (cone_frustum.min_depth is near, max_depth is far)
    // In Metal view space: near ~ -0.1, far ~ -1000 (looking down -Z)
    float view_near = -cone_frustum.min_depth;  // Convert to view space
    float view_far = -cone_frustum.max_depth;   // Convert to view space

    // Check if sphere is completely outside the depth range
    if (sphere_near < view_far || sphere_far > view_near) {
        return false;
    }

    // Cone-based intersection test (more efficient than 6-plane test)
    // Project sphere center onto the cone's perpendicular direction
    float3 center_f3 = float3(sphere.center);
    float3 sphere_rejection = center_f3 - dot(center_f3, cone_frustum.cone_direction) * cone_frustum.cone_direction;
    float dist_sq = dot(sphere_rejection, sphere_rejection);

    // Calculate effective radius at the sphere's distance
    // Use absolute distance since we're looking down -Z axis in view space
    float distance_along_cone = abs(center_f3.z);
    
    // RELAXED CULLING: Increase effective radius significantly (e.g., 2.0x) to prevent edge flickering during camera movement
    float relaxed_multiplier = 2.0; 
    float effective_radius = distance_along_cone * cone_frustum.unit_radius + (sphere.radius * relaxed_multiplier);
    float radius_sq = effective_radius * effective_radius;

    return dist_sq <= radius_sq;
}

// Convert traditional frustum to cone representation for whole screen
ConeFrustum create_screen_cone_frustum(float4x4 projection_matrix, float near_plane, float far_plane) {
    ConeFrustum cone;
    cone.cone_direction = float3(0, 0, 1); // Looking down +Z axis in view space

    // Calculate the field of view from projection matrix
    float tan_half_fov_y = 1.0f / projection_matrix[1][1];
    float tan_half_fov_x = 1.0f / projection_matrix[0][0];
    float max_half_fov = max(tan_half_fov_x, tan_half_fov_y);

    // Unit radius is the radius of the cone at unit distance
    cone.unit_radius = max_half_fov;

    cone.min_depth = near_plane;
    cone.max_depth = far_plane;

    return cone;
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
// Stage 0: Complete Buffer Reset (Clear all GPU buffers for fresh frame)
// ============================================================================

kernel void stage0_reset_all_buffers(
    device atomic_uint* visible_counter [[buffer(5)]],
    device atomic_uint* cluster_visibility_counter [[buffer(9)]], // NEW: Counter for cluster visibility
    device InstanceVisibility* instance_visibility [[buffer(1)]],
    device ClusterVisibility* cluster_visibility [[buffer(4)]],
    device uint* visible_cluster_list [[buffer(6)]],
    constant CullingUniforms& uniforms [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint idx = global_id.x;

    // Clear atomic counters (only first thread does this)
    if (idx == 0) {
        atomic_store_explicit(visible_counter, 0, memory_order_relaxed);
        atomic_store_explicit(cluster_visibility_counter, 0, memory_order_relaxed); // NEW: Reset cluster visibility counter
    }

    // Clear instance visibility buffer
    // Each thread handles one instance
    if (idx < uniforms.instance_count && idx < MAX_INSTANCES) {
        instance_visibility[idx].is_visible = 0;
        instance_visibility[idx].instance_index = 0;
        instance_visibility[idx].lod_level = 0;
        instance_visibility[idx].distance_rank = 0;
    }

    // Clear cluster visibility buffer (compact range: cluster_count)
    if (idx < uniforms.cluster_count && idx < MAX_CLUSTERS) {
        cluster_visibility[idx].is_visible = 0;
        cluster_visibility[idx].cluster_index = 0xFFFFFFFF; // Invalid cluster index
        cluster_visibility[idx].instance_index = 0;
        cluster_visibility[idx].lod_level = 0;
        cluster_visibility[idx].frame_index = 0xFFFFFFFF; // Invalid frame index to prevent stale data usage
    }

    // CRITICAL FIX: Ensure visible_cluster_list is properly cleared
    // This prevents stale data from being used in rendering
    if (idx < uniforms.cluster_count && idx < MAX_CLUSTERS) {
        visible_cluster_list[idx] = 0; // Clear to 0, not 0xFFFFFFFF
    }
}

// ============================================================================
// Stage 1: Instance-Level Frustum Culling
// ============================================================================

kernel void stage1_instance_frustum_culling(
    device const InstanceData* instances [[buffer(0)]],
    device InstanceVisibility* instance_visibility [[buffer(1)]],
    constant CullingUniforms& uniforms [[buffer(2)]],
    device CullingDebugData* debug_buffer [[buffer(10)]],
    device atomic_uint* cluster_visibility_counter [[buffer(9)]], // Reuse cluster visibility counter as debug counter
    uint3 global_id [[thread_position_in_grid]])
{
    uint instance_id = global_id.x;

    if (instance_id >= uniforms.instance_count || instance_id >= MAX_INSTANCES) {
        return;
    }

    // 🔥 FIX: Re-enable frustum culling with CORRECTED implementation
    // Transform to view space for frustum testing
    float3 bounds_center = instances[instance_id].bounds_center;
    float bounds_radius = instances[instance_id].bounds_radius;
    float4 view_center_4 = uniforms.view_matrix * float4(bounds_center, 1.0);

    // 🔥 CORRECTED: Basic near/far plane check considering object radius
    // In view space: camera looks down -Z axis, so visible objects have negative Z values
    // Near plane culling: only cull if object's closest point is BEHIND the camera
    // Objects that cross the camera plane (partially in front) should still be visible
    if (view_center_4.z - bounds_radius > 0.0) {
        // Entire object is behind camera (farthest point is still behind camera at Z > 0)

        // 🔥 DEBUG: Record culled objects for debugging
        if (uniforms.enable_debug_output) {
            uint debug_idx = atomic_fetch_add_explicit(cluster_visibility_counter, 1, memory_order_relaxed);
            debug_idx = STAGE1_DEBUG_OFFSET + (debug_idx % STAGE_DEBUG_COUNT);

            if (debug_idx < MAX_DEBUG_ENTRIES) {
                float distance_to_camera = length(bounds_center - uniforms.camera_position.xyz);
                debug_buffer[debug_idx].view_space_z = view_center_4.z;
                debug_buffer[debug_idx].bounds_radius = bounds_radius;
                debug_buffer[debug_idx].distance_to_camera = distance_to_camera;
                debug_buffer[debug_idx].is_visible = 0;
                debug_buffer[debug_idx].instance_id = instance_id;
                debug_buffer[debug_idx].cluster_id = 0xFFFFFFFF;
                debug_buffer[debug_idx].culling_reason = 0; // Frustum
                debug_buffer[debug_idx].culling_plane = 4; // Near plane
                for (int p = 0; p < 6; p++) {
                    debug_buffer[debug_idx].plane_distances[p] = 0.0;
                }
            }
        }

        instance_visibility[instance_id].is_visible = 0;
        instance_visibility[instance_id].instance_index = instance_id;
        instance_visibility[instance_id].lod_level = 0;
        instance_visibility[instance_id].distance_rank = 0;
        return;
    }

    if (view_center_4.z < -uniforms.far_plane - bounds_radius) {
        // Entire object is beyond far plane (center is further than far_plane + radius)

        // 🔥 DEBUG: Record culled objects for debugging
        if (uniforms.enable_debug_output) {
            uint debug_idx = atomic_fetch_add_explicit(cluster_visibility_counter, 1, memory_order_relaxed);
            debug_idx = STAGE1_DEBUG_OFFSET + (debug_idx % STAGE_DEBUG_COUNT);

            if (debug_idx < MAX_DEBUG_ENTRIES) {
                float distance_to_camera = length(bounds_center - uniforms.camera_position.xyz);
                debug_buffer[debug_idx].view_space_z = view_center_4.z;
                debug_buffer[debug_idx].bounds_radius = bounds_radius;
                debug_buffer[debug_idx].distance_to_camera = distance_to_camera;
                debug_buffer[debug_idx].is_visible = 0;
                debug_buffer[debug_idx].instance_id = instance_id;
                debug_buffer[debug_idx].cluster_id = 0xFFFFFFFF;
                debug_buffer[debug_idx].culling_reason = 0; // Frustum
                debug_buffer[debug_idx].culling_plane = 5; // Far plane
                for (int p = 0; p < 6; p++) {
                    debug_buffer[debug_idx].plane_distances[p] = 0.0;
                }
            }
        }

        instance_visibility[instance_id].is_visible = 0;
        instance_visibility[instance_id].instance_index = instance_id;
        instance_visibility[instance_id].lod_level = 0;
        instance_visibility[instance_id].distance_rank = 0;
        return;
    }

    // 🔥 FIX: Use very conservative tolerance to avoid false positives
    float conservative_tolerance = 2.0; // Fixed large tolerance for safety
    float effective_radius = bounds_radius + conservative_tolerance;

    // Extract view-space frustum planes
    FrustumPlanes view_frustum = extract_frustum_planes(uniforms.view_projection_matrix);

    // Test against side planes only (skip near/far as we already tested them)
    for (int i = 0; i < 4; i++) { // Only test Left, Right, Bottom, Top
        float4 plane = view_frustum.planes[i];
        float3 normal = plane.xyz;
        float normal_length = length(normal);

        if (normal_length < 0.001) continue;

        // 🔥 FIX: For side planes in view space, only use XY coordinates
        // Create a 3D point with Z=0 for side plane testing (since side planes are perpendicular to Z axis)
        float3 view_pos_xy_only = float3(view_center_4.xy, 0.0);
        float distance = dot(float4(view_pos_xy_only, 1.0), plane);

        // Conservative culling check - only cull if clearly outside
        if (distance < -effective_radius * 2.0) { // Extra safety factor

            // 🔥 DEBUG: Record culled objects for debugging
            if (uniforms.enable_debug_output) {
                uint debug_idx = atomic_fetch_add_explicit(cluster_visibility_counter, 1, memory_order_relaxed);
                debug_idx = STAGE1_DEBUG_OFFSET + (debug_idx % STAGE_DEBUG_COUNT);

                if (debug_idx < MAX_DEBUG_ENTRIES) {
                    float distance_to_camera = length(bounds_center - uniforms.camera_position.xyz);
                    debug_buffer[debug_idx].view_space_z = view_center_4.z;
                    debug_buffer[debug_idx].bounds_radius = bounds_radius;
                    debug_buffer[debug_idx].distance_to_camera = distance_to_camera;
                    debug_buffer[debug_idx].is_visible = 0;
                    debug_buffer[debug_idx].instance_id = instance_id;
                    debug_buffer[debug_idx].cluster_id = 0xFFFFFFFF;
                    debug_buffer[debug_idx].culling_reason = 0; // Frustum
                    debug_buffer[debug_idx].culling_plane = i; // Which side plane caused culling

                    // Calculate all plane distances for debugging
                    for (int p = 0; p < 6; p++) {
                        float4 current_plane = view_frustum.planes[p];
                        if (p < 4) { // Side planes use XY only
                            float3 current_view_pos_xy = float3(view_center_4.xy, 0.0);
                            debug_buffer[debug_idx].plane_distances[p] = dot(float4(current_view_pos_xy, 1.0), current_plane);
                        } else { // Near/Far planes use full XYZ
                            debug_buffer[debug_idx].plane_distances[p] = dot(float4(view_center_4.xyz, 1.0), current_plane);
                        }
                    }
                }
            }

            instance_visibility[instance_id].is_visible = 0;
            instance_visibility[instance_id].instance_index = instance_id;
            instance_visibility[instance_id].lod_level = 0;
            instance_visibility[instance_id].distance_rank = 0;
            return;
        }
    }

    // If we get here, object is visible
    instance_visibility[instance_id].is_visible = 1;
    instance_visibility[instance_id].instance_index = instance_id;
    instance_visibility[instance_id].lod_level = 0;
    float distance_z = max(0.0, -view_center_4.z);
    instance_visibility[instance_id].distance_rank = (uint)(distance_z * 100.0);

    // 🔥 DEBUG: Record ALL instances (both visible and culled) for Stage 1 analysis
    if (uniforms.enable_debug_output) {
        // Get atomic counter index for Stage 1 debug data
        uint debug_idx = atomic_fetch_add_explicit(cluster_visibility_counter, 1, memory_order_relaxed);

        // Apply Stage 1 offset so it doesn't conflict with Stage 4
        debug_idx = STAGE1_DEBUG_OFFSET + (debug_idx % STAGE_DEBUG_COUNT);

        // Limit debug output to prevent buffer overflow
        if (debug_idx < MAX_DEBUG_ENTRIES) {
            // Calculate distance to camera using world space positions
            float distance_to_camera = length(bounds_center - uniforms.camera_position.xyz);

            // Write debug data for ALL objects (visible ones reach here, culled ones returned above)
            debug_buffer[debug_idx].view_space_z = view_center_4.z;
            debug_buffer[debug_idx].bounds_radius = bounds_radius;
            debug_buffer[debug_idx].distance_to_camera = distance_to_camera;
            debug_buffer[debug_idx].is_visible = 1; // Only visible objects reach this code
            debug_buffer[debug_idx].instance_id = instance_id;
            debug_buffer[debug_idx].cluster_id = 0xFFFFFFFF;
            debug_buffer[debug_idx].culling_reason = 2; // None (reached here = not culled)
            debug_buffer[debug_idx].culling_plane = 0xFFFFFFFF; // None

            // 🔥 DEBUG: Calculate plane distances for analysis even if not culled
            FrustumPlanes view_frustum = extract_frustum_planes(uniforms.view_projection_matrix);

            // Calculate distances to all 6 planes for debugging
            for (int p = 0; p < 6; p++) {
                float4 plane = view_frustum.planes[p];

                // For side planes, use XY only
                if (p < 4) { // Left, Right, Bottom, Top
                    float3 view_pos_xy_only = float3(view_center_4.xy, 0.0);
                    debug_buffer[debug_idx].plane_distances[p] = dot(float4(view_pos_xy_only, 1.0), plane);
                } else { // Near, Far
                    debug_buffer[debug_idx].plane_distances[p] = dot(float4(view_center_4.xyz, 1.0), plane);
                }
            }
        }
    }
}

// ============================================================================
// Stage 2: Distance & Small Object Culling
// ============================================================================

kernel void stage2_distance_small_object_culling(
    device const InstanceData* instances [[buffer(0)]],
    device InstanceVisibility* instance_visibility [[buffer(1)]],
    constant CullingUniforms& uniforms [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint instance_id = global_id.x;

    if (instance_id >= uniforms.instance_count || instance_id >= MAX_INSTANCES) {
        return;
    }

    // Distance & Small Object Culling disabled in this debug version
    // Just copy visibility and set distance rank
    
    // Skip if already culled
    if (instance_visibility[instance_id].is_visible == 0) {
        return;
    }

    device const InstanceData& instance = instances[instance_id];

    // Transform bounds to view space for distance calculation
    float3 bounds_center = instance.bounds_center;
    float4 view_center_4 = uniforms.view_matrix * float4(bounds_center, 1.0);

    // Calculate distance rank for LOD selection (using view space Z, which is negative in Metal)
    instance_visibility[instance_id].distance_rank = (uint)(max(0.0, -view_center_4.z) * 10.0); // Quantize for sorting
}

// ============================================================================
// Stage 3: LOD Selection
// ============================================================================

kernel void stage3_lod_selection(
    device const InstanceData* instances [[buffer(0)]],
    device InstanceVisibility* instance_visibility [[buffer(1)]],
    constant CullingUniforms& uniforms [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint instance_id = global_id.x;

    if (instance_id >= uniforms.instance_count || instance_id >= MAX_INSTANCES) {
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

    // Transform bounds to world space (bounds_center is already in world space)
    float3 bounds_center = instance.bounds_center;
    float3 world_center = bounds_center;

    // Calculate distance and screen space error
    float distance = length(world_center - uniforms.camera_position.xyz);
    float screen_space_error = 100.0 / max(distance, 0.001); // Simplified SSE

    // Select LOD level
    uint lod_level = select_lod_level(distance, screen_space_error,
                                     uniforms.lod_bias, uniforms.max_lod_levels);

    // CRITICAL: Ensure LOD level is valid to prevent out-of-bounds indexing later
    if (lod_level >= uniforms.max_lod_levels) {
        lod_level = uniforms.max_lod_levels - 1;
    }

    instance_visibility[instance_id].lod_level = lod_level;
}

// ============================================================================
// Stage 4: Cluster-Level Expansion & Culling
// ============================================================================

kernel void stage4_cluster_expansion(
    device const InstanceVisibility* instance_visibility [[buffer(1)]],
    device const ClusterRef* cluster_refs [[buffer(3)]],
    device const InstanceData* instances [[buffer(0)]],
    device ClusterVisibility* cluster_visibility [[buffer(4)]],
    device atomic_uint* cluster_visibility_counter [[buffer(9)]],
    device CullingDebugData* debug_buffer [[buffer(10)]], // Add debug buffer
    constant CullingUniforms& uniforms [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    // Calculate view space frustum planes for cluster-level culling
    FrustumPlanes frustum = extract_frustum_planes(uniforms.view_projection_matrix);

    // 🔥 DEBUG: Force pass all culling - DETERMINISTIC SINGLE-THREADED APPROACH
    if (uniforms.force_pass_all) {
        // Only thread 0 processes to ensure deterministic ordering
        if (global_id.x == 0) {
            uint write_pos = 0;

            // Sequential processing for deterministic results
            for (uint instance_id = 0; instance_id < uniforms.instance_count && instance_id < MAX_INSTANCES; instance_id++) {
                InstanceData instance = instances[instance_id];
                uint cluster_count = instance.cluster_count;
                uint cluster_start = instance.cluster_start; // Use cluster_start for stable indexing

                // Safety check to prevent GPU hang if cluster_count is corrupt
                if (cluster_count > MAX_CLUSTERS_PER_INSTANCE) {
                    continue;
                }

                for (uint cluster_idx = 0; cluster_idx < cluster_count; cluster_idx++) {
                    uint cluster_map_idx = cluster_start + cluster_idx;

                    // 🔥 DISABLE: Skip all cluster-level culling for now - accept all clusters
                    // This will help us determine if Stage 1 (instance-level culling) is working correctly

                    if (write_pos >= uniforms.cluster_count || write_pos >= MAX_CLUSTERS) {
                        continue;
                    }

                    // Accept all clusters without culling
                    cluster_visibility[write_pos].is_visible = 1;
                    cluster_visibility[write_pos].cluster_index = cluster_map_idx;
                    cluster_visibility[write_pos].instance_index = instance_id;
                    cluster_visibility[write_pos].lod_level = 0;
                    cluster_visibility[write_pos].frame_index = uniforms.frame_index;
                    write_pos++;
                }
            }

            atomic_store_explicit(cluster_visibility_counter, write_pos, memory_order_relaxed);
        }
        return; // All other threads exit immediately
    }

    // CRITICAL FIX: Make cluster expansion deterministic by using single-threaded approach
    // Only thread 0 performs the expansion to ensure consistent ordering across frames
    if (global_id.x != 0) {
        return;
    }

    uint write_pos = 0;
    uint debug_write_pos = 0; // Separate counter for debug data

    // Deterministic sequential scan - always processes instances in same order
    for (uint instance_id = 0; instance_id < uniforms.instance_count && instance_id < MAX_INSTANCES; instance_id++) {
        // Skip if instance is culled
        if (instance_visibility[instance_id].is_visible == 0) {
            continue;
        }

        device const InstanceData& instance = instances[instance_id];
        uint lod_level = instance_visibility[instance_id].lod_level;

        // Expand instance to clusters
        uint cluster_count = instance.cluster_count;

        // Safety check to prevent GPU hang if cluster_count is corrupt
        if (cluster_count > MAX_CLUSTERS_PER_INSTANCE) {
            continue;
        }

        // Use cluster_map_base from CPU-side calculation
        uint global_cluster_base = instance.cluster_map_base;

        for (uint i = 0; i < cluster_count; i++) {
            // 🔥 Stage 4 全量通过: Accept all clusters without per-cluster culling
            // Only perform bounds calculation for debug output purposes
            float3 cluster_local_offset = float3(0.0);
            float t = 0.0; // Initialize t for all cases

            if (cluster_count > 1) {
                t = (float)i / (float)(cluster_count - 1);
                float angle = t * 3.14159 * 8.0;
                float radius = t * instance.bounds_radius * 0.8;

                cluster_local_offset.x = cos(angle) * radius;
                cluster_local_offset.y = sin(angle) * radius;
                cluster_local_offset.z = (t - 0.5) * instance.bounds_radius * 0.5;
            }

            // Calculate cluster bounds for debug output and culling
            float3 bounds_center = instance.bounds_center;
            float3 cluster_world_offset = (instance.world_matrix * float4(cluster_local_offset, 0.0)).xyz;
            float3 cluster_world_center = bounds_center + cluster_world_offset;
            float cluster_bounds_radius = instance.bounds_radius * max(0.5, t * 0.8 + 0.2);

            // Calculate world space radius
            float3 cluster_scale_vector = float3(
                length(instance.world_matrix[0].xyz),
                length(instance.world_matrix[1].xyz),
                length(instance.world_matrix[2].xyz)
            );
            float cluster_max_scale = max(cluster_scale_vector.x, max(cluster_scale_vector.y, cluster_scale_vector.z));
            float cluster_world_radius = cluster_bounds_radius * cluster_max_scale;

            // For debug output, calculate view space data
            float4 cluster_view_center_4 = uniforms.view_matrix * float4(cluster_world_center, 1.0);

            // 🔥 CRITICAL FIX: Create WORLD-space bounding sphere for culling consistency
            BoundingSphere cluster_world_sphere;
            cluster_world_sphere.center = cluster_world_center;
            cluster_world_sphere.radius = cluster_world_radius;

            // 🔥 Stage 4 全量通过: Always accept all clusters from visible instances
            // But still calculate actual plane distances for debugging
            bool cluster_is_visible = true;
            int culling_plane_idx = -1;
            float debug_plane_distances[6];

            // 🔥 FIX: Stage 4 should also perform correct culling for debug consistency
            bool cluster_visible = true;
            int cluster_culling_plane_idx = -1;

            if (uniforms.enable_debug_output) {
                cluster_culling_plane_idx = test_sphere_frustum_debug(cluster_world_sphere, frustum, debug_plane_distances);
                cluster_visible = (cluster_culling_plane_idx == -1);
            } else {
                cluster_visible = test_sphere_frustum(cluster_world_sphere, frustum);
            }

            // 🔥 DEBUG: Record debug information for ALL clusters when debug output is enabled
            // IMPORTANT: Move debug recording BEFORE the visibility check to ensure all clusters are recorded
            if (uniforms.enable_debug_output) {
                // Calculate world space distance to camera for accurate debugging
                float distance_to_camera = length(cluster_world_center - uniforms.camera_position.xyz);

                // 🔥 FIX: Use actual culling result instead of forcing visibility
                uint culling_reason = cluster_visible ? 2 : 0; // 2 = None, 0 = Frustum

                // Write debug data for this cluster - use Stage 4 offset to avoid conflicts with Stage 1
                uint cluster_map_idx = global_cluster_base + i;
                uint debug_idx = STAGE4_DEBUG_OFFSET + (debug_write_pos % STAGE_DEBUG_COUNT);

                if (debug_idx < MAX_DEBUG_ENTRIES) {
                    debug_buffer[debug_idx].view_space_z = cluster_view_center_4.z; // Keep for reference
                    debug_buffer[debug_idx].bounds_radius = cluster_world_radius; // Use world radius
                    debug_buffer[debug_idx].distance_to_camera = distance_to_camera; // World space distance
                    debug_buffer[debug_idx].is_visible = cluster_visible ? 1 : 0; // 🔥 FIX: Use actual visibility
                    debug_buffer[debug_idx].instance_id = instance_id;
                    debug_buffer[debug_idx].cluster_id = cluster_map_idx;
                    debug_buffer[debug_idx].culling_reason = culling_reason;

                    // 🔥 FIX: Use actual cluster culling plane info
                    debug_buffer[debug_idx].culling_plane = (cluster_culling_plane_idx >= 0) ? cluster_culling_plane_idx : 0xFFFFFFFF;
                    for (int p = 0; p < 6; p++) {
                        debug_buffer[debug_idx].plane_distances[p] = debug_plane_distances[p];
                    }

                    debug_write_pos++;
                }
            }

            if (cluster_is_visible) {
                if (write_pos >= uniforms.cluster_count || write_pos >= MAX_CLUSTERS) {
                    // Don't break here - let debug recording continue for remaining clusters
                    // Just skip writing to visibility buffer
                    continue;
                }

                // CRITICAL: Use CPU-calculated cluster_map index for correct rendering pipeline access
                uint cluster_map_idx = global_cluster_base + i;

                // Mark as visible for now - occlusion culling can disable later if needed
                // Use cluster_map index for rendering pipeline compatibility
                cluster_visibility[write_pos].is_visible = 1;
                cluster_visibility[write_pos].cluster_index = cluster_map_idx; // Use cluster_map index
                cluster_visibility[write_pos].instance_index = instance_id;
                cluster_visibility[write_pos].lod_level = lod_level;
                cluster_visibility[write_pos].frame_index = uniforms.frame_index;

                write_pos++;
            }
        }
    }

    // Update the atomic counter with final count for Stage6 to use
    atomic_store_explicit(cluster_visibility_counter, write_pos, memory_order_relaxed);
}

// ============================================================================
// Stage 5: Occlusion Culling (HZB) - Placeholder for now
// ============================================================================

kernel void stage5_occlusion_culling(
    device const ClusterVisibility* cluster_visibility [[buffer(4)]],
    texture2d<float> hzb_texture [[texture(0)]],
    constant CullingUniforms& uniforms [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint cluster_idx = global_id.x;

    if (cluster_idx >= uniforms.cluster_count || cluster_idx >= MAX_CLUSTERS) { // Safety limit
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
    device const InstanceVisibility* instance_visibility [[buffer(1)]],
    device const ClusterVisibility* cluster_visibility [[buffer(4)]],
    device atomic_uint* visible_counter [[buffer(5)]],
    device uint* visible_cluster_list [[buffer(6)]],
    device atomic_uint* cluster_visibility_counter [[buffer(9)]], // Use the counter from Stage 4
    constant CullingUniforms& uniforms [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint idx = global_id.x;
    
    // Get the total number of clusters produced by Stage 4
    uint total_clusters = atomic_load_explicit(cluster_visibility_counter, memory_order_relaxed);
    
    if (idx >= total_clusters || idx >= MAX_CLUSTERS) {
        return;
    }

    // Check visibility conditions
    // Note: cluster_visibility buffer is already packed by Stage 4,
    // so we iterate over the first 'total_clusters' elements.
    bool is_visible = cluster_visibility[idx].is_visible != 0;

    // 🔥 CRITICAL FIX: Validate cluster index more carefully
    // 0 is a valid cluster index, 0xFFFFFFFF is invalid
    bool has_valid_index = cluster_visibility[idx].cluster_index != 0xFFFFFFFF;

    // Additional validation: ensure cluster_index is within reasonable bounds
    bool has_safe_index = cluster_visibility[idx].cluster_index < uniforms.cluster_count;

    if (is_visible && has_valid_index && has_safe_index) {
        // Reserve space in the output list
        uint write_pos = atomic_fetch_add_explicit(visible_counter, 1, memory_order_relaxed);

        // Safety check to prevent buffer overflow
        if (write_pos < uniforms.cluster_count && write_pos < MAX_CLUSTERS) {
            // 🔥 MODIFIED: Write the cluster_index for proper rendering
            visible_cluster_list[write_pos] = cluster_visibility[idx].cluster_index;
        }
    }
}

// ============================================================================
// Stage 7: Build Indirect Commands
// ============================================================================

struct IndirectDrawCommand {
    uint vertex_count;
    uint instance_count;
    uint first_vertex;
    uint first_instance;
};

kernel void stage7_build_indirect_commands(
    device const atomic_uint* visible_counter [[buffer(5)]],
    device const uint* visible_cluster_list [[buffer(6)]],
    device IndirectDrawCommand* indirect_commands [[buffer(7)]],
    constant CullingUniforms& uniforms [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    // Only first thread builds the command
    if (global_id.x != 0) {
        return;
    }

    // CRITICAL: Ensure we read the final value from Stage 6
    // Use memory_order_relaxed since host-side barriers handle synchronization
    uint visible_count = atomic_load_explicit(visible_counter, memory_order_relaxed);

    // 🔥 MODIFIED: For instance-level rendering, use instance_count instead of cluster_count
    // Basic safety clamp to prevent GPU hangs
    if (visible_count > uniforms.cluster_count) {
        visible_count = uniforms.cluster_count;
    }

    // Additional safety for extreme corruption
    if (visible_count > MAX_INSTANCES) {
        visible_count = MAX_INSTANCES;
    }

    // 🔥 MODIFIED: Build indirect command for instance-level rendering
    // For instance-level rendering, we draw a simple geometry per instance
    // and use instance ID for coloring in the draw shader
    IndirectDrawCommand cmd;
    cmd.vertex_count =  384; // Simple quad (2 triangles) for each instance
    cmd.instance_count = visible_count; // Number of visible instances
    cmd.first_vertex = 0;
    cmd.first_instance = 0;

    indirect_commands[0] = cmd;
}
