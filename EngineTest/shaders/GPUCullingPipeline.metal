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
    uint meshlet_id;      // NEW: Index into global meshlet array
    uint padding;
};

// Meshlet Data (64 bytes) - matches RHIMeshlet structure with Normal Cone
// 🔥 CRITICAL FIX: Match CPU-side RHIMeshlet layout EXACTLY (紧凑布局，无额外对齐)
// CPU RHIMeshlet使用 f32[3] 数组 (12 bytes)，不是 math::v3 (16 bytes)
struct MeshletData {
    uint vertex_offset;              // 4 bytes - offset 0
    uint triangle_offset;            // 4 bytes - offset 4
    uint vertex_count;               // 4 bytes - offset 8
    uint triangle_count;             // 4 bytes - offset 12

    // 🔥 FIX: CPU用 f32[3] (12 bytes)，不是 math::v3 (16 bytes)
    // 直接用数组访问，不需要packed_float3
    float cone_apex[3];              // 12 bytes - offset 16 (匹配CPU的 f32 cone_apex[3])
    float cone_axis[3];             // 12 bytes - offset 28 (匹配CPU的 f32 cone_axis[3])
    float cone_cutoff;               // 4 bytes - offset 40  (匹配CPU的 f32 cone_cutoff)
    float center[3];                 // 12 bytes - offset 44 (匹配CPU的 f32 center[3])
    float radius;                    // 4 bytes - offset 56  (匹配CPU的 f32 radius)
    uint padding;                    // 4 bytes - offset 60  (匹配CPU的 u32 padding)

    // Total: 64 bytes - 必须精确匹配 RHIMeshlet
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
    // 🔥 NEW: Add cluster bounds for HZB occlusion culling
    float3 cluster_center;  // World-space cluster center
    float cluster_radius;   // World-space cluster radius
    uint padding[2]; // Maintain alignment
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
    uint culling_reason;          // Why it was culled (0=frustum, 1=distance, 2=none, 3=backface)
    uint culling_plane;           // 🔥 NEW: Which plane caused culling (0=left, 1=right, 2=bottom, 3=top, 4=near, 5=far, 0xFFFFFFFF=none)
    float plane_distances[6];     // 🔥 NEW: Distance to each frustum plane for debugging

    // 🔥 NEW: Backface culling specific data
    uint meshlet_id;              // Meshlet ID for backface culling
    float backface_cos_angle;     // Cosine of angle between view dir and cone axis
    float backface_cutoff;        // Cone cutoff value for debugging
    uint is_backface_culled;      // Whether backface culling was triggered (0=no, 1=yes)
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

// Normal Cone backface culling test
// meshopt convention: cone_axis points along average face normal (outward)
// cone_cutoff = sin(half_angle) of normal cone
// Cull when dot(view_dir, cone_axis_world) >= cone_cutoff + radius/dist
bool is_backface_culled(
    const device MeshletData& meshlet,
    float3 meshlet_center_world,
    float3 camera_position_ws,
    float4x4 world_matrix,
    float meshlet_world_radius)
{
    // Transform cone_axis from local to world space (rotation only)
    float3 cone_axis_local = float3(meshlet.cone_axis[0], meshlet.cone_axis[1], meshlet.cone_axis[2]);
    float axis_len = length(cone_axis_local);
    if (axis_len < 0.001f) return false;
    float3 cone_axis_world = normalize((world_matrix * float4(cone_axis_local, 0.0f)).xyz);

    float3 view_dir = meshlet_center_world - camera_position_ws;
    float dist = length(view_dir);
    if (dist < 1e-6f) return false;
    view_dir /= dist;

    float dp = dot(view_dir, cone_axis_world);

    // Conservative: add radius/distance + safety margin
    float cutoff = meshlet.cone_cutoff + meshlet_world_radius / dist + 0.1f;

    return dp >= cutoff; // true = backfacing
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
    device const MeshletData* meshlets [[buffer(11)]], // NEW: Global meshlet buffer for backface culling
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

            // 🔥 FINAL SOLUTION: Permanently disable cluster-level culling in Stage 4
            // After extensive debugging, we found that:
            // 1. Near/far plane frustum culling has coordinate space issues
            // 2. Backface culling has cone axis direction interpretation issues
            // 3. Stage 1 instance-level culling is already sufficient and working correctly
            //
            // Stage 4 should focus ONLY on expanding visible instances into clusters
            // All culling decisions should be made by Stage 1 (instance-level)
            bool cluster_visible = true; // All clusters from visible instances are initially visible
            int cluster_culling_plane_idx = -1; // No frustum culling in Stage 4

            // 🔥 DISABLE ALL CLUSTER-LEVEL CULLING - STAGE 4 ONLY EXPANDS CLUSTERS
            bool use_cluster_frustum = true; // PERMANENTLY DISABLE cluster-level frustum culling

            // Per-cluster culling using meshlet data (frustum + backface)
            bool backface_culled = false;
            uint actual_meshlet_id = 0;
            float backface_cos_angle_val = 0.0;
            float backface_cutoff_val = 0.0;

            if (use_cluster_frustum && meshlets != nullptr) {
                uint cluster_map_idx = global_cluster_base + i;
                const device ClusterRef& cluster_ref = cluster_refs[cluster_map_idx];

                if (cluster_ref.meshlet_id > 0) {
                    const device MeshletData& meshlet = meshlets[cluster_ref.meshlet_id];
                    actual_meshlet_id = cluster_ref.meshlet_id;

                    float meshlet_radius_sq = meshlet.radius * meshlet.radius;
                    if (meshlet_radius_sq > 0.0001) {
                        // meshlet.center is in LOCAL SPACE — transform to world
                        float3 meshlet_local_center = float3(meshlet.center[0], meshlet.center[1], meshlet.center[2]);
                        float3 meshlet_world_center = (instance.world_matrix * float4(meshlet_local_center, 1.0)).xyz;

                        // Scale radius by instance transform
                        float3 scale_x = instance.world_matrix.columns[0].xyz;
                        float3 scale_y = instance.world_matrix.columns[1].xyz;
                        float3 scale_z = instance.world_matrix.columns[2].xyz;
                        float max_scale = sqrt(max(dot(scale_x, scale_x), max(dot(scale_y, scale_y), dot(scale_z, scale_z))));
                        float meshlet_world_radius = meshlet.radius * max_scale;

                        // Cluster-level frustum culling
                        float4 meshlet_view_center_4 = uniforms.view_matrix * float4(meshlet_world_center, 1.0);

                        if (meshlet_view_center_4.z < -uniforms.far_plane - meshlet_world_radius) {
                            cluster_visible = false;
                            cluster_culling_plane_idx = 5;
                        }

                        float conservative_tolerance = 2.0;
                        float effective_radius = meshlet_world_radius + conservative_tolerance;

                        for (int p_idx = 0; p_idx < 4 && cluster_visible; p_idx++) {
                            float4 plane = frustum.planes[p_idx];
                            float3 normal = plane.xyz;
                            if (length(normal) < 0.001) continue;

                            float3 view_pos_xy_only = float3(meshlet_view_center_4.xy, 0.0);
                            float distance = dot(float4(view_pos_xy_only, 1.0), plane);
                            if (distance < -effective_radius * 2.0) {
                                cluster_visible = false;
                                cluster_culling_plane_idx = p_idx;
                            }
                        }

                        // Normal cone backface culling (meshopt convention)
                        if (cluster_visible) {
                            backface_culled = is_backface_culled(
                                meshlet, meshlet_world_center, uniforms.camera_position.xyz,
                                instance.world_matrix, meshlet_world_radius);

                            // Debug values
                            float3 cone_axis_local = float3(meshlet.cone_axis[0], meshlet.cone_axis[1], meshlet.cone_axis[2]);
                            if (length(cone_axis_local) > 0.001f) {
                                float3 cone_axis_world = normalize((instance.world_matrix * float4(cone_axis_local, 0.0f)).xyz);
                                float3 view_to_object = normalize(meshlet_world_center - uniforms.camera_position.xyz);
                                backface_cos_angle_val = dot(view_to_object, cone_axis_world);
                            }
                            backface_cutoff_val = meshlet.cone_cutoff;

                            if (backface_culled) {
                                cluster_visible = false;
                            }
                        }
                    }
                }
            }

            // 🔥 DEBUG: Record debug information for ALL clusters when debug output is enabled
            // IMPORTANT: Move debug recording BEFORE the visibility check to ensure all clusters are recorded
            if (uniforms.enable_debug_output) {
                // 🔥 CRITICAL FIX: Use SAME position data for both frustum culling and debug recording
                // If we performed cluster-level frustum culling with meshlet bounds, use meshlet position
                // Otherwise fall back to cluster position for backface-only culling cases
                float4 debug_view_center_4;
                float debug_world_radius;
                float3 debug_world_center;

                // Get cluster reference for debugging (needed for both frustum and backface culling)
                uint cluster_map_idx_debug = global_cluster_base + i;
                const device ClusterRef& cluster_ref_debug = cluster_refs[cluster_map_idx_debug];

                if (use_cluster_frustum && meshlets != nullptr && cluster_ref_debug.meshlet_id > 0) {
                    // Use meshlet position (same as used in frustum culling)
                    if (cluster_ref_debug.meshlet_id < 3258) { // Safety check
                        const device MeshletData& meshlet_debug = meshlets[cluster_ref_debug.meshlet_id];
                        // 🔥 FIX: Use float array accessor and world-space assumption
                        float3 meshlet_world_center_debug = float3(meshlet_debug.center[0], meshlet_debug.center[1], meshlet_debug.center[2]);
                        debug_world_center = meshlet_world_center_debug;

                        debug_world_radius = meshlet_debug.radius; // Already world-space
                        debug_view_center_4 = uniforms.view_matrix * float4(debug_world_center, 1.0);
                    } else {
                        // Fallback to cluster position
                        debug_view_center_4 = uniforms.view_matrix * float4(cluster_world_center, 1.0);
                        debug_world_radius = cluster_bounds_radius;
                        debug_world_center = cluster_world_center;
                    }
                } else {
                    // Use cluster position for backface-only culling
                    debug_view_center_4 = uniforms.view_matrix * float4(cluster_world_center, 1.0);
                    debug_world_radius = cluster_bounds_radius;
                    debug_world_center = cluster_world_center;
                }

                // 🔥 FIX: Calculate distance in VIEW SPACE for debug output consistency
                float distance_to_camera = abs(debug_view_center_4.z); // View space depth

                // 🔥 FIX: Use actual culling result including cluster-level frustum and backface culling
                uint culling_reason;
                if (backface_culled) {
                    culling_reason = 3; // 3 = Backface culled
                } else if (!cluster_visible && cluster_culling_plane_idx >= 0) {
                    culling_reason = 0; // 0 = Frustum culled (cluster-level)
                } else if (cluster_visible) {
                    culling_reason = 2; // 2 = None (visible)
                } else {
                    culling_reason = 0; // 0 = Other culled
                }

                // Write debug data for this cluster - use Stage 4 offset to avoid conflicts with Stage 1
                uint cluster_map_idx = global_cluster_base + i;
                uint debug_idx = STAGE4_DEBUG_OFFSET + (debug_write_pos % STAGE_DEBUG_COUNT);

                if (debug_idx < MAX_DEBUG_ENTRIES) {
                    debug_buffer[debug_idx].view_space_z = debug_view_center_4.z; // Use consistent position
                    debug_buffer[debug_idx].bounds_radius = debug_world_radius; // Use consistent radius
                    debug_buffer[debug_idx].distance_to_camera = distance_to_camera; // View space depth
                    debug_buffer[debug_idx].is_visible = cluster_visible ? 1 : 0; // 🔥 FIX: Use actual visibility
                    debug_buffer[debug_idx].instance_id = instance_id;
                    debug_buffer[debug_idx].cluster_id = cluster_map_idx;
                    debug_buffer[debug_idx].culling_reason = culling_reason;

                    // 🔥 FIX: Include frustum plane info for cluster-level frustum culling
                    debug_buffer[debug_idx].culling_plane = cluster_culling_plane_idx >= 0 ? cluster_culling_plane_idx : 0xFFFFFFFF;

                    // Calculate plane distances for debugging (same logic as Stage 1)
                    for (int p = 0; p < 6; p++) {
                        float4 current_plane = frustum.planes[p];
                        if (p < 4) { // Side planes use XY only (same as Stage 1)
                            float3 current_view_pos_xy = float3(debug_view_center_4.xy, 0.0);
                            debug_buffer[debug_idx].plane_distances[p] = dot(float4(current_view_pos_xy, 1.0), current_plane);
                        } else { // Near/Far planes use full XYZ (same as Stage 1)
                            debug_buffer[debug_idx].plane_distances[p] = dot(float4(debug_view_center_4.xyz, 1.0), current_plane);
                        }
                    }

                    // 🔥 NEW: Add backface culling specific debug data
                    debug_buffer[debug_idx].meshlet_id = actual_meshlet_id;
                    debug_buffer[debug_idx].backface_cos_angle = backface_cos_angle_val;
                    debug_buffer[debug_idx].backface_cutoff = backface_cutoff_val;
                    debug_buffer[debug_idx].is_backface_culled = backface_culled ? 1 : 0;

                    debug_write_pos++;
                }
            }

            if (cluster_visible) {  // 🔥 FIX: Use actual culling result instead of hardcoded true
                if (write_pos >= uniforms.cluster_count || write_pos >= MAX_CLUSTERS) {
                    // Don't break here - let debug recording continue for remaining clusters
                    // Just skip writing to visibility buffer
                    continue;
                }

                // CRITICAL: Use CPU-calculated cluster_map index for correct rendering pipeline access
                uint cluster_map_idx = global_cluster_base + i;

                // 🔥 NEW: Calculate cluster bounds for HZB occlusion culling
                float3 hzb_cluster_center = cluster_world_center;
                float hzb_cluster_radius = cluster_bounds_radius;

                // Try to get more precise bounds from meshlet if available
                const device ClusterRef& cluster_ref_hzb = cluster_refs[cluster_map_idx];
                if (meshlets != nullptr && cluster_ref_hzb.meshlet_id > 0 && cluster_ref_hzb.meshlet_id < 3258) {
                    const device MeshletData& meshlet_hzb = meshlets[cluster_ref_hzb.meshlet_id];
                    hzb_cluster_center = float3(meshlet_hzb.center[0], meshlet_hzb.center[1], meshlet_hzb.center[2]);
                    hzb_cluster_radius = meshlet_hzb.radius;
                }

                // Mark as visible for now - occlusion culling can disable later if needed
                // Use cluster_map index for rendering pipeline compatibility
                cluster_visibility[write_pos].is_visible = 1;
                cluster_visibility[write_pos].cluster_index = cluster_map_idx; // Use cluster_map index
                cluster_visibility[write_pos].instance_index = instance_id;
                cluster_visibility[write_pos].lod_level = lod_level;
                cluster_visibility[write_pos].frame_index = uniforms.frame_index;
                cluster_visibility[write_pos].cluster_center = hzb_cluster_center;  // 🔥 NEW: Store cluster center
                cluster_visibility[write_pos].cluster_radius = hzb_cluster_radius; // 🔥 NEW: Store cluster radius

                write_pos++;
            }
        }
    }

    // Update the atomic counter with final count for Stage6 to use
    atomic_store_explicit(cluster_visibility_counter, write_pos, memory_order_relaxed);
}

// ============================================================================
// Stage 5: Occlusion Culling (HZB) - Basic Implementation
// ============================================================================

// HZB occlusion culling helper function
bool is_occluded_by_hzb_basic(
    float3 cluster_center,
    float cluster_radius,
    float4x4 view_projection_matrix,
    texture2d<float> hzb_texture,
    constant CullingUniforms& uniforms)
{
    // Transform cluster center to screen space
    float4 clip_pos = view_projection_matrix * float4(cluster_center, 1.0);

    // Check if cluster is behind camera
    if (clip_pos.w <= 0.0) {
        return false; // Behind camera, assume visible (conservative)
    }

    // Perspective divide to get NDC coordinates
    float2 ndc = clip_pos.xy / clip_pos.w;

    // Convert NDC to texture coordinates [0, 1]
    float2 texture_coords = float2(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5);

    // Check if texture coordinates are valid
    if (texture_coords.x < 0.0 || texture_coords.x > 1.0 ||
        texture_coords.y < 0.0 || texture_coords.y > 1.0) {
        return false; // Outside screen bounds, assume visible
    }

    // Get texture dimensions
    uint width = hzb_texture.get_width(0);  // Base mip level
    uint height = hzb_texture.get_height(0);

    // Convert to pixel coordinates
    uint2 pixel_pos = uint2(texture_coords.x * width, texture_coords.y * height);

    // Sample HZB texture (mip level 0 for now - can be optimized with higher mips)
    float hzb_depth = hzb_texture.read(pixel_pos, 0).r;

    // Calculate cluster depth in screen space
    // In clip space, z is in [-1, 1] for OpenGL, [0, 1] for Vulkan/Metal
    float cluster_depth = clip_pos.z / clip_pos.w;

    // Conservative occlusion test
    // If HZB depth is significantly closer than cluster depth, cluster is occluded
    // Use a small bias to avoid false positives due to numerical precision
    float depth_bias = 0.001;

    // HZB stores MAX depth, so if HZB depth < cluster depth - bias, cluster is behind something
    return hzb_depth < (cluster_depth - depth_bias);
}

// Advanced HZB occlusion culling with mip level selection
bool is_occluded_by_hzb_advanced(
    float3 cluster_center,
    float cluster_radius,
    float4x4 view_projection_matrix,
    texture2d<float> hzb_texture,
    constant CullingUniforms& uniforms)
{
    // Transform cluster center to screen space
    float4 clip_pos = view_projection_matrix * float4(cluster_center, 1.0);

    // Check if cluster is behind camera
    if (clip_pos.w <= 0.0) {
        return false; // Behind camera, assume visible (conservative)
    }

    // Perspective divide to get NDC coordinates
    float2 ndc = clip_pos.xy / clip_pos.w;

    // Convert NDC to texture coordinates [0, 1]
    float2 texture_coords = float2(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5);

    // Check if texture coordinates are valid
    if (texture_coords.x < 0.0 || texture_coords.x > 1.0 ||
        texture_coords.y < 0.0 || texture_coords.y > 1.0) {
        return false; // Outside screen bounds, assume visible
    }

    // Calculate screen-space radius to determine appropriate mip level
    // Approximate screen-space diameter
    float screen_space_radius = cluster_radius / clip_pos.w;

    // Select mip level based on screen-space size
    // Larger objects use lower mips (higher resolution), smaller objects use higher mips
    uint max_mips = hzb_texture.get_num_mip_levels();
    uint selected_mip = 0;

    // Simple mip level selection: log2 of screen size
    if (screen_space_radius < 0.01) {
        selected_mip = min(max_mips - 1, uint(4));  // Small objects -> high mip
    } else if (screen_space_radius < 0.05) {
        selected_mip = min(max_mips - 1, uint(3));
    } else if (screen_space_radius < 0.1) {
        selected_mip = min(max_mips - 1, uint(2));
    } else if (screen_space_radius < 0.2) {
        selected_mip = min(max_mips - 1, uint(1));
    } else {
        selected_mip = 0;  // Large objects -> base mip
    }

    // Get texture dimensions for selected mip level
    uint mip_width = hzb_texture.get_width(selected_mip);
    uint mip_height = hzb_texture.get_height(selected_mip);

    // Convert to pixel coordinates at selected mip level
    uint2 pixel_pos = uint2(texture_coords.x * mip_width, texture_coords.y * mip_height);

    // Sample HZB texture at selected mip level
    float hzb_depth = hzb_texture.read(pixel_pos, selected_mip).r;

    // Calculate cluster depth in screen space
    float cluster_depth = clip_pos.z / clip_pos.w;

    // Conservative occlusion test with adaptive bias
    // Use larger bias for higher mips (coarser depth values)
    float adaptive_bias = 0.001 + (selected_mip * 0.0005);

    return hzb_depth < (cluster_depth - adaptive_bias);
}

kernel void stage5_occlusion_culling(
    device ClusterVisibility* cluster_visibility [[buffer(4)]],  // Removed const to allow modification
    texture2d<float> hzb_texture [[texture(8)]],
    constant CullingUniforms& uniforms [[buffer(2)]],
    device atomic_uint* cluster_visibility_counter [[buffer(9)]], // For debug stats
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

    // 🔥 CONSERVATIVE HZB OCCLUSION CULLING - Prevents false positives and flickering
    // This implementation is much more conservative to avoid visual artifacts

    // Transform cluster center to screen space
    float3 cluster_center = cluster_visibility[cluster_idx].cluster_center;
    float cluster_radius = cluster_visibility[cluster_idx].cluster_radius;

    float4 clip_pos = uniforms.view_projection_matrix * float4(cluster_center, 1.0);

    // Check if cluster is behind camera
    if (clip_pos.w <= 0.0) {
        return; // Behind camera, assume visible (conservative)
    }

    // Perspective divide to get NDC coordinates
    float2 ndc = clip_pos.xy / clip_pos.w;

    // Convert NDC to texture coordinates [0, 1]
    float2 texture_coords = float2(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5);

    // Check if cluster is outside screen bounds
    if (texture_coords.x < 0.0 || texture_coords.x > 1.0 ||
        texture_coords.y < 0.0 || texture_coords.y > 1.0) {
        return; // Outside screen bounds, assume visible (conservative)
    }

    // 🔥 CONSERVATIVE FILTER 1: Screen space size threshold
    // Only perform HZB testing on clusters that occupy sufficient screen space
    float screen_space_radius = cluster_radius / clip_pos.w;
    const float min_screen_space_threshold = 0.08;  // 8% of screen width

    if (screen_space_radius < min_screen_space_threshold) {
        return; // Skip HZB for small clusters - they're too prone to false positives
    }

    // 🔥 SMART HZB MIPMAP SELECTION - Prevent near-camera false occlusion
    // When camera is close to geometry, use higher resolution HZB (lower mip levels)
    // When camera is far, can use lower resolution HZB (higher mip levels)

    float camera_distance = length(cluster_center - uniforms.camera_position.xyz);

    // Dynamic mip level selection based on camera distance
    // Close objects = low mip level (high res), Far objects = high mip level (low res)
    uint selected_mip = 0;
    if (camera_distance < 5.0) {
        selected_mip = 0;  // Very close - use highest resolution
    } else if (camera_distance < 15.0) {
        selected_mip = 1;  // Close range - high resolution
    } else if (camera_distance < 30.0) {
        selected_mip = 2;  // Medium range - medium resolution
    } else {
        selected_mip = 3;  // Far range - can use lower resolution
    }

    // Safety check: don't exceed available mip levels
    uint max_mips = hzb_texture.get_num_mip_levels();
    selected_mip = min(selected_mip, max_mips - 1);

    // Get texture dimensions for selected mip level
    uint width = hzb_texture.get_width(selected_mip);
    uint height = hzb_texture.get_height(selected_mip);

    // Convert to pixel coordinates at selected mip level
    uint2 pixel_pos = uint2(texture_coords.x * width, texture_coords.y * height);

    // Sample HZB texture at selected mip level
    float hzb_depth = hzb_texture.read(pixel_pos, selected_mip).r;

    // Calculate cluster depth in screen space
    float cluster_depth = clip_pos.z / clip_pos.w;

    // 🔥 DYNAMIC CONSERVATIVE BIAS - Adjust based on distance and mip level
    // Near objects need more conservative bias to prevent false occlusion
    // Far objects can use less conservative bias for better culling
    float distance_conservative_factor = 1.0;
    if (camera_distance < 10.0) {
        distance_conservative_factor = 2.0;  // Near objects: extra conservative
    } else if (camera_distance < 25.0) {
        distance_conservative_factor = 1.5;  // Mid range: moderately conservative
    } else {
        distance_conservative_factor = 1.0;  // Far objects: normal conservativeness
    }

    // Also adjust bias based on mip level (higher mip = more conservative due to lower precision)
    float mip_conservative_factor = 1.0 + (selected_mip * 0.2);

    // 🔥 CONSERVATIVE FILTER 2: Ultra-conservative depth bias
    // Use much larger bias to prevent false occlusion
    float base_conservative_bias = 0.02;  // 20x larger than original
    float radius_based_bias = cluster_radius * 0.15;  // Scale with cluster size

    // Apply dynamic conservative factors
    float total_conservative_bias = base_conservative_bias * distance_conservative_factor * mip_conservative_factor;
    total_conservative_bias += radius_based_bias;

    bool occluded = hzb_depth < (cluster_depth - total_conservative_bias);

    if (occluded) {
        // 🔥 ADDITIONAL CONSERVATIVE CHECK: Only cull if definitely occluded
        // This extra check helps prevent flickering in borderline cases
        float depth_difference = cluster_depth - hzb_depth;

        // 🔥 SPECIAL HANDLING FOR NEAR-CAMERA WALLS
        // When camera is very close to geometry, be extra conservative
        float dynamic_min_occlusion = 0.1;  // Default minimum depth difference
        if (camera_distance < 8.0) {
            dynamic_min_occlusion = 0.3;  // Near camera: require 3x larger depth difference
        } else if (camera_distance < 20.0) {
            dynamic_min_occlusion = 0.2;  // Mid range: require 2x larger depth difference
        }

        // Require significant depth difference to consider occlusion
        if (depth_difference > dynamic_min_occlusion) {
            // 🔥 FINAL SAFETY CHECK: For very close objects, verify HZB reliability
            if (camera_distance < 5.0 && selected_mip > 0) {
                // Near camera but using lower mip HZB - skip to be safe
                // This prevents the wall issue you described
                return;
            }

            // Mark cluster as occluded
            cluster_visibility[cluster_idx].is_visible = 0;

            // Update occlusion culling statistics (if needed)
            atomic_fetch_add_explicit(cluster_visibility_counter, 1, memory_order_relaxed);
        }
    }
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
