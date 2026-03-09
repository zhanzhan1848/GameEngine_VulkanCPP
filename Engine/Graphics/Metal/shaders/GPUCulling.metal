#include <metal_stdlib>
using namespace metal;

struct ClusterBounds {
    float3 min;
    float3 max;
    float screen_space_error;
};

struct LODData {
    uint lod_level;
    uint cluster_index;
    float distance_to_camera;
    float padding;
};

struct VisibilityData {
    uint is_visible;
    uint cluster_index;
    uint instance_index;
    uint padding;
};

struct CullingUniforms {
    float4x4 view_projection;
    float4x4 view_matrix;
    float3 camera_position;
    uint cluster_count;
    uint instance_count;
    float lod_bias;
    uint enable_occlusion;
    uint enable_lod_selection;
    uint enable_instance_culling;
};

kernel void stage1_frustum_culling(
    device const ClusterBounds* cluster_bounds [[buffer(0)]],
    device VisibilityData* visibility [[buffer(1)]],
    constant CullingUniforms& uniforms [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint index = global_id.x;
    
    if (index >= uniforms.cluster_count) {
        return;
    }
    
    device const ClusterBounds& bounds = cluster_bounds[index];
    
    float3 center = (bounds.min + bounds.max) * 0.5;
    float4 clip_pos = uniforms.view_projection * float4(center, 1.0);
    
    bool inside_frustum = true;
    
    if (clip_pos.w <= 0.0) inside_frustum = false;
    
    float w = clip_pos.w;
    if (clip_pos.x < -w || clip_pos.x > w) inside_frustum = false;
    if (clip_pos.y < -w || clip_pos.y > w) inside_frustum = false;
    if (clip_pos.z < 0.0 || clip_pos.z > w) inside_frustum = false;
    
    visibility[index].is_visible = inside_frustum ? 1 : 0;
    visibility[index].cluster_index = index;
    visibility[index].instance_index = 0;
}

kernel void stage2_occlusion_culling(
    device const ClusterBounds* cluster_bounds [[buffer(0)]],
    device VisibilityData* visibility [[buffer(1)]],
    texture2d<float, access::read> hiz_buffer [[texture(0)]],
    constant CullingUniforms& uniforms [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint index = global_id.x;
    
    if (index >= uniforms.cluster_count) {
        return;
    }
    
    if (visibility[index].is_visible == 0) {
        return;
    }
    
    device const ClusterBounds& bounds = cluster_bounds[index];
    
    float3 center = (bounds.min + bounds.max) * 0.5;
    float4 clip_pos = uniforms.view_projection * float4(center, 1.0);
    
    float2 screen_pos = clip_pos.xy / clip_pos.w;
    float depth = clip_pos.z / clip_pos.w;
    
    constexpr sampler hiz_sampler(mag_filter::nearest, min_filter::nearest);
    float hiz_depth = hiz_buffer.sample(hiz_sampler, screen_pos * 0.5 + 0.5).r;
    
    if (depth > hiz_depth + 0.001) {
        visibility[index].is_visible = 0;
    }
}

kernel void stage3_lod_selection(
    device const ClusterBounds* cluster_bounds [[buffer(0)]],
    device VisibilityData* visibility [[buffer(1)]],
    device LODData* lod_data [[buffer(2)]],
    constant CullingUniforms& uniforms [[buffer(3)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint index = global_id.x;
    
    if (index >= uniforms.cluster_count) {
        return;
    }
    
    if (visibility[index].is_visible == 0) {
        lod_data[index].lod_level = 0;
        return;
    }
    
    device const ClusterBounds& bounds = cluster_bounds[index];
    
    float3 center = (bounds.min + bounds.max) * 0.5;
    float distance_to_camera = length(center - uniforms.camera_position);
    
    float screen_space_error = bounds.screen_space_error / max(distance_to_camera, 0.001);
    
    uint lod_level = 0;
    if (screen_space_error < 0.01 * uniforms.lod_bias) {
        lod_level = 3;
    } else if (screen_space_error < 0.02 * uniforms.lod_bias) {
        lod_level = 2;
    } else if (screen_space_error < 0.05 * uniforms.lod_bias) {
        lod_level = 1;
    }
    
    lod_data[index].lod_level = lod_level;
    lod_data[index].cluster_index = index;
    lod_data[index].distance_to_camera = distance_to_camera;
}

kernel void stage4_instance_culling(
    device VisibilityData* visibility [[buffer(0)]],
    device VisibilityData* instance_visibility [[buffer(1)]],
    constant CullingUniforms& uniforms [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint instance_index = global_id.x;
    
    if (instance_index >= uniforms.instance_count) {
        return;
    }
    
    instance_visibility[instance_index].is_visible = 0;
    instance_visibility[instance_index].instance_index = instance_index;
    
    for (uint i = 0; i < uniforms.cluster_count; ++i) {
        if (visibility[i].is_visible && visibility[i].instance_index == instance_index) {
            instance_visibility[instance_index].is_visible = 1;
            break;
        }
    }
}
