#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

struct StreamingFeedbackConstants {
    uint32_t cluster_count;
    uint32_t max_requests;
    uint32_t frame_index;
    uint32_t padding;
};

struct ClusterRequest {
    uint32_t cluster_id;
    uint32_t priority;
    uint32_t access_time;
    uint32_t padding;
};

struct ResidencyData {
    uint32_t is_resident;
    uint32_t last_access_frame;
    uint32_t page_index;
    uint32_t padding;
};

kernel void streaming_feedback_shader(
    device const ResidencyData* residency_buffer [[buffer(0)]],
    device ClusterRequest* request_buffer [[buffer(1)]],
    device atomic_uint& request_count [[buffer(2)]],
    constant StreamingFeedbackConstants& constants [[buffer(3)]],
    uint3 global_id [[thread_position_in_grid]])
{
    const uint32_t cluster_id = global_id.x;
    
    if (cluster_id >= constants.cluster_count) {
        return;
    }
    
    device const ResidencyData& residency = residency_buffer[cluster_id];
    
    bool needs_request = (residency.is_resident == 0);
    
    if (needs_request) {
        uint32_t request_index = atomic_fetch_add_explicit(&request_count, 1, memory_order_relaxed);
        
        if (request_index < constants.max_requests) {
            device ClusterRequest& request = request_buffer[request_index];
            request.cluster_id = cluster_id;
            request.priority = 0;
            request.access_time = constants.frame_index;
        }
    }
}

kernel void update_access_time_shader(
    device ResidencyData* residency_buffer [[buffer(0)]],
    device const uint32_t* visible_clusters [[buffer(1)]],
    constant StreamingFeedbackConstants& constants [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    const uint32_t index = global_id.x;
    
    if (index >= constants.cluster_count) {
        return;
    }
    
    const uint32_t cluster_id = visible_clusters[index];
    
    if (cluster_id < constants.cluster_count && residency_buffer[cluster_id].is_resident) {
        residency_buffer[cluster_id].last_access_frame = constants.frame_index;
    }
}

kernel void check_residency_shader(
    device const ResidencyData* residency_buffer [[buffer(0)]],
    device uint32_t* visibility_mask [[buffer(1)]],
    device ClusterRequest* request_buffer [[buffer(2)]],
    device atomic_uint& request_count [[buffer(3)]],
    constant StreamingFeedbackConstants& constants [[buffer(4)]],
    uint3 global_id [[thread_position_in_grid]])
{
    const uint32_t cluster_id = global_id.x;
    
    if (cluster_id >= constants.cluster_count) {
        return;
    }
    
    device const ResidencyData& residency = residency_buffer[cluster_id];
    
    if (!residency.is_resident) {
        uint32_t request_index = atomic_fetch_add_explicit(&request_count, 1, memory_order_relaxed);
        
        if (request_index < constants.max_requests) {
            request_buffer[request_index].cluster_id = cluster_id;
            request_buffer[request_index].priority = 1;
            request_buffer[request_index].access_time = constants.frame_index;
        }
        
        uint32_t word_index = cluster_id / 32;
        uint32_t bit_index = cluster_id % 32;
        atomic_fetch_and_explicit(
            reinterpret_cast<device atomic_uint*>(&visibility_mask[word_index]),
            ~(1u << bit_index),
            memory_order_relaxed
        );
    }
}
