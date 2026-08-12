#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"
#include "HZBSystem.h"
#include "../Scene/RenderSceneSnapshot.h"
#include "GPUCullingPipeline.h"

namespace primal::graphics::rhi {
    class RHIDeviceBase;
    class RHICommandBuffer;
}

namespace primal::graphics::nanite {

/**
 * @brief Nanite Visibility Buffer System
 * @details Implements efficient visibility buffer rendering for GPU-driven pipeline
 * Based on UE5 Nanite's approach with simplified implementation for real-time rendering
 */
class VisibilityBufferSystem {
public:
    /**
     * @brief Visibility Buffer Configuration
     */
    struct Config {
        u32 width{ 1920 };                    // Visibility buffer width
        u32 height{ 1080 };                   // Visibility buffer height
        rhi::DataFormat format{ rhi::DataFormat::R32_UInt }; // Format for visibility data
        bool enable_depth{ true };            // Enable depth output
        bool enable_conservative{ false };    // Enable conservative rasterization
        bool enable_msaa{ false };            // Enable MSAA (not recommended for Nanite)
    };

    /**
     * @brief Visibility Buffer Entry
     * @details Packed visibility data stored in R32_UINT format
     */
    struct VisibilityEntry {
        u32 triangle_id : 24;    // Triangle primitive ID (0-16M)
        u32 material_id : 8;     // Material ID (0-255)
    };

    /**
     * @brief Visibility Buffer Render Result
     */
    struct RenderResult {
        rhi::ResourceHandle visibility_buffer{ rhi::handles::INVALID_RESOURCE };  // Visibility buffer texture
        rhi::ResourceHandle depth_buffer{ rhi::handles::INVALID_RESOURCE };        // Depth buffer texture
        u32 visible_triangles{ 0 };       // Number of visible triangles
        u32 visible_clusters{ 0 };        // Number of visible clusters
        u32 render_time_ms{ 0 };          // Time taken to render visibility buffer
    };

    VisibilityBufferSystem() = default;
    ~VisibilityBufferSystem();

    /**
     * @brief Initialize visibility buffer system
     */
    bool Initialize(rhi::RHIDeviceBase* device, const Config& config);

    /**
     * @brief Shutdown visibility buffer system
     */
    void Shutdown();

    /**
     * @brief Render visibility buffer
     * @param cmd_buffer Command buffer for rendering
     * @param scene_snapshot Scene data to render
     * @param view_matrix View matrix
     * @param projection_matrix Projection matrix
     * @param culling_results GPU culling results for visible clusters
     * @param frame_index Current frame index
     */
    RenderResult RenderVisibilityBuffer(rhi::RHICommandBuffer* cmd_buffer,
                                       const RenderSceneSnapshot& scene_snapshot,
                                       const math::m4x4& view_matrix,
                                       const math::m4x4& projection_matrix,
                                       const CullingResults& culling_results,
                                       u32 frame_index = 0);

    /**
     * @brief Get visibility buffer texture for reading
     */
    rhi::ResourceHandle GetVisibilityBuffer() const { return visibility_buffer_; }

    /**
     * @brief Get depth buffer texture for reading
     */
    rhi::ResourceHandle GetDepthBuffer() const { return depth_buffer_; }

    /**
     * @brief Get visibility buffer configuration
     */
    const Config& GetConfig() const { return config_; }

    /**
     * @brief Check if visibility buffer is ready
     */
    bool IsReady() const { return initialized_; }

    /**
     * @brief Update visibility buffer configuration (rebuilds resources)
     */
    bool UpdateConfig(const Config& new_config);

    /**
     * @brief Get render pass for visibility buffer rendering
     */
    rhi::RenderPassHandle GetVisibilityRenderPass() const { return visibility_render_pass_; }

    /**
     * @brief Get graphics pipeline for visibility buffer rendering
     */
    rhi::PipelineHandle GetVisibilityPipeline() const { return visibility_pipeline_; }

private:
    bool CreateVisibilityResources();
    bool CreateVisibilityRenderPass();
    bool CreateVisibilityPipeline();
    bool CreateDescriptorSets();

    bool RenderToVisibilityBuffer(rhi::RHICommandBuffer* cmd_buffer,
                                 const RenderSceneSnapshot& scene_snapshot,
                                 const math::m4x4& view_matrix,
                                 const math::m4x4& projection_matrix,
                                 const CullingResults& culling_results);

    rhi::RHIDeviceBase* device_{ nullptr };
    Config config_;
    RenderResult last_result_;

    // Resources
    rhi::ResourceHandle visibility_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle depth_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::RenderPassHandle visibility_render_pass_{ rhi::handles::INVALID_RENDER_PASS };
    rhi::PipelineHandle visibility_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle visibility_pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };

    // Descriptor sets
    rhi::DescriptorSetLayoutHandle descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle descriptor_set_{ rhi::handles::INVALID_DESCRIPTOR_SET };

    bool initialized_{ false };
    std::mutex mutex_;
};

/**
 * @brief Visibility Buffer Reader
 * @details Utilities for reading and processing visibility buffer data
 */
class VisibilityBufferReader {
public:
    /**
     * @brief Resolve visibility buffer to final render target
     * @param visibility_buffer Visibility buffer texture
     * @param depth_buffer Depth buffer texture
     * @param output_texture Output render target
     * @param cmd_buffer Command buffer for rendering
     * @param scene_snapshot Scene data for material lookup
     */
    static bool ResolveVisibilityBuffer(rhi::ResourceHandle visibility_buffer,
                                      rhi::ResourceHandle depth_buffer,
                                      rhi::ResourceHandle output_texture,
                                      rhi::RHICommandBuffer* cmd_buffer,
                                      const RenderSceneSnapshot& scene_snapshot);

    /**
     * @brief Visibility Statistics
     */
    struct VisibilityStats {
        u32 total_pixels{ 0 };
        u32 visible_pixels{ 0 };
        u32 occluded_pixels{ 0 };
        float average_overdraw{ 0.0f };
    };

    /**
     * @brief Extract visibility statistics from visibility buffer
     */
    static VisibilityStats ExtractStats(rhi::ResourceHandle visibility_buffer,
                                      rhi::RHIDeviceBase* device);
};

} // namespace primal::graphics::nanite