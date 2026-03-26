#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"
#include <array>
#include <mutex>

namespace primal::graphics::rhi {
    class RHIDeviceBase;
    class RHICommandBuffer;
}

namespace primal::graphics::nanite {

/**
 * @brief Depth History Manager for Triple-Buffered Depth Management
 * @details Manages three frame buffers for depth history to support temporal HZB occlusion culling
 *
 * Architecture:
 * - Frame N: Current frame rendering (writes to current depth)
 * - Frame N-1: Previous frame depth (used for HZB testing)
 * - Frame N-2: Two frames ago (being prepared for next cycle)
 *
 * This triple-buffering ensures that we always have valid previous frame depth data
 * while avoiding synchronization issues between rendering and HZB generation.
 */
class DepthHistoryManager {
public:
    /**
     * @brief Configuration for depth history management
     */
    struct Config {
        u32 width;           // Depth buffer width
        u32 height;          // Depth buffer height
        rhi::DataFormat format;  // Depth format (default: R32_Float for HZB compatibility)
        u32 buffer_count;    // Number of buffers (default: 3 for triple buffering)

        Config() : width(1920), height(1080), format(rhi::DataFormat::R32_Float), buffer_count(3) {}
    };

    /**
     * @brief Depth buffer handle with frame metadata
     */
    struct DepthBuffer {
        rhi::ResourceHandle texture{ rhi::handles::INVALID_RESOURCE };
        u32 frame_index{ 0xFFFFFFFF };     // Frame this buffer represents
        bool is_valid{ false };            // Whether data is valid
    };

    DepthHistoryManager() = default;
    ~DepthHistoryManager();

    /**
     * @brief Initialize the depth history manager
     */
    bool Initialize(rhi::RHIDeviceBase* device, const Config& config);

    /**
     * @brief Shutdown and cleanup resources
     */
    void Shutdown();

    /**
     * @brief Store current frame depth buffer
     * @param depth_texture Current frame depth texture
     * @param cmd_buffer Command buffer for copy operations
     * @param frame_index Current frame index
     */
    bool StoreCurrentFrameDepth(rhi::ResourceHandle depth_texture,
                                rhi::RHICommandBuffer* cmd_buffer,
                                u32 frame_index);

    /**
     * @brief Get previous frame depth buffer for HZB testing
     * @param frame_index Current frame index (used to determine which buffer is "previous")
     */
    DepthBuffer GetPreviousFrameDepth(u32 frame_index) const;

    /**
     * @brief Get current frame depth buffer (for debugging)
     */
    DepthBuffer GetCurrentFrameDepth(u32 frame_index) const;

    /**
     * @brief Check if previous frame depth is available
     */
    bool IsPreviousFrameDepthAvailable(u32 frame_index) const;

    /**
     * @brief Get current configuration
     */
    const Config& GetConfig() const { return config_; }

    /**
     * @brief Check if manager is initialized
     */
    bool IsInitialized() const { return initialized_; }

    /**
     * @brief Get buffer count
     */
    u32 GetBufferCount() const { return static_cast<u32>(depth_buffers_.size()); }

private:
    bool CreateDepthResources();
    bool CopyDepthTexture(rhi::ResourceHandle source, rhi::ResourceHandle destination,
                         rhi::RHICommandBuffer* cmd_buffer);

    rhi::RHIDeviceBase* device_{ nullptr };
    Config config_;
    std::array<DepthBuffer, 3> depth_buffers_;  // Triple buffers
    u32 current_write_index_{ 0 };
    bool initialized_{ false };
    mutable std::mutex mutex_;  // Thread safety for multi-threaded access
};

} // namespace primal::graphics::nanite