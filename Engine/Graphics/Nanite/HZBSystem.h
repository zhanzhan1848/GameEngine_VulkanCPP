#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"

namespace primal::graphics::rhi {
    class RHIDeviceBase;
    class RHICommandBuffer;
}

namespace primal::graphics::nanite {
    class VisibilityBufferSystem;

/**
 * @brief Hierarchical Z-Buffer System for Nanite
 * @details Implements multi-level depth pyramid for efficient occlusion culling
 * Based on UE5 Nanite's HZB approach with simplified implementation
 */
class HZBSystem {
public:
    /**
     * @brief HZB Configuration
     */
    struct Config {
        u32 max_width;           // Maximum HZB width
        u32 max_height;          // Maximum HZB height
        u32 min_mip_size;        // Minimum mip dimension (8x8 pixels)
        bool enable_compression;  // Enable depth compression
        bool generate_on_gpu;     // Generate HZB on GPU
    };

    /**
     * @brief HZB Build Result
     */
    struct BuildResult {
        rhi::ResourceHandle hzb_texture{ rhi::handles::INVALID_RESOURCE };  // HZB texture with mip chain
        u32 mip_levels{ 0 };               // Number of mip levels generated
        u32 build_time_ms{ 0 };           // Time taken to build HZB
    };

    HZBSystem() = default;
    ~HZBSystem();

    /**
     * @brief Initialize HZB system
     */
    bool Initialize(rhi::RHIDeviceBase* device, const Config& config = Config{});

    /**
     * @brief Shutdown HZB system
     */
    void Shutdown();

    /**
     * @brief Build HZB from depth buffer
     * @param depth_texture Source depth texture (full resolution)
     * @param cmd_buffer Command buffer for GPU operations
     * @param frame_index Current frame index for resource tracking
     */
    BuildResult BuildHZB(rhi::ResourceHandle depth_texture,
                        rhi::RHICommandBuffer* cmd_buffer,
                        u32 frame_index = 0);

    /**
     * @brief Get HZB texture for reading
     */
    rhi::ResourceHandle GetHZBTexture() const { return hzb_texture_; }

    /**
     * @brief Get HZB sampler for reading
     */
    rhi::SamplerHandle GetHZBSampler() const { return hzb_sampler_; }

    /**
     * @brief Get current HZB configuration
     */
    const Config& GetConfig() const { return config_; }

    /**
     * @brief Check if HZB is ready for use
     */
    bool IsReady() const { return initialized_ && hzb_texture_ != rhi::handles::INVALID_RESOURCE; }

    /**
     * @brief Get HZB mip level count
     */
    u32 GetMipLevels() const { return mip_levels_; }

    /**
     * @brief Get HZB dimensions at specific mip level
     */
    math::v2 GetMipDimensions(u32 mip_level) const;

    /**
     * @brief Update HZB configuration (rebuilds resources)
     */
    bool UpdateConfig(const Config& new_config);

private:
    friend class VisibilityBufferSystem;

    bool CreateHZBResources();
    bool CreateHZBComputePipeline();
    bool CreateHZBSampler();

    bool GenerateHZBOnCPU(rhi::ResourceHandle depth_texture);
    bool GenerateHZBOnGPU(rhi::RHICommandBuffer* cmd_buffer, rhi::ResourceHandle depth_texture);

    bool CreateHZBTexture();
    bool UpdateHZBMipLevel(u32 mip_level, rhi::RHICommandBuffer* cmd_buffer);

    rhi::RHIDeviceBase* device_{ nullptr };
    Config config_;
    BuildResult last_result_;

    rhi::ResourceHandle hzb_texture_{ rhi::handles::INVALID_RESOURCE };
    rhi::SamplerHandle hzb_sampler_{ rhi::handles::INVALID_SAMPLER };
    rhi::PipelineHandle hzb_compute_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle hzb_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };

    u32 mip_levels_{ 0 };
    bool initialized_{ false };
    std::mutex mutex_;

    // Resource tracking for frame synchronization
    struct FrameResources {
        rhi::ResourceHandle staging_buffer{ rhi::handles::INVALID_RESOURCE };
        u32 frame_index{ 0 };
        bool in_use{ false };
    };
    std::array<FrameResources, 3> frame_resources_;
    u32 current_frame_resource_{ 0 };
};

/**
 * @brief HZB Occlusion Culling Result
 */
struct HZBOcclusionResult {
    u32 total_objects_tested{ 0 };
    u32 objects_visible{ 0 };
    u32 objects_occluded{ 0 };
    float culling_time_ms{ 0.0f };
};

/**
 * @brief HZB Occlusion Culling Interface
 * @details Provides utilities for performing occlusion culling against HZB
 */
class HZBOcclusionCulling {
public:
    /**
     * @brief Test bounding box against HZB
     * @param bbox World-space bounding box
     * @param view_projection View-projection matrix
     * @param hzb_system HZB system to test against
     * @return True if potentially visible, false if definitely occluded
     */
    static bool TestBoundingBox(const math::v3& bbox_min, const math::v3& bbox_max,
                               const math::m4x4& view_projection,
                               const HZBSystem& hzb_system);

    /**
     * @brief Test sphere against HZB
     * @param sphere_center World-space sphere center
     * @param sphere_radius World-space sphere radius
     * @param view_projection View-projection matrix
     * @param hzb_system HZB system to test against
     * @return True if potentially visible, false if definitely occluded
     */
    static bool TestSphere(const math::v3& sphere_center, float sphere_radius,
                         const math::m4x4& view_projection,
                         const HZBSystem& hzb_system);

    /**
     * @brief Batch test multiple objects against HZB
     * @param bounding_boxes Array of world-space bounding boxes (pairs of min/max)
     * @param view_projection View-projection matrix
     * @param hzb_system HZB system to test against
     * @param out_visibility Output visibility array (same size as input)
     * @return Occlusion culling statistics
     */
    static HZBOcclusionResult TestBatch(const std::vector<std::pair<math::v3, math::v3>>& bounding_boxes,
                                       const math::m4x4& view_projection,
                                       const HZBSystem& hzb_system,
                                       std::vector<bool>& out_visibility);
};

} // namespace primal::graphics::nanite