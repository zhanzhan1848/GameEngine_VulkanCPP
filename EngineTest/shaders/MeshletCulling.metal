#include <metal_stdlib>
using namespace metal;

// Meshlet structure (matches RHIMeshlet)
struct Meshlet {
    uint vertex_offset;
    uint triangle_offset;
    uint vertex_count;
    uint triangle_count;
    float cone_apex[3];
    float cone_axis[3];
    float cone_cutoff;
    float center[3];
    float radius;
    uint padding;
};

// Culling uniforms
struct CullingUniforms {
    float4x4 view_projection;
    uint meshlet_count;
    uint padding[3];
};

// Indirect draw command (matches VkDrawIndirectCommand)
struct DrawIndirectCommand {
    uint vertex_count;
    uint instance_count;
    uint first_vertex;
    uint first_instance;
};

// Extract frustum planes from view-projection matrix
// Returns 6 planes: left, right, bottom, top, near, far
// Each plane is (normal.xyz, distance)
float4 extract_frustum_planes(float4x4 vp, int plane_index) {
    // Plane extraction from VP matrix
    // Left:   row3 + row0
    // Right:  row3 - row0
    // Bottom: row3 + row1
    // Top:    row3 - row1
    // Near:   row3 + row2
    // Far:    row3 - row2
    
    float4 plane;
    
    if (plane_index == 0) {        // Left
        plane = vp[3] + vp[0];
    } else if (plane_index == 1) { // Right
        plane = vp[3] - vp[0];
    } else if (plane_index == 2) { // Bottom
        plane = vp[3] + vp[1];
    } else if (plane_index == 3) { // Top
        plane = vp[3] - vp[1];
    } else if (plane_index == 4) { // Near
        plane = vp[3] + vp[2];
    } else {                        // Far
        plane = vp[3] - vp[2];
    }
    
    // Normalize plane
    float len = length(plane.xyz);
    return float4(plane.xyz / len, plane.w / len);
}

// Test if sphere intersects frustum
bool sphere_in_frustum(float3 center, float radius, float4x4 vp) {
    for (int i = 0; i < 6; i++) {
        float4 plane = extract_frustum_planes(vp, i);
        float distance = dot(plane.xyz, center) + plane.w;
        if (distance < -radius) {
            return false; // Sphere is outside this plane
        }
    }
    return true; // Sphere intersects or inside frustum
}

// Meshlet culling compute shader
// One thread per meshlet
kernel void meshlet_culling_cs(
    constant CullingUniforms& uniforms [[buffer(0)]],
    constant Meshlet* meshlets [[buffer(1)]],
    device DrawIndirectCommand& indirect_cmd [[buffer(2)]],  // Single command for all visible meshlets
    device uint* visible_meshlet_indices [[buffer(3)]],       // Output: indices of visible meshlets
    device atomic_uint& visible_count [[buffer(4)]],          // Output: count of visible meshlets
    uint gid [[thread_position_in_grid]])
{
    uint meshlet_idx = gid;
    
    // Bounds check
    if (meshlet_idx >= uniforms.meshlet_count) {
        return;
    }
    
    Meshlet m = meshlets[meshlet_idx];
    
    // Get bounding sphere from meshlet
    float3 center = float3(m.center[0], m.center[1], m.center[2]);
    float radius = m.radius;
    
    // Frustum culling test
    bool visible = sphere_in_frustum(center, radius, uniforms.view_projection);
    
    if (visible) {
        // Add to visible list
        uint idx = atomic_fetch_add_explicit(&visible_count, 1, memory_order_relaxed);
        visible_meshlet_indices[idx] = meshlet_idx;
    }
}

// Multi-draw command generation for true GPU-driven rendering
// Each visible meshlet gets its own draw command
kernel void generate_multi_draw_commands_cs(
    constant uint& visible_count [[buffer(0)]],
    constant uint* visible_meshlet_indices [[buffer(1)]],
    constant Meshlet* meshlets [[buffer(2)]],
    device DrawIndirectCommand* draw_commands [[buffer(3)]],  // Output: array of draw commands
    uint gid [[thread_position_in_grid]])
{
    if (gid >= visible_count) return;

    uint meshlet_idx = visible_meshlet_indices[gid];
    Meshlet m = meshlets[meshlet_idx];

    // Generate individual draw command for this meshlet
    draw_commands[gid].vertex_count = m.triangle_count * 3;  // 3 vertices per triangle
    draw_commands[gid].instance_count = 1;                   // Single instance per meshlet
    draw_commands[gid].first_vertex = m.vertex_offset;       // Start from this meshlet's vertices
    draw_commands[gid].first_instance = 0;
}

// Instance culling and LOD selection
struct InstanceData {
    float4x4 transform;
    uint instance_id;
    uint visible;
    uint2 padding;
};

struct CameraData {
    float4x4 view_matrix;
    float4x4 projection_matrix;
    float4x4 view_projection_matrix;
    float3 camera_position;
    float near_plane;
    float far_plane;
    float2 padding;
};

kernel void instance_culling_cs(
    constant CameraData& camera [[buffer(0)]],
    device const InstanceData* instances [[buffer(1)]],
    device atomic_uint* visible_instance_count [[buffer(2)]],
    device uint* visible_instance_indices [[buffer(3)]],
    uint gid [[thread_position_in_grid]],
    uint total_instances [[threads_per_grid]])
{
    if (gid >= total_instances) return;

    device const InstanceData& instance = instances[gid];

    // Simple frustum culling for instance
    float4 world_pos = instance.transform * float4(0, 0, 0, 1);
    float4 view_pos = camera.view_matrix * world_pos;

    // Distance culling
    float distance = length(view_pos.xyz);
    bool visible = (distance >= camera.near_plane && distance <= camera.far_plane);

    if (visible) {
        uint idx = atomic_fetch_add_explicit(&visible_instance_count[0], 1, memory_order_relaxed);
        visible_instance_indices[idx] = gid;
    }
}

// LOD selection based on screen space error
kernel void lod_selection_cs(
    constant CameraData& camera [[buffer(0)]],
    constant Meshlet* meshlets [[buffer(1)]],
    constant uint* visible_meshlet_indices [[buffer(2)]],
    constant uint& visible_count [[buffer(3)]],
    device uint* selected_lods [[buffer(4)]],  // Output: LOD level for each visible meshlet
    uint gid [[thread_position_in_grid]])
{
    if (gid >= visible_count) return;

    uint meshlet_idx = visible_meshlet_indices[gid];
    Meshlet m = meshlets[meshlet_idx];

    // Calculate screen space size
    float3 center = float3(m.center[0], m.center[1], m.center[2]);
    float4 view_center = camera.view_matrix * float4(center, 1.0);
    float distance = length(view_center.xyz);

    // Approximate screen size (conservative estimate)
    float screen_size = m.radius / (distance * 0.001);

    // Simple LOD selection
    uint lod_level = 0;
    if (screen_size < 0.5) lod_level = 1;
    if (screen_size < 0.25) lod_level = 2;
    if (screen_size < 0.1) lod_level = 3;

    selected_lods[gid] = lod_level;
}
