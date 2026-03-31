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
 * @brief Color History Manager for Triple-Buffered Scene Color Management
 * @details Manages three frame buffers for scene color history to support temporal GI effects
 *          (SSGI ray hit sampling, temporal accumulation)
 *
 * Architecture:
 * - Frame N: Current frame rendering (writes to current color)
 * - Frame N-1: Previous frame color (used for temporal sampling)
 * - Frame N-2: Two frames ago (being prepared for next cycle)
 *
 * This triple-buffering ensures that we always have valid previous frame color data
 * while avoiding synchronization issues between rendering and temporal effects.
 */
class ColorHistoryManager {
public:
    /**
     * @brief Configuration for color history management
     */
    struct Config {
        u32 width;           // Color buffer width
        u32 height;          // Color buffer height
        rhi::DataFormat format;  // Color format (default: RGBA16_Float for HDR color)
        u32 buffer_count;    // Number of buffers (default: 3 for triple buffering)

        Config() : width(1920), height(1080), format(rhi::DataFormat::RGBA16_Float), buffer_count(3) {}
    };

    /**
     * @brief Color buffer handle with frame metadata
     */
    struct ColorBuffer {
        rhi::ResourceHandle texture{ rhi::handles::INVALID_RESOURCE };
        u32 frame_index{ 0xFFFFFFFF };     // Frame this buffer represents
        bool is_valid{ false };            // Whether data is valid
    };

    ColorHistoryManager() = default;
    ~ColorHistoryManager();

    /**
     * @brief Initialize the color history manager
     */
    bool Initialize(rhi::RHIDeviceBase* device, const Config& config);

    /**
     * @brief Shutdown and cleanup resources
     */
    void Shutdown();

    /**
     * @brief Store current frame color buffer
     * @param color_texture Current frame color texture
     * @param cmd_buffer Command buffer for copy operations
     * @param frame_index Current frame index
     */
    bool StoreCurrentFrameColor(rhi::ResourceHandle color_texture,
                                rhi::RHICommandBuffer* cmd_buffer,
                                u32 frame_index);

    /**
     * @brief Get previous frame color buffer for temporal sampling
     * @param frame_index Current frame index (used to determine which buffer is "previous")
     */
    ColorBuffer GetPreviousFrameColor(u32 frame_index) const;

    /**
     * @brief Get current frame color buffer (for debugging)
     */
    ColorBuffer GetCurrentFrameColor(u32 frame_index) const;

    /**
     * @brief Check if previous frame color is available
     */
    bool IsPreviousFrameColorAvailable(u32 frame_index) const;

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
    u32 GetBufferCount() const { return static_cast<u32>(color_buffers_.size()); }

private:
    bool CreateColorResources();
    bool CopyColorTexture(rhi::ResourceHandle source, rhi::ResourceHandle destination,
                         rhi::RHICommandBuffer* cmd_buffer);

    rhi::RHIDeviceBase* device_{ nullptr };
    Config config_;
    std::array<ColorBuffer, 3> color_buffers_;  // Triple buffers
    u32 current_write_index_{ 0 };
    bool initialized_{ false };
    mutable std::mutex mutex_;  // Thread safety for multi-threaded access
};

} // namespace primal::graphics::nanite
