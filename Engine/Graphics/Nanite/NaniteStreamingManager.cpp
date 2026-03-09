#include "NaniteStreamingManager.h"
#include "NaniteResourceManager.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHIResource.h"
#include <algorithm>

namespace primal::graphics::nanite {

namespace {
    constexpr u64 RESIDENCY_BUFFER_SIZE = 1024 * 1024;
    constexpr u64 REQUEST_BUFFER_SIZE = 512 * 1024;
}

bool NaniteStreamingManager::Initialize(rhi::RHIDeviceBase* device, const NaniteStreamingConfig& config) {
    if (initialized_) return true;
    if (!device) return false;
    if (config.page_pool_size_bytes == 0 || config.page_size_bytes == 0) return false;
    
    device_ = device;
    config_ = config;
    
    if (!AllocatePagePool() || !AllocateBuffers()) return false;
    
    const u32 page_count = static_cast<u32>(config.page_pool_size_bytes / config.page_size_bytes);
    free_pages_.reserve(page_count);
    for (u32 i = 0; i < page_count; ++i) {
        free_pages_.push_back(i);
    }
    
    initialized_ = true;
    return true;
}

void NaniteStreamingManager::Shutdown() {
    if (!initialized_) return;
    
    if (device_) {
        if (page_pool_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(page_pool_buffer_);
        if (residency_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(residency_buffer_);
        if (request_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(request_buffer_);
        if (feedback_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(feedback_buffer_);
        if (request_staging_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(request_staging_buffer_);
        if (feedback_staging_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(feedback_staging_buffer_);
    }
    
    free_pages_.clear();
    cluster_page_info_.clear();
    pending_requests_.clear();
    gpu_requests_.clear();
    cluster_last_access_.clear();
    
    stats_ = StreamingStats{};
    current_resident_count_ = 0;
    initialized_ = false;
}

bool NaniteStreamingManager::AllocatePagePool() {
    rhi::BufferDesc desc{};
    desc.size = config_.page_pool_size_bytes;
    desc.type = rhi::BufferType::Structured;
    desc.usage = rhi::GPUMemoryUsage::Static;
    desc.bindFlags = static_cast<u32>(rhi::ResourceUsage::UnorderedAccess);
    
    page_pool_buffer_ = device_->CreateBuffer(desc);
    return page_pool_buffer_ != rhi::handles::INVALID_RESOURCE;
}

bool NaniteStreamingManager::AllocateBuffers() {
    rhi::BufferDesc residency_desc{};
    residency_desc.size = RESIDENCY_BUFFER_SIZE;
    residency_desc.type = rhi::BufferType::Structured;
    residency_desc.usage = rhi::GPUMemoryUsage::Static;
    residency_desc.bindFlags = static_cast<u32>(rhi::ResourceUsage::UnorderedAccess);
    
    residency_buffer_ = device_->CreateBuffer(residency_desc);
    if (residency_buffer_ == rhi::handles::INVALID_RESOURCE) return false;
    
    rhi::BufferDesc request_desc{};
    request_desc.size = REQUEST_BUFFER_SIZE;
    request_desc.type = rhi::BufferType::Structured;
    request_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    request_desc.bindFlags = static_cast<u32>(rhi::ResourceUsage::UnorderedAccess);
    
    request_buffer_ = device_->CreateBuffer(request_desc);
    request_staging_buffer_ = device_->CreateBuffer(request_desc);
    
    rhi::BufferDesc feedback_desc{};
    feedback_desc.size = REQUEST_BUFFER_SIZE;
    feedback_desc.type = rhi::BufferType::Structured;
    feedback_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    feedback_desc.bindFlags = static_cast<u32>(rhi::ResourceUsage::UnorderedAccess);
    
    feedback_buffer_ = device_->CreateBuffer(feedback_desc);
    feedback_staging_buffer_ = device_->CreateBuffer(feedback_desc);
    
    return request_buffer_ != rhi::handles::INVALID_RESOURCE &&
           feedback_buffer_ != rhi::handles::INVALID_RESOURCE;
}

u32 NaniteStreamingManager::AllocatePage() {
    if (free_pages_.empty()) EvictLRUClusters();
    if (free_pages_.empty()) return UINT32_MAX;
    
    u32 page_index = free_pages_.back();
    free_pages_.erase(free_pages_.end() - 1);
    return page_index;
}

void NaniteStreamingManager::FreePage(u32 page_index) {
    if (page_index != UINT32_MAX) free_pages_.push_back(page_index);
}

void NaniteStreamingManager::ProcessRequests(u64 current_frame) {
    if (!initialized_) return;
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    void* mapped = device_->MapBuffer(feedback_staging_buffer_, 0, REQUEST_BUFFER_SIZE);
    if (!mapped) return;
    
    u32 request_count = 0;
    memcpy(&request_count, mapped, sizeof(u32));
    
    gpu_requests_.resize(request_count);
    if (request_count > 0) {
        memcpy(gpu_requests_.data(), static_cast<u8*>(mapped) + sizeof(u32),
               request_count * sizeof(StreamingRequest));
    }
    
    device_->UnmapBuffer(feedback_staging_buffer_);
    
    for (const auto& req : gpu_requests_) {
        RequestCluster(req.geometry_id, req.cluster_index, req.priority, req.camera_position);
    }
    gpu_requests_.clear();
    
    u32 processed = 0;
    std::sort(pending_requests_.begin(), pending_requests_.end(),
              [](const StreamingRequest& a, const StreamingRequest& b) { return a.priority > b.priority; });
    
    for (const auto& req : pending_requests_) {
        if (processed >= config_.max_requests_per_frame) break;
        
        u64 key = MakeClusterKey(req.geometry_id, req.cluster_index);
        
        if (!IsClusterResident(req.geometry_id, req.cluster_index)) {
            if (LoadClusterToGPU(req.geometry_id, req.cluster_index)) {
                processed++;
                stats_.total_clusters_streamed++;
            }
        }
        
        cluster_last_access_[key] = current_frame;
    }
    
    pending_requests_.clear();
    stats_.pending_requests_count = static_cast<u32>(pending_requests_.size());
}

void NaniteStreamingManager::UpdateLRU(u64 current_frame, const math::v3& camera_position) {
    if (!initialized_) return;
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    f32 pool_usage = 1.0f - (static_cast<f32>(free_pages_.size()) /
                               (config_.page_pool_size_bytes / config_.page_size_bytes));
    
    stats_.page_pool_usage = static_cast<u32>(pool_usage * 100.0f);
    
    if (pool_usage > config_.eviction_threshold) {
        EvictLRUClusters();
    }
}

bool NaniteStreamingManager::IsClusterResident(id::id_type geometry_id, u32 cluster_index) const {
    u64 key = MakeClusterKey(geometry_id, cluster_index);
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    auto it = cluster_page_info_.find(key);
    if (it != cluster_page_info_.end()) {
        return it->second.is_resident;
    }
    return false;
}

bool NaniteStreamingManager::RequestCluster(id::id_type geometry_id, u32 cluster_index,
                                             u32 priority, const math::v3& camera_position) {
    if (!initialized_) return false;
    
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    
    u64 key = MakeClusterKey(geometry_id, cluster_index);
    auto it = cluster_page_info_.find(key);
    if (it != cluster_page_info_.end() && it->second.is_resident) {
        auto access_it = cluster_last_access_.find(key);
        if (access_it != cluster_last_access_.end()) {
            access_it->second = priority;
        }
        return true;
    }
    
    StreamingRequest req;
    req.geometry_id = geometry_id;
    req.cluster_index = cluster_index;
    req.priority = priority;
    req.camera_position = camera_position;
    
    pending_requests_.push_back(req);
    stats_.pending_requests_count = static_cast<u32>(pending_requests_.size());
    
    return false;
}

void NaniteStreamingManager::EvictLRUClusters() {
    if (cluster_last_access_.empty()) return;

    utl::vector<std::pair<u64, u64>> access_times;
    access_times.reserve(cluster_last_access_.size());

    for (const auto& [key, frame] : cluster_last_access_) {
        access_times.emplace_back(key, frame);
    }

    std::sort(access_times.begin(), access_times.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    u32 to_evict = static_cast<u32>(access_times.size() * 0.2f);
    to_evict = std::max(1u, to_evict);
    to_evict = std::min(to_evict, static_cast<u32>(access_times.size()));

    for (u32 i = 0; i < to_evict && i < access_times.size(); ++i) {
        u64 key = access_times[i].first;
        
        auto it = cluster_page_info_.find(key);
        if (it != cluster_page_info_.end() && it->second.is_resident) {
            FreePage(it->second.page_index);
            it->second.is_resident = false;
            it->second.page_index = UINT32_MAX;
            cluster_last_access_.erase(key);
            
            stats_.total_clusters_evicted++;
            stats_.eviction_count++;
            current_resident_count_--;
        }
    }

    stats_.current_resident_clusters = current_resident_count_.load();
}

bool NaniteStreamingManager::LoadClusterToGPU(id::id_type geometry_id, u32 cluster_index) {
    if (!resource_manager_) return false;

    u32 page_index = AllocatePage();
    if (page_index == UINT32_MAX) return false;

    u64 key = MakeClusterKey(geometry_id, cluster_index);

    auto& info = cluster_page_info_[key];
    info.page_index = page_index;
    info.is_resident = true;

    current_resident_count_++;
    stats_.current_resident_clusters = current_resident_count_.load();

    UpdateResidency(geometry_id, cluster_index, true);

    return true;
}

void NaniteStreamingManager::UpdateResidency(id::id_type geometry_id, u32 cluster_index, bool is_resident) {
    if (residency_buffer_ == rhi::handles::INVALID_RESOURCE) return;
    
    u64 key = MakeClusterKey(geometry_id, cluster_index);
    
    u8* mapped = static_cast<u8*>(device_->MapBuffer(residency_buffer_, 0, RESIDENCY_BUFFER_SIZE));
    if (mapped) {
        u32 byte_index = static_cast<u32>(key / 8);
        u32 bit_index = static_cast<u32>(key % 8);
        
        if (byte_index < RESIDENCY_BUFFER_SIZE) {
            if (is_resident) {
                mapped[byte_index] |= (1 << bit_index);
            } else {
                mapped[byte_index] &= ~(1 << bit_index);
            }
        }
        
        device_->UnmapBuffer(residency_buffer_);
    }
}

} // namespace primal::graphics::nanite
