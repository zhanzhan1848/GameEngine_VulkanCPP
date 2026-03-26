#include "DepthHistoryManager.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include <iostream>

namespace primal::graphics::nanite {

DepthHistoryManager::~DepthHistoryManager() {
    Shutdown();
}

bool DepthHistoryManager::Initialize(rhi::RHIDeviceBase* device, const Config& config) {
    if (initialized_) {
        std::cerr << "[DepthHistoryManager] Already initialized" << std::endl;
        return false;
    }

    if (!device) {
        std::cerr << "[DepthHistoryManager] Invalid device pointer" << std::endl;
        return false;
    }

    device_ = device;
    config_ = config;

    // Validate configuration
    if (config_.buffer_count < 2 || config_.buffer_count > 4) {
        std::cerr << "[DepthHistoryManager] Invalid buffer count: " << config_.buffer_count
                  << " (must be 2-4)" << std::endl;
        return false;
    }

    std::cout << "[DepthHistoryManager] Initializing..." << std::endl;
    std::cout << "  Resolution: " << config_.width << "x" << config_.height << std::endl;
    std::cout << "  Format: " << static_cast<int>(config_.format) << std::endl;
    std::cout << "  Buffer Count: " << config_.buffer_count << std::endl;

    if (!CreateDepthResources()) {
        std::cerr << "[DepthHistoryManager] Failed to create depth resources" << std::endl;
        return false;
    }

    initialized_ = true;
    std::cout << "[DepthHistoryManager] Initialized successfully" << std::endl;

    return true;
}

void DepthHistoryManager::Shutdown() {
    if (!initialized_) return;

    std::cout << "[DepthHistoryManager] Shutting down..." << std::endl;

    std::lock_guard<std::mutex> lock(mutex_);

    // Cleanup depth buffers
    if (device_) {
        for (auto& depth_buffer : depth_buffers_) {
            if (depth_buffer.texture != rhi::handles::INVALID_RESOURCE) {
                // TODO: Properly destroy texture via RHI
                depth_buffer.texture = rhi::handles::INVALID_RESOURCE;
            }
            depth_buffer.frame_index = 0xFFFFFFFF;
            depth_buffer.is_valid = false;
        }
    }

    initialized_ = false;
    current_write_index_ = 0;
}

bool DepthHistoryManager::CreateDepthResources() {
    std::cout << "[DepthHistoryManager] Creating depth resources..." << std::endl;

    // Create depth textures for each buffer
    for (size_t i = 0; i < config_.buffer_count; ++i) {
        rhi::TextureDesc depthDesc{};
        depthDesc.size = { config_.width, config_.height, 1 };
        depthDesc.format = config_.format;
        depthDesc.type = rhi::TextureType::Texture2D;
        depthDesc.mipLevels = 1; // Single mip level for depth history
        depthDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopyDest;

        depth_buffers_[i].texture = device_->CreateTexture(depthDesc);
        if (depth_buffers_[i].texture == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[DepthHistoryManager] Failed to create depth texture " << i << std::endl;
            return false;
        }

        depth_buffers_[i].frame_index = 0xFFFFFFFF;
        depth_buffers_[i].is_valid = false;

        std::cout << "  Created depth buffer " << i << std::endl;
    }

    return true;
}

bool DepthHistoryManager::StoreCurrentFrameDepth(rhi::ResourceHandle depth_texture,
                                                rhi::RHICommandBuffer* cmd_buffer,
                                                u32 frame_index) {
    if (!initialized_ || !cmd_buffer) {
        std::cerr << "[DepthHistoryManager] Not initialized or invalid command buffer" << std::endl;
        return false;
    }

    if (depth_texture == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[DepthHistoryManager] Invalid source depth texture" << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Determine which buffer to write to (round-robin through buffers)
    u32 write_index = frame_index % config_.buffer_count;

    // Copy current depth to history buffer
    if (!CopyDepthTexture(depth_texture, depth_buffers_[write_index].texture, cmd_buffer)) {
        std::cerr << "[DepthHistoryManager] Failed to copy depth texture to buffer " << write_index << std::endl;
        return false;
    }

    // Update buffer metadata
    depth_buffers_[write_index].frame_index = frame_index;
    depth_buffers_[write_index].is_valid = true;
    current_write_index_ = write_index;

    std::cout << "[DepthHistoryManager] Stored frame " << frame_index
              << " depth in buffer " << write_index << std::endl;

    return true;
}

DepthHistoryManager::DepthBuffer DepthHistoryManager::GetPreviousFrameDepth(u32 frame_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        std::cerr << "[DepthHistoryManager] Not initialized" << std::endl;
        return {};
    }

    // Calculate which buffer contains the previous frame
    // If current frame is N, we want frame N-1
    u32 prev_frame_index = (frame_index == 0) ? 0 : frame_index - 1;
    u32 prev_buffer_index = prev_frame_index % config_.buffer_count;

    const auto& buffer = depth_buffers_[prev_buffer_index];

    // Validate that this buffer actually contains the previous frame
    if (buffer.frame_index != prev_frame_index || !buffer.is_valid) {
        std::cout << "[DepthHistoryManager] Previous frame depth not available (frame "
                  << prev_frame_index << " in buffer " << prev_buffer_index << ")" << std::endl;
        return {}; // Return invalid buffer
    }

    std::cout << "[DepthHistoryManager] Retrieved previous frame " << prev_frame_index
              << " from buffer " << prev_buffer_index << std::endl;

    return buffer;
}

DepthHistoryManager::DepthBuffer DepthHistoryManager::GetCurrentFrameDepth(u32 frame_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        std::cerr << "[DepthHistoryManager] Not initialized" << std::endl;
        return {};
    }

    u32 current_buffer_index = frame_index % config_.buffer_count;
    return depth_buffers_[current_buffer_index];
}

bool DepthHistoryManager::IsPreviousFrameDepthAvailable(u32 frame_index) const {
    auto prev_depth = GetPreviousFrameDepth(frame_index);
    return prev_depth.is_valid && prev_depth.texture != rhi::handles::INVALID_RESOURCE;
}

bool DepthHistoryManager::CopyDepthTexture(rhi::ResourceHandle source,
                                          rhi::ResourceHandle destination,
                                          rhi::RHICommandBuffer* cmd_buffer) {
    if (!device_ || !cmd_buffer) {
        return false;
    }

    // TODO: Implement actual texture copy via RHI
    // This would typically involve:
    // 1. Transition destination texture to copy dest state
    // 2. Issue copy command
    // 3. Insert barrier for synchronization

    // For now, this is a placeholder
    std::cout << "[DepthHistoryManager] Copying depth texture (placeholder)" << std::endl;

    return true;
}

} // namespace primal::graphics::nanite