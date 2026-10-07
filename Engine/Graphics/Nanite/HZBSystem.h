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
     * @brief Build HZB from depth buffer (simplified interface)
     * @param depth_texture Source depth texture (full resolution)
     * @param cmd_buffer Command buffer for GPU operations
     */
    BuildResult BuildHZB(rhi::ResourceHandle depth_texture,
                        rhi::RHICommandBuffer* cmd_buffer,
                        u32 frame_slot = 0);

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
    bool GenerateHZBOnGPU(rhi::RHICommandBuffer* cmd_buffer, rhi::ResourceHandle depth_texture,
                          u32 frame_slot);

    bool CreateHZBTexture();
    bool UpdateHZBMipLevel(u32 mip_level, rhi::RHICommandBuffer* cmd_buffer);

    rhi::RHIDeviceBase* device_{ nullptr };
    Config config_;
    BuildResult last_result_;

    rhi::ResourceHandle hzb_texture_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle last_depth_texture_{ rhi::handles::INVALID_RESOURCE };
    rhi::SamplerHandle hzb_sampler_{ rhi::handles::INVALID_SAMPLER };
    rhi::PipelineHandle hzb_copy_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle hzb_downsample_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle hzb_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle hzb_descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    // Dawn-only: copy stage reads texture_depth_2d (needs SampledDepthImage binding),
    // mip stage reads texture_2d<f32> (needs SampledImage). WGSL can't host both
    // binding types at slot 0 in one module, so we split copy into its own DSL+PL.
    rhi::PipelineLayoutHandle hzb_copy_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle hzb_copy_descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };

    // P0.1 (03047) 修复:mip 派发的持久描述符集池,按帧槽(3)+迭代(≤16 级)
    // 分槽。此前 Vulkan 每帧「创建→录制→销毁」11 个 set,GC 立即回收同 id,
    // 下一帧更新时上一帧 pending 的 command buffer 仍握着旧 native 句柄,
    // MoltenVK 判 set 在用(03047)并作废整条 recording。持久池下每个 set
    // 每帧槽只更新一次,槽间由 BeginFrame 的 fence 等待隔离。
    static constexpr u32 kMipPoolFrames = 3;
    static constexpr u32 kMipPoolPerFrame = 16;
    rhi::DescriptorSetHandle mip_set_pool_[kMipPoolFrames][kMipPoolPerFrame]{};
    // copy 阶段(Dawn/Vulkan)同理:每帧一槽,避免「帧内创建销毁→GC 同 id
    // 回收→旧 pending command buffer 握着新 set」的 03047。
    rhi::DescriptorSetHandle copy_set_pool_[kMipPoolFrames]{};

    u32 mip_levels_{ 0 };
    bool initialized_{ false };
    std::mutex mutex_;
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
