#include "ColorHistoryManager.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include <iostream>

namespace primal::graphics::nanite {

ColorHistoryManager::~ColorHistoryManager() {
    Shutdown();
}

bool ColorHistoryManager::Initialize(rhi::RHIDeviceBase* device, const Config& config) {
    if (initialized_) {
        std::cerr << "[ColorHistoryManager] Already initialized" << std::endl;
        return false;
    }

    if (!device) {
        std::cerr << "[ColorHistoryManager] Invalid device pointer" << std::endl;
        return false;
    }

    device_ = device;
    config_ = config;

    // Validate configuration
    if (config_.buffer_count < 2 || config_.buffer_count > 4) {
        std::cerr << "[ColorHistoryManager] Invalid buffer count: " << config_.buffer_count
                  << " (must be 2-4)" << std::endl;
        return false;
    }

//    std::cout << "[ColorHistoryManager] Initializing..." << std::endl;
//    std::cout << "  Resolution: " << config_.width << "x" << config_.height << std::endl;
//    std::cout << "  Format: " << static_cast<int>(config_.format) << std::endl;
//    std::cout << "  Buffer Count: " << config_.buffer_count << std::endl;

    if (!CreateColorResources()) {
        std::cerr << "[ColorHistoryManager] Failed to create color resources" << std::endl;
        return false;
    }

    initialized_ = true;
//    std::cout << "[ColorHistoryManager] Initialized successfully" << std::endl;

    return true;
}

void ColorHistoryManager::Shutdown() {
    if (!initialized_) return;

//    std::cout << "[ColorHistoryManager] Shutting down..." << std::endl;

    std::lock_guard<std::mutex> lock(mutex_);

    // Cleanup color buffers
    if (device_) {
        for (auto& color_buffer : color_buffers_) {
            if (color_buffer.texture != rhi::handles::INVALID_RESOURCE) {
                // TODO: Properly destroy texture via RHI
                color_buffer.texture = rhi::handles::INVALID_RESOURCE;
            }
            color_buffer.frame_index = 0xFFFFFFFF;
            color_buffer.is_valid = false;
        }
    }

    initialized_ = false;
    current_write_index_ = 0;
}

bool ColorHistoryManager::CreateColorResources() {
//    std::cout << "[ColorHistoryManager] Creating color resources..." << std::endl;

    // Create color textures for each buffer
    for (size_t i = 0; i < config_.buffer_count; ++i) {
        rhi::TextureDesc colorDesc{};
        colorDesc.size = { config_.width, config_.height, 1 };
        colorDesc.format = config_.format;
        colorDesc.type = rhi::TextureType::Texture2D;
        colorDesc.mipLevels = 1; // Single mip level for color history
        colorDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopyDest;

        color_buffers_[i].texture = device_->CreateTexture(colorDesc);
        if (color_buffers_[i].texture == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[ColorHistoryManager] Failed to create color texture " << i << std::endl;
            return false;
        }

        color_buffers_[i].frame_index = 0xFFFFFFFF;
        color_buffers_[i].is_valid = false;

//        std::cout << "  Created color buffer " << i << std::endl;
    }

    return true;
}

bool ColorHistoryManager::StoreCurrentFrameColor(rhi::ResourceHandle color_texture,
                                                rhi::RHICommandBuffer* cmd_buffer,
                                                u32 frame_index) {
    if (!initialized_ || !cmd_buffer) {
        std::cerr << "[ColorHistoryManager] Not initialized or invalid command buffer" << std::endl;
        return false;
    }

    if (color_texture == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[ColorHistoryManager] Invalid source color texture" << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Determine which buffer to write to (round-robin through buffers)
    u32 write_index = frame_index % config_.buffer_count;

    // Copy current color to history buffer
    if (!CopyColorTexture(color_texture, color_buffers_[write_index].texture, cmd_buffer)) {
        std::cerr << "[ColorHistoryManager] Failed to copy color texture to buffer " << write_index << std::endl;
        return false;
    }

    // Update buffer metadata
    color_buffers_[write_index].frame_index = frame_index;
    color_buffers_[write_index].is_valid = true;
    current_write_index_ = write_index;

    return true;
}

ColorHistoryManager::ColorBuffer ColorHistoryManager::GetPreviousFrameColor(u32 frame_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        std::cerr << "[ColorHistoryManager] Not initialized" << std::endl;
        return {};
    }

    // CRITICAL: Use 2-frame delay for triple-buffer safety.
    // In a triple-buffered GPU pipeline, Frame N-1's StoreCurrentFrameColor
    // GPU command may still be in-flight when Frame N reads it.
    // Reading from Frame N-2 guarantees the GPU copy has completed
    // (the buffer is no longer being written to by any in-flight frame).
    u32 prev_frame_index = (frame_index >= 2) ? frame_index - 2 : 0;
    u32 prev_buffer_index = prev_frame_index % config_.buffer_count;

    const auto& buffer = color_buffers_[prev_buffer_index];

    // Validate that this buffer actually contains the target frame
    if (buffer.frame_index != prev_frame_index || !buffer.is_valid) {
        // Fallback: try 1-frame delay if 2-frame data isn't available yet (warmup)
        if (frame_index >= 1) {
            u32 fallback_index = (frame_index - 1) % config_.buffer_count;
            const auto& fallback_buffer = color_buffers_[fallback_index];
            if (fallback_buffer.is_valid) {
                return fallback_buffer;
            }
        }
        return {};
    }

    return buffer;
}

ColorHistoryManager::ColorBuffer ColorHistoryManager::GetCurrentFrameColor(u32 frame_index) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        std::cerr << "[ColorHistoryManager] Not initialized" << std::endl;
        return {};
    }

    u32 current_buffer_index = frame_index % config_.buffer_count;
    return color_buffers_[current_buffer_index];
}

bool ColorHistoryManager::IsPreviousFrameColorAvailable(u32 frame_index) const {
    auto prev_color = GetPreviousFrameColor(frame_index);
    return prev_color.is_valid && prev_color.texture != rhi::handles::INVALID_RESOURCE;
}

bool ColorHistoryManager::CopyColorTexture(rhi::ResourceHandle source,
                                          rhi::ResourceHandle destination,
                                          rhi::RHICommandBuffer* cmd_buffer) {
    if (!device_ || !cmd_buffer) {
        return false;
    }

    if (source == rhi::handles::INVALID_RESOURCE || destination == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[ColorHistoryManager] CopyColorTexture: invalid texture handles" << std::endl;
        return false;
    }

    // Insert barrier: source -> CopySource, destination -> CopyDest
    // CRITICAL: Source texture was used as RenderTarget in the previous render pass,
    // so beforeState must be RenderTarget (not ShaderResource).
    rhi::ResourceBarrier barriers[2]{};
    barriers[0].resource = source;
    barriers[0].beforeState = rhi::ResourceState::RenderTarget;
    barriers[0].afterState = rhi::ResourceState::CopySource;
    barriers[0].subresource = 0xFFFFFFFF;
    barriers[0].queueFamily = 0xFFFFFFFF;

    barriers[1].resource = destination;
    barriers[1].beforeState = rhi::ResourceState::Unknown;
    barriers[1].afterState = rhi::ResourceState::CopyDest;
    barriers[1].subresource = 0xFFFFFFFF;
    barriers[1].queueFamily = 0xFFFFFFFF;

    cmd_buffer->InsertBarrier(barriers, 2);

    // Blit copy from source to destination
    rhi::TextureBlitRegion region{};
    region.srcSubresource = {0, 0, 1};
    region.srcOffsets[0] = {0, 0, 0};
    region.srcOffsets[1] = {static_cast<s32>(config_.width), static_cast<s32>(config_.height), 1};
    region.dstSubresource = {0, 0, 1};
    region.dstOffsets[0] = {0, 0, 0};
    region.dstOffsets[1] = {static_cast<s32>(config_.width), static_cast<s32>(config_.height), 1};

//    std::cout << "[ColorHistoryManager] CopyColorTexture: src=" << source
//              << " dst=" << destination
//              << " size=" << config_.width << "x" << config_.height << std::endl;

    cmd_buffer->BlitTexture(source, destination, &region, 1, rhi::FilterMode::Nearest);

    return true;
}

} // namespace primal::graphics::nanite
