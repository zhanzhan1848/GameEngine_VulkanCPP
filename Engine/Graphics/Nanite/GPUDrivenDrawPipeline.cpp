#include "GPUDrivenDrawPipeline.h"
#include "GPUCullingPipeline.h"
#include "NaniteResourceManager.h"
#include "HZBSystem.h"
#include "VisibilityBufferSystem.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include "../RHI/Core/RHIMath.h"
#include "../RHI/Core/RHIGpuMesh.h"
#include "../Scene/RenderSceneSnapshot.h"
#include "../../Content/ContentToEngine.h" // Needed for get_rhi_mesh_asset
#include "CommonHeaders.h"
#include <cassert>
#include <fstream>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <set>

namespace primal::graphics::nanite {

namespace {
    std::vector<u8> LoadShaderBytecode(const char* shaderName, const char* entryPoint) {
        std::string shaderPath = std::string("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/shaders/") + shaderName + ".metal";

        std::ifstream file(shaderPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            shaderPath = std::string("EngineTest/shaders/") + shaderName + ".metal";
            file.open(shaderPath, std::ios::binary | std::ios::ate);
        }

        if (!file.is_open()) {
            std::cerr << "Failed to load shader: " << shaderName << " (entry point: " << entryPoint << ")" << std::endl;
            return {};
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<u8> bytecode(size);
        if (!file.read(reinterpret_cast<char*>(bytecode.data()), size)) {
            std::cerr << "Failed to read shader file: " << shaderPath << std::endl;
            return {};
        }
        /*
        std::cout << "[GPUDrivenDrawPipeline] Loaded shader: " << shaderName
                  << " (" << size << " bytes)" << std::endl;
        */
        return bytecode;
    }
} // anonymous namespace

GPUDrivenDrawPipeline& GPUDrivenDrawPipeline::Get() {
    static GPUDrivenDrawPipeline instance;
    return instance;
}

GPUDrivenDrawPipeline::~GPUDrivenDrawPipeline() {
    Shutdown();
}

bool GPUDrivenDrawPipeline::Initialize(rhi::RHIDeviceBase* device,
                                     const BinningConfig& binning_config,
                                     const VisibilityBufferConfig& visibility_config) {
    if (initialized_) {
        std::cerr << "[GPUDrivenDrawPipeline] Already initialized" << std::endl;
        return false;
    }

    device_ = device;
    binning_config_ = binning_config;
    visibility_config_ = visibility_config;

    // std::cout << "[GPUDrivenDrawPipeline] Initializing..." << std::endl;
    // std::cout << "  Bin Size: " << binning_config_.bin_size << " pixels" << std::endl;
    // std::cout << "  Max Bins: " << binning_config_.max_bins_per_frame << std::endl;
    // std::cout << "  Visibility Buffer: " << visibility_config_.width << "x" << visibility_config_.height << std::endl;

    if (!CreateResources()) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create resources" << std::endl;
        return false;
    }

    if (!CreateRenderPasses()) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create render passes" << std::endl;
        return false;
    }

    if (!CreatePipelines()) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create pipelines" << std::endl;
        return false;
    }

    if (!CreateDescriptorSets()) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create descriptor sets" << std::endl;
        return false;
    }

    initialized_ = true;
    // std::cout << "[GPUDrivenDrawPipeline] Initialized successfully" << std::endl;
    return true;
}

void GPUDrivenDrawPipeline::Shutdown() {
    if (!initialized_) return;

    // std::cout << "[GPUDrivenDrawPipeline] Shutting down..." << std::endl;

    // Cleanup resources
    if (device_) {
        // Destroy Triple Buffered Resources
        for (auto& res : frame_resources_) {
            if (res.camera_constants_buffer != rhi::handles::INVALID_RESOURCE) {
                device_->DestroyBuffer(res.camera_constants_buffer);
                res.camera_constants_buffer = rhi::handles::INVALID_RESOURCE;
            }
            if (res.global_draw_descriptor_set != rhi::handles::INVALID_DESCRIPTOR_SET) {
                device_->DestroyDescriptorSet(res.global_draw_descriptor_set);
                res.global_draw_descriptor_set = rhi::handles::INVALID_DESCRIPTOR_SET;
            }
        }
        frame_resources_.clear();

        // Destroy single buffers
        if (bin_data_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(bin_data_buffer_);
        if (bin_counter_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(bin_counter_buffer_);
        if (visibility_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyTexture(visibility_buffer_);
        if (indirect_draw_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(indirect_draw_buffer_);
        
        // Destroy global geometry buffers
        if (global_meshlet_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(global_meshlet_buffer_);
        if (global_meshlet_vertices_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(global_meshlet_vertices_buffer_);
        if (global_meshlet_triangles_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(global_meshlet_triangles_buffer_);
        if (global_vertex_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(global_vertex_buffer_);
        if (cluster_map_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(cluster_map_buffer_);
        if (global_instance_data_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(global_instance_data_buffer_);
        
        // Destroy test geometry buffers
        if (vertex_position_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(vertex_position_buffer_);
        if (vertex_normal_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(vertex_normal_buffer_);
        if (vertex_uv_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(vertex_uv_buffer_);
        if (index_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(index_buffer_);
        
        // Destroy render passes and textures
        if (visibility_render_pass_ != rhi::handles::INVALID_RENDER_PASS) device_->DestroyRenderPass(visibility_render_pass_);
        if (final_render_pass_ != rhi::handles::INVALID_RENDER_PASS) device_->DestroyRenderPass(final_render_pass_);
        if (final_color_texture_ != rhi::handles::INVALID_RESOURCE) device_->DestroyTexture(final_color_texture_);
        if (final_depth_texture_ != rhi::handles::INVALID_RESOURCE) device_->DestroyTexture(final_depth_texture_);
    }

    initialized_ = false;
}

bool GPUDrivenDrawPipeline::CreateResources() {
    // std::cout << "[GPUDrivenDrawPipeline] Creating resources..." << std::endl;

    // Create bin data buffer
    rhi::BufferDesc binBufferDesc{};
    binBufferDesc.size = binning_config_.max_bins_per_frame * sizeof(u32) * binning_config_.max_clusters_per_bin;
    binBufferDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::Indirect);
    binBufferDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;

    bin_data_buffer_ = device_->CreateBuffer(binBufferDesc);
    if (bin_data_buffer_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create bin data buffer" << std::endl;
        return false;
    }

    // Create bin counter buffer
    rhi::BufferDesc binCounterDesc{};
    binCounterDesc.size = sizeof(u32) * binning_config_.max_bins_per_frame;
    binCounterDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::TransferDst);
    binCounterDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;

    bin_counter_buffer_ = device_->CreateBuffer(binCounterDesc);
    if (bin_counter_buffer_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create bin counter buffer" << std::endl;
        return false;
    }

    // Create visibility buffer
    rhi::TextureDesc visibilityDesc{};
    visibilityDesc.size = { visibility_config_.width, visibility_config_.height, 1 };
    visibilityDesc.format = visibility_config_.format;
    visibilityDesc.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;

    visibility_buffer_ = device_->CreateTexture(visibilityDesc);
    if (visibility_buffer_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create visibility buffer" << std::endl;
        return false;
    }

    // Create indirect draw buffer
    rhi::BufferDesc indirectDesc{};
    indirectDesc.size = sizeof(u32) * 5 * binning_config_.max_bins_per_frame; // 5 params per draw
    indirectDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Indirect | rhi::BufferUsageFlags::Storage);
    indirectDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;

    indirect_draw_buffer_ = device_->CreateBuffer(indirectDesc);
    if (indirect_draw_buffer_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create indirect draw buffer" << std::endl;
        return false;
    }

    // Create camera constants buffer (Triple Buffered)
    frame_resources_.resize(3);
    for (int i = 0; i < 3; ++i) {
        rhi::BufferDesc constantsDesc{};
        constantsDesc.size = 256; // sizeof(DrawConstants)
        constantsDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Uniform | rhi::BufferUsageFlags::TransferDst);
        constantsDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic; // Updated every frame

        frame_resources_[i].camera_constants_buffer = device_->CreateBuffer(constantsDesc);
        if (frame_resources_[i].camera_constants_buffer == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[GPUDrivenDrawPipeline] Failed to create camera constants buffer for frame " << i << std::endl;
            return false;
        }
        
        // Create Descriptor Set for this frame
        // Note: Descriptor Layout must be created before this!
        // We will do this in CreateDescriptorSets() instead
    }

    // std::cout << "[GPUDrivenDrawPipeline] Resources created successfully" << std::endl;
    return true;
}

bool GPUDrivenDrawPipeline::CreateRenderPasses() {
    // std::cout << "[GPUDrivenDrawPipeline] Creating render passes..." << std::endl;

    // Create Visibility Buffer Render Pass
    rhi::RenderPassDesc visibilityPassDesc{};

    // Color attachment - visibility buffer (R32_UINT format)
    rhi::RenderPassDesc::Attachment visibilityColorAttachment{};
    visibilityColorAttachment.texture = visibility_buffer_;
    visibilityColorAttachment.format = rhi::DataFormat::R32_UInt;
    visibilityColorAttachment.loadOp = rhi::LoadAction::Clear;
    visibilityColorAttachment.storeOp = rhi::StoreAction::Store;
    visibilityColorAttachment.clearValue.color = math::v4{0.0f, 0.0f, 0.0f, 0.0f}; // Clear to 0 (no visible geometry)

    visibilityPassDesc.colorAttachments.push_back(visibilityColorAttachment);

    // Depth attachment
    rhi::RenderPassDesc::Attachment depthAttachment{};
    depthAttachment.format = rhi::DataFormat::D32_Float;
    depthAttachment.loadOp = rhi::LoadAction::Clear;
    depthAttachment.storeOp = rhi::StoreAction::Store;
    depthAttachment.clearValue.depth = 1.0f;
    depthAttachment.clearValue.stencil = 0;

    visibilityPassDesc.depthAttachment = depthAttachment;

    // Set viewport to match visibility buffer size
    visibilityPassDesc.viewport.size.x = static_cast<float>(visibility_config_.width);
    visibilityPassDesc.viewport.size.y = static_cast<float>(visibility_config_.height);
    visibilityPassDesc.scissor.extent.x = visibility_config_.width;
    visibilityPassDesc.scissor.extent.y = visibility_config_.height;

    visibility_render_pass_ = device_->CreateRenderPass(visibilityPassDesc);
    if (visibility_render_pass_ == rhi::handles::INVALID_RENDER_PASS) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create visibility render pass" << std::endl;
        return false;
    }

    // std::cout << "[GPUDrivenDrawPipeline] Visibility render pass created successfully" << std::endl;

    // Create Final Render Pass (for Stage 3)
    // First, create a color texture as render target
    rhi::TextureDesc finalColorDesc{};
    finalColorDesc.size = { visibility_config_.width, visibility_config_.height, 1 };
    finalColorDesc.format = rhi::DataFormat::BGRA8_UNorm; // Match swapchain format for Metal blit compatibility
    finalColorDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;

    final_color_texture_ = device_->CreateTexture(finalColorDesc);
    if (final_color_texture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create final color texture" << std::endl;
        return false;
    }

    // Create depth texture for final render pass
    rhi::TextureDesc finalDepthDesc{};
    finalDepthDesc.size = { visibility_config_.width, visibility_config_.height, 1 };
    finalDepthDesc.format = rhi::DataFormat::D32_Float;
    // CRITICAL: Add ShaderResource usage to allow depth texture to be sampled by HZB generation shader
    finalDepthDesc.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;

    final_depth_texture_ = device_->CreateTexture(finalDepthDesc);
    if (final_depth_texture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create final depth texture" << std::endl;
        return false;
    }

    rhi::RenderPassDesc finalPassDesc{};

    // Color attachment - use the created texture
    rhi::RenderPassDesc::Attachment finalColorAttachment{};
    finalColorAttachment.texture = final_color_texture_;
    finalColorAttachment.format = rhi::DataFormat::BGRA8_UNorm; // Match texture and swapchain format
    finalColorAttachment.loadOp = rhi::LoadAction::Clear;
    finalColorAttachment.storeOp = rhi::StoreAction::Store;
    finalColorAttachment.clearValue.color = math::v4{0.0f, 0.0f, 0.0f, 1.0f};

    finalPassDesc.colorAttachments.push_back(finalColorAttachment);

    // Depth attachment - use the created depth texture
    rhi::RenderPassDesc::Attachment finalDepthAttachment{};
    finalDepthAttachment.texture = final_depth_texture_;
    finalDepthAttachment.format = rhi::DataFormat::D32_Float;
    finalDepthAttachment.loadOp = rhi::LoadAction::Clear;
    finalDepthAttachment.storeOp = rhi::StoreAction::Store;
    finalDepthAttachment.clearValue.depth = 1.0f;

    finalPassDesc.depthAttachment = finalDepthAttachment;

    // Set viewport to match screen size
    finalPassDesc.viewport.size.x = static_cast<float>(visibility_config_.width);
    finalPassDesc.viewport.size.y = static_cast<float>(visibility_config_.height);
    finalPassDesc.scissor.extent.x = visibility_config_.width;
    finalPassDesc.scissor.extent.y = visibility_config_.height;

    final_render_pass_ = device_->CreateRenderPass(finalPassDesc);
    if (final_render_pass_ == rhi::handles::INVALID_RENDER_PASS) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create final render pass" << std::endl;
        return false;
    }

    // std::cout << "[GPUDrivenDrawPipeline] Final render pass created successfully" << std::endl;

    return true;
}

bool GPUDrivenDrawPipeline::CreatePipelines() {
    // std::cout << "[GPUDrivenDrawPipeline] Creating pipelines..." << std::endl;

    // TEMPORARILY DISABLE minimal test pipeline to test fixed complex pipeline
    // std::cout << "[GPUDrivenDrawPipeline] Skipping minimal test pipeline - testing fixed complex pipeline" << std::endl;
    /*
    // FIRST: Create a minimal test pipeline with absolutely no dependencies
    std::cout << "[GPUDrivenDrawPipeline] Creating minimal test pipeline..." << std::endl;
    auto minimalVertexShaderCode = LoadShaderBytecode("MinimalTest", "minimal_test_vertex");
    auto minimalFragmentShaderCode = LoadShaderBytecode("MinimalTest", "minimal_test_fragment");

    if (!minimalVertexShaderCode.empty() && !minimalFragmentShaderCode.empty()) {
        std::cout << "[GPUDrivenDrawPipeline] Minimal test shaders loaded successfully" << std::endl;

        rhi::ShaderHandle minimalVS = device_->CreateShader(
            minimalVertexShaderCode.data(),
            minimalVertexShaderCode.size(),
            rhi::ShaderStage::Vertex,
            "minimal_test_vertex"
        );
        rhi::ShaderHandle minimalFS = device_->CreateShader(
            minimalFragmentShaderCode.data(),
            minimalFragmentShaderCode.size(),
            rhi::ShaderStage::Pixel,
            "minimal_test_fragment"
        );

        if (minimalVS != rhi::handles::INVALID_SHADER && minimalFS != rhi::handles::INVALID_SHADER) {
            std::cout << "[GPUDrivenDrawPipeline] Minimal test shaders created successfully" << std::endl;

            // Create minimal pipeline layout (no descriptor sets needed!)
            rhi::PipelineLayoutDesc minimalLayoutDesc{};
            minimalLayoutDesc.setLayoutCount = 0;
            minimalLayoutDesc.setLayouts = nullptr;

            rhi::PipelineLayoutHandle minimalLayout = device_->CreatePipelineLayout(minimalLayoutDesc);
            if (minimalLayout != rhi::handles::INVALID_PIPELINE_LAYOUT) {
                std::cout << "[GPUDrivenDrawPipeline] Minimal pipeline layout created" << std::endl;

                // Create minimal graphics pipeline
                rhi::GraphicsPipelineDesc minimalPipelineDesc{};
                minimalPipelineDesc.vertexShader = minimalVS;
                minimalPipelineDesc.pixelShader = minimalFS;
                minimalPipelineDesc.layout = minimalLayout;

                // Render target format - match swapchain format
                minimalPipelineDesc.renderTargetCount = 1;
                minimalPipelineDesc.renderTargetFormats[0] = rhi::DataFormat::RGBA8_UNorm; // Try different format
                minimalPipelineDesc.renderTargetFormats[0] = rhi::DataFormat::BGRA8_UNorm; // Common swapchain format

                // No depth testing for minimal test
                minimalPipelineDesc.enableDepthTest = false;
                minimalPipelineDesc.enableDepthWrite = false;

                // No blending for clearer test
                minimalPipelineDesc.enableBlend = false;

                // Create the minimal pipeline
                rhi::PipelineHandle minimalPipeline = device_->CreateGraphicsPipeline(minimalPipelineDesc);
                if (minimalPipeline != rhi::handles::INVALID_PIPELINE) {
                    std::cout << "[GPUDrivenDrawPipeline] MINIMAL TEST PIPELINE CREATED SUCCESSFULLY!" << std::endl;
                    // Replace the complex draw pipeline with our minimal test pipeline
                    draw_pipeline_ = minimalPipeline;
                    draw_pipeline_layout_ = minimalLayout;
                } else {
                    std::cerr << "[GPUDrivenDrawPipeline] Failed to create minimal test pipeline" << std::endl;
                }
            } else {
                std::cerr << "[GPUDrivenDrawPipeline] Failed to create minimal pipeline layout" << std::endl;
            }
        } else {
            std::cerr << "[GPUDrivenDrawPipeline] Failed to create minimal test shaders" << std::endl;
        }
    } else {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to load minimal test shaders" << std::endl;
    }
    */

    // Load Cluster Binning shader
    auto binningShaderCode = LoadShaderBytecode("ClusterBinning", "cluster_binning_kernel");
    if (!binningShaderCode.empty()) {
        rhi::ShaderHandle binningShader = device_->CreateShader(
            binningShaderCode.data(),
            binningShaderCode.size(),
            rhi::ShaderStage::Compute,
            "cluster_binning_kernel"
        );

        if (binningShader != rhi::handles::INVALID_SHADER) {
            rhi::ComputePipelineDesc binningPipelineDesc{};
            binningPipelineDesc.computeShader = binningShader;
            binningPipelineDesc.threadGroupSize = {64, 1, 1}; // Adjust based on cluster count

            binning_pipeline_ = device_->CreateComputePipeline(binningPipelineDesc);
            if (binning_pipeline_ != rhi::handles::INVALID_PIPELINE) {
                // std::cout << "[GPUDrivenDrawPipeline] Cluster Binning pipeline created successfully" << std::endl;
            } else {
                std::cerr << "[GPUDrivenDrawPipeline] Failed to create Cluster Binning pipeline" << std::endl;
            }
        }
    } else {
        // std::cout << "[GPUDrivenDrawPipeline] Cluster Binning shader not found, using CPU fallback" << std::endl;
    }

    // Load Visibility Buffer shaders and create graphics pipeline
    auto visibilityVertexShaderCode = LoadShaderBytecode("VisibilityBuffer", "visibility_vertex_shader");
    auto visibilityFragmentShaderCode = LoadShaderBytecode("VisibilityBuffer", "visibility_fragment_shader");

    if (!visibilityVertexShaderCode.empty() && !visibilityFragmentShaderCode.empty()) {
        rhi::ShaderHandle visibilityVS = device_->CreateShader(
            visibilityVertexShaderCode.data(),
            visibilityVertexShaderCode.size(),
            rhi::ShaderStage::Vertex,
            "visibility_vertex_shader"
        );

        rhi::ShaderHandle visibilityFS = device_->CreateShader(
            visibilityFragmentShaderCode.data(),
            visibilityFragmentShaderCode.size(),
            rhi::ShaderStage::Pixel,
            "visibility_fragment_shader"
        );

        if (visibilityVS != rhi::handles::INVALID_SHADER && visibilityFS != rhi::handles::INVALID_SHADER) {
            // Create pipeline layout for visibility buffer
            rhi::PipelineLayoutDesc visibilityLayoutDesc{};
            // TODO: Add descriptor set layouts for cluster data, vertex buffers, etc.
            visibility_pipeline_layout_ = device_->CreatePipelineLayout(visibilityLayoutDesc);

            if (visibility_pipeline_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
                rhi::GraphicsPipelineDesc visibilityPipelineDesc{};
                visibilityPipelineDesc.vertexShader = visibilityVS;
                visibilityPipelineDesc.pixelShader = visibilityFS;
                visibilityPipelineDesc.layout = visibility_pipeline_layout_;

                // Visibility buffer format: R32_UINT (packed cluster_id + primitive_id)
                visibilityPipelineDesc.renderTargetCount = 1;
                visibilityPipelineDesc.renderTargetFormats[0] = rhi::DataFormat::R32_UInt;

                // Depth settings for conservative rasterization
                visibilityPipelineDesc.depthStencilFormat = rhi::DataFormat::D32_Float;
                visibilityPipelineDesc.enableDepthTest = true;
                visibilityPipelineDesc.enableDepthWrite = true;
                visibilityPipelineDesc.depthFunc = rhi::ComparisonFunc::Less;

                // No blending for visibility buffer
                visibilityPipelineDesc.enableBlend = false;

                // Conservative rasterization is not directly supported in current RHI
                // Would be implemented via depth bias or other techniques

                visibility_pipeline_ = device_->CreateGraphicsPipeline(visibilityPipelineDesc);
                if (visibility_pipeline_ != rhi::handles::INVALID_PIPELINE) {
                    // std::cout << "[GPUDrivenDrawPipeline] Visibility Buffer pipeline created successfully" << std::endl;
                } else {
                    std::cerr << "[GPUDrivenDrawPipeline] Failed to create Visibility Buffer pipeline" << std::endl;
                }
            }
        }
    } else {
        // std::cout << "[GPUDrivenDrawPipeline] Visibility Buffer shaders not found" << std::endl;
    }
    
    // Load GPU Driven Draw shaders for the main rendering pipeline
    auto gpuDrawVertexShaderCode = LoadShaderBytecode("GPUDrivenDraw", "gpu_driven_vertex_shader");
    auto gpuDrawFragmentShaderCode = LoadShaderBytecode("GPUDrivenDraw", "gpu_driven_fragment_shader");

    if (!gpuDrawVertexShaderCode.empty() && !gpuDrawFragmentShaderCode.empty()) {
        // std::cout << "[GPUDrivenDrawPipeline] Loaded GPU Draw shaders (" << gpuDrawVertexShaderCode.size()
        //           << " VS bytes, " << gpuDrawFragmentShaderCode.size() << " FS bytes)" << std::endl;

        rhi::ShaderHandle gpuDrawVS = device_->CreateShader(
            gpuDrawVertexShaderCode.data(),
            gpuDrawVertexShaderCode.size(),
            rhi::ShaderStage::Vertex,
            "gpu_driven_vertex_shader"
        );
        rhi::ShaderHandle gpuDrawFS = device_->CreateShader(
            gpuDrawFragmentShaderCode.data(),
            gpuDrawFragmentShaderCode.size(),
            rhi::ShaderStage::Pixel,
            "gpu_driven_fragment_shader"
        );

        // std::cout << "[GPUDrivenDrawPipeline] VS handle: " << gpuDrawVS << ", FS handle: " << gpuDrawFS << std::endl;

        if (gpuDrawVS != rhi::handles::INVALID_SHADER && gpuDrawFS != rhi::handles::INVALID_SHADER) {
            // Create GPU Draw descriptor layout
            rhi::DescriptorSetLayoutBinding gpuDrawBindings[8];
            gpuDrawBindings[0].binding = 0;
            gpuDrawBindings[0].descriptorType = rhi::DescriptorType::UniformBufferDynamic;
            gpuDrawBindings[0].descriptorCount = 1;
            gpuDrawBindings[0].stageFlags = rhi::ShaderStage::Vertex;

            gpuDrawBindings[1].binding = 1;
            gpuDrawBindings[1].descriptorType = rhi::DescriptorType::StorageBuffer;
            gpuDrawBindings[1].descriptorCount = 1;
            gpuDrawBindings[1].stageFlags = rhi::ShaderStage::Vertex;

            gpuDrawBindings[2].binding = 2;
            gpuDrawBindings[2].descriptorType = rhi::DescriptorType::StorageBuffer;
            gpuDrawBindings[2].descriptorCount = 1;
            gpuDrawBindings[2].stageFlags = rhi::ShaderStage::Vertex;

            gpuDrawBindings[3].binding = 3;
            gpuDrawBindings[3].descriptorType = rhi::DescriptorType::StorageBuffer;
            gpuDrawBindings[3].descriptorCount = 1;
            gpuDrawBindings[3].stageFlags = rhi::ShaderStage::Vertex;

            gpuDrawBindings[4].binding = 4;
            gpuDrawBindings[4].descriptorType = rhi::DescriptorType::StorageBuffer;
            gpuDrawBindings[4].descriptorCount = 1;
            gpuDrawBindings[4].stageFlags = rhi::ShaderStage::Vertex;

            gpuDrawBindings[5].binding = 5;
            gpuDrawBindings[5].descriptorType = rhi::DescriptorType::StorageBuffer;
            gpuDrawBindings[5].descriptorCount = 1;
            gpuDrawBindings[5].stageFlags = rhi::ShaderStage::Vertex;

            gpuDrawBindings[6].binding = 6;
            gpuDrawBindings[6].descriptorType = rhi::DescriptorType::StorageBuffer;
            gpuDrawBindings[6].descriptorCount = 1;
            gpuDrawBindings[6].stageFlags = rhi::ShaderStage::Vertex;

            gpuDrawBindings[7].binding = 7;
            gpuDrawBindings[7].descriptorType = rhi::DescriptorType::StorageBuffer;
            gpuDrawBindings[7].descriptorCount = 1;
            gpuDrawBindings[7].stageFlags = rhi::ShaderStage::Vertex;

            rhi::DescriptorSetLayoutDesc gpuDrawLayoutDesc{};
            gpuDrawLayoutDesc.bindingCount = 8;
            gpuDrawLayoutDesc.bindings = gpuDrawBindings;

            draw_descriptor_layout_ = device_->CreateDescriptorSetLayout(gpuDrawLayoutDesc);
            if (draw_descriptor_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
                std::cerr << "[GPUDrivenDrawPipeline] Failed to create GPU Draw descriptor layout" << std::endl;
            } else {
                // std::cout << "[GPUDrivenDrawPipeline] GPU Draw descriptor layout created successfully" << std::endl;
            }

            rhi::PipelineLayoutDesc gpuDrawPipelineLayoutDesc{};
            gpuDrawPipelineLayoutDesc.setLayoutCount = 1;
            gpuDrawPipelineLayoutDesc.setLayouts = &draw_descriptor_layout_;

            draw_pipeline_layout_ = device_->CreatePipelineLayout(gpuDrawPipelineLayoutDesc);
            if (draw_pipeline_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) {
                std::cerr << "[GPUDrivenDrawPipeline] Failed to create GPU Draw pipeline layout" << std::endl;
            } else {
                // std::cout << "[GPUDrivenDrawPipeline] Pipeline layout created successfully" << std::endl;
            }

            // Create the main GPU Draw graphics pipeline with proper depth testing
            rhi::GraphicsPipelineDesc gpuDrawPipelineDesc{};
            gpuDrawPipelineDesc.vertexShader = gpuDrawVS;
            gpuDrawPipelineDesc.pixelShader = gpuDrawFS;
            gpuDrawPipelineDesc.layout = draw_pipeline_layout_;

            // Render target format - final color buffer
            gpuDrawPipelineDesc.renderTargetCount = 1;
            gpuDrawPipelineDesc.renderTargetFormats[0] = rhi::DataFormat::BGRA8_UNorm; // Match render pass and swapchain format

            // DEPTH SETTINGS - CRITICAL FOR PROPER RENDERING
            // Based on UE5 Nanite depth rendering practices
            // Relaxed depth testing to ensure geometry visibility
            gpuDrawPipelineDesc.depthStencilFormat = rhi::DataFormat::D32_Float;
            gpuDrawPipelineDesc.enableDepthTest = true;
            gpuDrawPipelineDesc.enableDepthWrite = true;
            gpuDrawPipelineDesc.depthFunc = rhi::ComparisonFunc::Less;  // Use Less (strict) to prevent Z-fighting

            // No blending for opaque geometry
            gpuDrawPipelineDesc.enableBlend = false;

            draw_pipeline_ = device_->CreateGraphicsPipeline(gpuDrawPipelineDesc);
            if (draw_pipeline_ != rhi::handles::INVALID_PIPELINE) {
                // std::cout << "[GPUDrivenDrawPipeline] GPU Driven Draw pipeline created successfully!" << std::endl;
                // std::cout << "[GPUDrivenDrawPipeline] Pipeline will render meshlets with proper depth testing" << std::endl;
            } else {
                std::cerr << "[GPUDrivenDrawPipeline] Failed to create GPU Driven Draw pipeline" << std::endl;
            }
        } else {
            std::cerr << "[GPUDrivenDrawPipeline] Failed to create GPU Draw shaders" << std::endl;
        }
    } else {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to load GPU Draw shaders" << std::endl;
    }

    auto resolveShaderCode = LoadShaderBytecode("VisibilityBufferResolve", "ComputeMain");

    if (!resolveShaderCode.empty()) {
        // std::cout << "[GPUDrivenDrawPipeline] VisibilityBufferResolve shader not found, using fallback direct rendering" << std::endl;
        return true;
    }
    
    // std::cout << "[GPUDrivenDrawPipeline] Creating Visibility Buffer Resolve compute pipeline..." << std::endl;
    
    rhi::DescriptorSetLayoutBinding resolveBindings[4];
    resolveBindings[0].binding = 0;
    resolveBindings[0].descriptorType = rhi::DescriptorType::SampledImage;
    resolveBindings[0].descriptorCount = 1;
    resolveBindings[0].stageFlags = rhi::ShaderStage::Compute;
    resolveBindings[1].binding = 1;
    resolveBindings[1].descriptorType = rhi::DescriptorType::SampledImage;
    resolveBindings[1].descriptorCount = 1;
    resolveBindings[1].stageFlags = rhi::ShaderStage::Compute;
    resolveBindings[2].binding = 2;
    resolveBindings[2].descriptorType = rhi::DescriptorType::SampledImage;
    resolveBindings[2].descriptorCount = 1;
    resolveBindings[2].stageFlags = rhi::ShaderStage::Compute;
    resolveBindings[3].binding = 3;
    resolveBindings[3].descriptorType = rhi::DescriptorType::StorageBuffer;
    resolveBindings[3].descriptorCount = 1;
    resolveBindings[3].stageFlags = rhi::ShaderStage::Compute;
    
    rhi::DescriptorSetLayoutDesc resolveLayoutDesc{};
    resolveLayoutDesc.bindingCount = 4;
    resolveLayoutDesc.bindings = resolveBindings;
    resolve_descriptor_layout_ = device_->CreateDescriptorSetLayout(resolveLayoutDesc);
    if (resolve_descriptor_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create resolve descriptor layout" << std::endl;
        return true;
    }
    
    rhi::PipelineLayoutDesc resolvePipelineLayoutDesc{};
    resolvePipelineLayoutDesc.setLayoutCount = 1;
    resolvePipelineLayoutDesc.setLayouts = &resolve_descriptor_layout_;
    resolve_pipeline_layout_ = device_->CreatePipelineLayout(resolvePipelineLayoutDesc);
    if (resolve_pipeline_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create resolve pipeline layout" << std::endl;
        return true;
    }
    
    rhi::ShaderHandle resolveShader = device_->CreateShader(
        resolveShaderCode.data(),
        resolveShaderCode.size(),
        rhi::ShaderStage::Compute,
        "ComputeMain"
    );
    if (resolveShader == rhi::handles::INVALID_SHADER) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create resolve shader" << std::endl;
        return true;
    }
    
    rhi::ComputePipelineDesc resolvePipelineDesc{};
    resolvePipelineDesc.layout = resolve_pipeline_layout_;
    resolvePipelineDesc.computeShader = resolveShader;
    resolvePipelineDesc.threadGroupSize = {8, 8, 1};
    resolve_pipeline_ = device_->CreateComputePipeline(resolvePipelineDesc);
    if (resolve_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create resolve compute pipeline" << std::endl;
        return true;
    }
    
    // std::cout << "[GPUDrivenDrawPipeline] Visibility Buffer Resolve pipeline created successfully!" << std::endl;
    
    rhi::SamplerDesc resolveSamplerDesc{};
    resolveSamplerDesc.minFilter = rhi::FilterMode::Nearest;
    resolveSamplerDesc.magFilter = rhi::FilterMode::Nearest;
    resolveSamplerDesc.addressU = rhi::TextureAddressMode::Clamp;
    resolveSamplerDesc.addressV = rhi::TextureAddressMode::Clamp;
    resolve_sampler_ = device_->CreateSampler(resolveSamplerDesc);
    
    rhi::TextureDesc resolveOutputDesc{};
    resolveOutputDesc.size = { visibility_config_.width, visibility_config_.height, 1 };
    resolveOutputDesc.format = rhi::DataFormat::RGBA8_UNorm;
    resolveOutputDesc.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
    resolve_output_texture_ = device_->CreateTexture(resolveOutputDesc);
    
    resolve_descriptor_set_ = device_->CreateDescriptorSet({resolve_descriptor_layout_});
    
    // std::cout << "[GPUDrivenDrawPipeline] Visibility Buffer Resolve resources created successfully!" << std::endl;
    
    return true;
}

bool GPUDrivenDrawPipeline::CreateDescriptorSets() {
    // std::cout << "[GPUDrivenDrawPipeline] Creating descriptor sets..." << std::endl;

    // Create descriptor set layouts for Visibility Buffer pipeline
    if (visibility_pipeline_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
        // Create bindings array
        static rhi::DescriptorSetLayoutBinding visibilityBindings[4];

        // Binding 0: Cluster ID (uniform buffer)
        visibilityBindings[0].binding = 0;
        visibilityBindings[0].descriptorType = rhi::DescriptorType::UniformBuffer;
        visibilityBindings[0].descriptorCount = 1;
        visibilityBindings[0].stageFlags = rhi::ShaderStage::Vertex | rhi::ShaderStage::Pixel;

        // Binding 1: Vertex positions (storage buffer)
        visibilityBindings[1].binding = 1;
        visibilityBindings[1].descriptorType = rhi::DescriptorType::StorageBuffer;
        visibilityBindings[1].descriptorCount = 1;
        visibilityBindings[1].stageFlags = rhi::ShaderStage::Vertex;

        // Binding 2: Vertex normals (storage buffer)
        visibilityBindings[2].binding = 2;
        visibilityBindings[2].descriptorType = rhi::DescriptorType::StorageBuffer;
        visibilityBindings[2].descriptorCount = 1;
        visibilityBindings[2].stageFlags = rhi::ShaderStage::Vertex;

        // Binding 3: Vertex UVs (storage buffer)
        visibilityBindings[3].binding = 3;
        visibilityBindings[3].descriptorType = rhi::DescriptorType::StorageBuffer;
        visibilityBindings[3].descriptorCount = 1;
        visibilityBindings[3].stageFlags = rhi::ShaderStage::Vertex;

        // Create the layout
        rhi::DescriptorSetLayoutDesc visibilityLayoutDesc{};
        visibilityLayoutDesc.bindingCount = 4;
        visibilityLayoutDesc.bindings = visibilityBindings;

        global_descriptor_layout_ = device_->CreateDescriptorSetLayout(visibilityLayoutDesc);
        if (global_descriptor_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            // std::cout << "[GPUDrivenDrawPipeline] Visibility descriptor layout created successfully" << std::endl;
        }

        // TODO: Create descriptor pool and allocate descriptor sets
        // std::cout << "[GPUDrivenDrawPipeline] Descriptor layout creation - PLACEHOLDER for set allocation" << std::endl;
    }

    // Create descriptor set layouts for GPU Draw pipeline
    if (draw_pipeline_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
        // std::cout << "[GPUDrivenDrawPipeline] GPU Draw descriptor layout already created in CreatePipelines" << std::endl;
        
        // Allocate 3 descriptor sets for triple buffering
        for (u32 i = 0; i < 3; ++i) {
            if (i < frame_resources_.size()) {
                frame_resources_[i].global_draw_descriptor_set = device_->CreateDescriptorSet({draw_descriptor_layout_});
                if (frame_resources_[i].global_draw_descriptor_set == rhi::handles::INVALID_DESCRIPTOR_SET) {
                    std::cerr << "[GPUDrivenDrawPipeline] Failed to create descriptor set for frame " << i << std::endl;
                    return false;
                }
            }
        }
    }

    return true;
}

void GPUDrivenDrawPipeline::UpdateGeometryData(const RenderSceneSnapshot& scene_snapshot) {
    if (total_meshlet_count_ > 0) {
        // Already built global buffers
        return;
    }

    // std::cout << "[GPUDrivenDrawPipeline] Building Global Geometry Buffers..." << std::endl;

    const auto& instanceData = scene_snapshot.GetInstanceData();
    std::set<id::id_type> processedGeometries;

    // 1. Calculate total sizes
    struct PackedV3 { float x, y, z; };

    u32 totalMeshlets = 0;
    u32 totalVertices = 0;
    u32 totalTriangles = 0; // total meshlet triangle indices (bytes or count?) - count
    u32 totalPositions = 0;

    // Use packed float3 for positions to match Metal's packed_float3 and typical disk format

    for (const auto& instance : instanceData) {
        if (processedGeometries.count(instance.geometry_id)) continue;
        
        graphics::rhi::RHIMeshAsset meshAsset;
        if (primal::content::get_rhi_mesh_asset(instance.geometry_id, meshAsset)) {
            totalMeshlets += (u32)meshAsset.meshlets.size();
            totalVertices += (u32)meshAsset.meshlet_vertices.size();
            totalTriangles += (u32)meshAsset.meshlet_triangles.size();
            
            // RHIMeshAsset stores positions as bytes
            // Assuming the asset data is tightly packed float3 (12 bytes)
            totalPositions += (u32)(meshAsset.position_buffer.size() / sizeof(PackedV3));
            
            processedGeometries.insert(instance.geometry_id);
        }
    }

    if (totalMeshlets == 0) {
        // std::cout << "[GPUDrivenDrawPipeline] No meshlets found!" << std::endl;
        return;
    }

    // std::cout << "[GPUDrivenDrawPipeline] Total Stats:" << std::endl;
    // std::cout << "  Meshlets: " << totalMeshlets << std::endl;
    // std::cout << "  Vertices: " << totalVertices << std::endl;
    // std::cout << "  Triangles: " << totalTriangles << std::endl;
    // std::cout << "  Positions: " << totalPositions << std::endl;

    // 2. Allocate Global Buffers
    // Meshlets
    rhi::BufferDesc meshletDesc{};
    meshletDesc.size = totalMeshlets * sizeof(rhi::RHIMeshlet);
    meshletDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage);
    meshletDesc.usage = rhi::GPUMemoryUsage::Dynamic; // Changed to Dynamic
    global_meshlet_buffer_ = device_->CreateBuffer(meshletDesc);

    // Meshlet Vertices (Global Vertex Indices)
    rhi::BufferDesc meshletVerticesDesc{};
    meshletVerticesDesc.size = totalVertices * sizeof(u32);
    meshletVerticesDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage);
    meshletVerticesDesc.usage = rhi::GPUMemoryUsage::Dynamic; // Changed to Dynamic
    global_meshlet_vertices_buffer_ = device_->CreateBuffer(meshletVerticesDesc);

    // Meshlet Triangles (Local Indices)
    rhi::BufferDesc meshletTrianglesDesc{};
    meshletTrianglesDesc.size = totalTriangles * sizeof(u8); // u8 per index
    meshletTrianglesDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage);
    meshletTrianglesDesc.usage = rhi::GPUMemoryUsage::Dynamic; // Changed to Dynamic
    global_meshlet_triangles_buffer_ = device_->CreateBuffer(meshletTrianglesDesc);

    // Global Positions
    // Use packed float3 (12 bytes) to match Metal's packed_float3
    rhi::BufferDesc positionDesc{};
    positionDesc.size = totalPositions * sizeof(PackedV3);
    positionDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage); // Vertex pulling
    positionDesc.usage = rhi::GPUMemoryUsage::Dynamic; // Changed to Dynamic
    global_vertex_buffer_ = device_->CreateBuffer(positionDesc);

    // 3. Fill Data
    // We need staging buffers or map/unmap. Since these are Static, we might need staging.
    // For simplicity in this test, let's assume we can create staging buffers, fill them, and copy.
    // Or just use CreateBuffer with initial data if supported (RHI abstract might hide it).
    // Here we'll allocate CPU memory, fill it, and use a helper to upload.

    utl::vector<rhi::RHIMeshlet> mergedMeshlets;
    mergedMeshlets.reserve(totalMeshlets);

    utl::vector<u32> mergedVertices;
    mergedVertices.reserve(totalVertices);

    utl::vector<u8> mergedTriangles;
    mergedTriangles.reserve(totalTriangles);

    utl::vector<PackedV3> mergedPositions;
    mergedPositions.reserve(totalPositions);

    u32 currentMeshletOffset = 0;
    u32 currentVertexOffset = 0;   // Offset in global_meshlet_vertices_buffer
    u32 currentTriangleOffset = 0; // Offset in global_meshlet_triangles_buffer
    u32 currentPositionOffset = 0; // Offset in global_vertex_buffer

    processedGeometries.clear();

    // Re-iterate to fill data (order must match iteration in Culling Pipeline?)
    // Culling Pipeline iterates instances. Instances point to GeometryID.
    // We need a map: GeometryID -> GlobalMeshletStartIndex
    // But wait, InstanceData has `cluster_start`. This is set by `SceneSnapshot`.
    // We need to ensure `SceneSnapshot`'s `cluster_start` matches our packing order.
    // `SceneSnapshot` assigns `cluster_start` sequentially based on instance order.
    // BUT we are packing based on Unique Geometry.
    // This is a mismatch!
    
    // CORRECTION:
    // If we pack by Unique Geometry, then multiple instances sharing the same geometry 
    // will share the same meshlet data.
    // The `cluster_start` in `InstanceData` is currently:
    //   instance[0].cluster_start = 0
    //   instance[1].cluster_start = instance[0].cluster_count
    // This implies `cluster_start` is a unique range for THAT INSTANCE.
    // If we use Instanced Drawing where InstanceID -> ClusterID, we need to map:
    //   InstanceID -> [GlobalClusterStart, GlobalClusterEnd]
    // 
    // In `GPUCullingPipeline`, we output `visible_cluster_indices`.
    // These indices are in the range [0, TotalClustersInScene].
    // "TotalClustersInScene" assumes we duplicated clusters for each instance?
    // Let's check `RenderSceneSnapshot::Initialize`.
    
    // If `RenderSceneSnapshot` treats clusters as unique per instance (which it seems to do),
    // then our Global Buffer should also duplicate meshlets for each instance? 
    // NO, that defeats the purpose of instancing.
    
    // GPU-Driven Rendering typically works by:
    // 1. Culling produces a list of visible (MeshletID, InstanceID) pairs.
    // 2. OR Culling produces a list of visible ClusterIDs, where ClusterID encodes (MeshID, LocalClusterID).
    
    // In our case, `GPUCullingPipeline` outputs `visible_cluster_indices`.
    // These indices correspond to the flattened list of all clusters in all instances.
    // So if Instance 0 has 10 clusters, and Instance 1 has 10 clusters (same mesh):
    //   Cluster 0-9 -> Instance 0
    //   Cluster 10-19 -> Instance 1
    
    // So, `GlobalMeshletBuffer` should store UNIQUE Meshlets (per geometry).
    // And we need a way to map FlattenedClusterID -> (GeometryID, LocalClusterID, InstanceID).
    
    // BUT `GPUCullingPipeline` logic:
    //   results_.visible_cluster_indices.push_back(instance.cluster_start + c);
    
    // If we want to use `DrawIndirect(vertexCount, instanceCount, ...)`,
    // and `instanceCount` = `visible_cluster_count`.
    // Vertex Shader runs `visible_cluster_count` times.
    // `instanceID` (0..visible_count) -> `ClusterID` (from compacted buffer).
    
    // Now, what does `ClusterID` represent?
    // It represents `instance.cluster_start + local_cluster_index`.
    // This `ClusterID` is unique for every cluster of every instance.
    
    // We need to map `ClusterID` -> `MeshletData`.
    // If we duplicate MeshletData for every instance, memory usage explodes.
    // We should map `ClusterID` -> `(GlobalMeshletIndex, InstanceIndex)`.
    
    // Since we don't have a buffer for that mapping yet, we can create one!
    // `GlobalClusterMapBuffer`:
    //    struct ClusterMap { uint globalMeshletIndex; uint instanceIndex; };
    //    Buffer[TotalClustersInScene]
    
    // Then Shader:
    //    uint flatClusterID = compact_cluster_ids[instanceID];
    //    ClusterMap map = cluster_map[flatClusterID];
    //    Meshlet meshlet = global_meshlets[map.globalMeshletIndex];
    //    // transform using map.instanceIndex
    
    // So we need:
    // 1. `GlobalMeshlets` (Unique)
    // 2. `ClusterMap` (Per Instance Cluster)
    
    // Let's build `GlobalMeshlets` (Unique) first.
    std::unordered_map<id::id_type, u32> geometryToGlobalMeshletBase;
    
    for (const auto& instance : instanceData) {
        if (processedGeometries.count(instance.geometry_id)) continue;
        
        graphics::rhi::RHIMeshAsset meshAsset;
        if (primal::content::get_rhi_mesh_asset(instance.geometry_id, meshAsset)) {
            
            geometryToGlobalMeshletBase[instance.geometry_id] = currentMeshletOffset;
            
            // Copy Positions
            // Assume input is packed float3 (12 bytes)
            u32 posCount = (u32)(meshAsset.position_buffer.size() / sizeof(PackedV3));
            const PackedV3* posSrc = reinterpret_cast<const PackedV3*>(meshAsset.position_buffer.data());
            for(u32 i=0; i<posCount; ++i) mergedPositions.push_back(posSrc[i]);
            
            // Copy MeshletVertices (Global Vertex Indices)
            // Need to offset by currentPositionOffset
            for(u32 idx : meshAsset.meshlet_vertices) {
                mergedVertices.push_back(idx + currentPositionOffset);
            }
            
            // Copy MeshletTriangles (Local Indices)
            for(u8 idx : meshAsset.meshlet_triangles) {
                mergedTriangles.push_back(idx);
            }
            
            // Copy Meshlets
            for(const auto& m : meshAsset.meshlets) {
                rhi::RHIMeshlet newM = m;
                // Fix offsets
                newM.vertex_offset += currentVertexOffset;
                newM.triangle_offset += currentTriangleOffset;
                mergedMeshlets.push_back(newM);
            }
            
            currentMeshletOffset += (u32)meshAsset.meshlets.size();
            currentVertexOffset += (u32)meshAsset.meshlet_vertices.size();
            currentTriangleOffset += (u32)meshAsset.meshlet_triangles.size();
            currentPositionOffset += posCount;
            
            processedGeometries.insert(instance.geometry_id);
        }
    }
    
    // Upload Unique Geometry Data
    auto UploadBuffer = [&](rhi::ResourceHandle buffer, const void* data, u64 size) {
        // Simple map/unmap or staging
        // Assuming we can map for now (Static might fail on some backends, but Metal usually allows managed)
        // If not, we need a staging buffer helper. 
        // Let's assume device_->MapBuffer works or we'll fix it.
        // Actually, RHIDevice usually requires Staging for Static buffers.
        // We'll skip implementation details and assume a helper `UpdateBuffer` exists or map works.
        // For this test, let's try mapping.
        void* ptr = device_->MapBuffer(buffer);
        if (ptr) {
            memcpy(ptr, data, size);
            device_->UnmapBuffer(buffer);
        } else {
             std::cerr << "Failed to map buffer for upload!" << std::endl;
        }
    };
    
    UploadBuffer(global_meshlet_buffer_, mergedMeshlets.data(), mergedMeshlets.size() * sizeof(rhi::RHIMeshlet));
    UploadBuffer(global_meshlet_vertices_buffer_, mergedVertices.data(), mergedVertices.size() * sizeof(u32));
    UploadBuffer(global_meshlet_triangles_buffer_, mergedTriangles.data(), mergedTriangles.size() * sizeof(u8));
    UploadBuffer(global_vertex_buffer_, mergedPositions.data(), mergedPositions.size() * sizeof(PackedV3));

    // 🔥 NEW: Check meshlet data quality for backface culling and cluster-level frustum culling
    std::cout << "[GPUDrivenDrawPipeline] Meshlet Data Quality Check:" << std::endl;
    std::cout << "  Total Meshlets: " << mergedMeshlets.size() << std::endl;

    u32 valid_cone_cutoff = 0;
    u32 invalid_cone_cutoff = 0;
    u32 valid_cone_axis = 0;
    u32 invalid_cone_axis = 0;
    u32 negative_cutoff = 0;
    u32 nan_cutoff = 0;
    u32 zero_axis_count = 0;
    u32 non_zero_axis_count = 0;

    // 🔥 NEW: Check meshlet bounds data for cluster-level frustum culling
    u32 valid_center_count = 0;
    u32 invalid_center_count = 0;
    u32 valid_radius_count = 0;
    u32 invalid_radius_count = 0;
    u32 zero_center_count = 0;
    u32 zero_radius_count = 0;

    for (size_t i = 0; i < mergedMeshlets.size(); ++i) {
        const auto& meshlet = mergedMeshlets[i];

        // Check cone_cutoff validity (should be in range [0, 1])
        if (std::isfinite(meshlet.cone_cutoff)) {
            if (meshlet.cone_cutoff >= 0.0f && meshlet.cone_cutoff <= 1.0f) {
                valid_cone_cutoff++;
            } else {
                invalid_cone_cutoff++;
                if (meshlet.cone_cutoff < 0.0f) {
                    negative_cutoff++;
                }
            }
        } else {
            nan_cutoff++;
        }

        // Check cone_axis validity (should be normalized vector)
        float axis_length = std::sqrt(meshlet.cone_axis[0] * meshlet.cone_axis[0] +
                                      meshlet.cone_axis[1] * meshlet.cone_axis[1] +
                                      meshlet.cone_axis[2] * meshlet.cone_axis[2]);

        if (std::isfinite(axis_length) && axis_length > 0.001f) {
            valid_cone_axis++;
            non_zero_axis_count++;
        } else {
            invalid_cone_axis++;
            zero_axis_count++;
        }

        // 🔥 NEW: Check meshlet bounds for cluster-level frustum culling
        bool valid_center = (meshlet.center[0] != 0 || meshlet.center[1] != 0 || meshlet.center[2] != 0);
        bool valid_radius = (meshlet.radius > 0.001f && std::isfinite(meshlet.radius));

        if (valid_center) {
            valid_center_count++;
        } else {
            invalid_center_count++;
            if (meshlet.center[0] == 0 && meshlet.center[1] == 0 && meshlet.center[2] == 0) {
                zero_center_count++;
            }
        }

        if (valid_radius) {
            valid_radius_count++;
        } else {
            invalid_radius_count++;
            if (meshlet.radius <= 0.001f) {
                zero_radius_count++;
            }
        }

        // 🔥 NEW: Print EVERY meshlet with bounds info for complete analysis
        if (i < 100) { // Print first 100 to avoid log spam
            std::cout << "  Meshlet #" << i << ": "
                      << "cone_cutoff=" << meshlet.cone_cutoff << ", "
                      << "cone_axis=[" << meshlet.cone_axis[0] << "," << meshlet.cone_axis[1] << "," << meshlet.cone_axis[2] << "], "
                      << "center=[" << meshlet.center[0] << "," << meshlet.center[1] << "," << meshlet.center[2] << "], "
                      << "radius=" << meshlet.radius;

            bool has_issues = (axis_length < 0.001f || !std::isfinite(axis_length) ||
                              !valid_center || !valid_radius);
            if (has_issues) {
                std::cout << " ❌ ISSUES:";
                if (axis_length < 0.001f) std::cout << " zero_axis";
                if (!valid_center) std::cout << " invalid_center";
                if (!valid_radius) std::cout << " invalid_radius";
            }
            std::cout << std::endl;
        }
    }

    std::cout << "\n  Cone Cutoff Quality: " << valid_cone_cutoff << " valid, " << invalid_cone_cutoff << " invalid" << std::endl;
    std::cout << "    - Negative cutoffs: " << negative_cutoff << std::endl;
    std::cout << "    - NaN cutoffs: " << nan_cutoff << std::endl;
    std::cout << "  Cone Axis Quality: " << valid_cone_axis << " valid, " << invalid_cone_axis << " invalid" << std::endl;
    std::cout << "    - Zero axis vectors: " << zero_axis_count << " (40% issue)" << std::endl;
    std::cout << "    - Non-zero axis vectors: " << non_zero_axis_count << std::endl;

    // 🔥 NEW: Meshlet Bounds Quality Summary
    std::cout << "\n  Meshlet Bounds Quality:" << std::endl;
    std::cout << "    Valid Centers: " << valid_center_count << " / " << mergedMeshlets.size() << std::endl;
    std::cout << "    Invalid Centers: " << invalid_center_count << " (including " << zero_center_count << " zero centers)" << std::endl;
    std::cout << "    Valid Radii: " << valid_radius_count << " / " << mergedMeshlets.size() << std::endl;
    std::cout << "    Invalid Radii: " << invalid_radius_count << " (including " << zero_radius_count << " zero radii)" << std::endl;

    // 🔥 NEW: Cluster-level frustum culling capability assessment
    bool cluster_frustum_capable = (valid_center_count > (mergedMeshlets.size() * 0.9)) &&
                                    (valid_radius_count > (mergedMeshlets.size() * 0.9));
    std::cout << "\n  Cluster-Level Frustum Culling Capability: " << (cluster_frustum_capable ? "✅ READY" : "❌ NOT READY") << std::endl;
    if (cluster_frustum_capable) {
        std::cout << "    → " << valid_center_count << " meshlets have valid bounds for precise cluster frustum culling" << std::endl;
    } else {
        std::cout << "    → Only " << valid_center_count << "/" << mergedMeshlets.size() << " meshlets have valid bounds" << std::endl;
        std::cout << "    → Cluster-level frustum culling would be too aggressive" << std::endl;
    }

    // 🔥 NEW: Instance-Cluster Bounds Consistency Check
    std::cout << "\n  Instance-Cluster Bounds Consistency Check:" << std::endl;
    const auto& instanceDataForCheck = scene_snapshot.GetInstanceData();
    u32 instances_to_check = std::min((u32)instanceDataForCheck.size(), (u32)10);

    for (u32 i = 0; i < instances_to_check; i++) {
        const auto& instance = instanceDataForCheck[i];
        if (instance.cluster_count == 0) continue;

        std::cout << "  Instance #" << i << ":" << std::endl;
        std::cout << "    Instance Bounds: center=[" << instance.bounds_center.x << ", "
                  << instance.bounds_center.y << ", " << instance.bounds_center.z << "], "
                  << "radius=" << instance.bounds_radius << std::endl;

        // Check first few clusters of this instance by using cluster_start directly
        u32 clusters_to_check = std::min(instance.cluster_count, (u32)5);
        std::cout << "    Cluster bounds (first " << clusters_to_check << "):" << std::endl;

        for (u32 j = 0; j < clusters_to_check; j++) {
            // Directly use cluster_start + j as meshlet_id (based on current mapping)
            u32 meshlet_id = instance.cluster_start + j;

            // Get meshlet bounds from global meshlet array
            if (meshlet_id < mergedMeshlets.size()) {
                const auto& meshlet = mergedMeshlets[meshlet_id];

                // Transform local meshlet center to world space
                math::v3 meshlet_local_center{ meshlet.center[0], meshlet.center[1], meshlet.center[2] };

                // Use SIMD matrix multiplication
                simd_float4 local_center_4 = {meshlet_local_center.x, meshlet_local_center.y, meshlet_local_center.z, 1.0f};
                simd_float4 world_center_4 = simd_mul(instance.world_matrix, local_center_4);
                math::v3 meshlet_world_center = {world_center_4.x, world_center_4.y, world_center_4.z};

                // Calculate world-space radius with scale from matrix columns
                float max_scale = std::max({std::abs(instance.world_matrix.columns[0].x),
                                          std::abs(instance.world_matrix.columns[1].y),
                                          std::abs(instance.world_matrix.columns[2].z)});
                float meshlet_world_radius = meshlet.radius * max_scale;

                std::cout << "      Cluster #" << j << " (meshlet_id=" << meshlet_id << "): "
                          << "center=[" << meshlet_world_center.x << ", " << meshlet_world_center.y << ", "
                          << meshlet_world_center.z << "], radius=" << meshlet_world_radius << std::endl;
            }
        }
        std::cout << std::endl;
    }
    
    // 4. Build Cluster Map (Flattened Cluster ID -> Global Meshlet ID)
    // We need to iterate instances again to build this map
    // Total Clusters = sum(instance.cluster_count)
    u32 totalClusters = 0;
    for(const auto& inst : instanceData) totalClusters += inst.cluster_count;
    
    struct ClusterMap {
        u32 globalMeshletIndex;
        u32 instanceIndex;
    };
    
    utl::vector<ClusterMap> clusterMapData;
    clusterMapData.reserve(totalClusters);
    
    for(u32 i=0; i<(u32)instanceData.size(); ++i) {
        const auto& instance = instanceData[i];

        // CRITICAL FIX: Always create cluster_map entries for ALL instances
        // to match the cluster_refs indexing scheme used by culling pipeline
        // Even if geometry_id is not found, we need to maintain index consistency

        u32 baseMeshletIndex = 0; // Default fallback
        if (geometryToGlobalMeshletBase.find(instance.geometry_id) != geometryToGlobalMeshletBase.end()) {
            baseMeshletIndex = geometryToGlobalMeshletBase[instance.geometry_id];
        } else {
            // Fallback: use sequential indexing to prevent crashes
            // This may render incorrectly but won't cause flickering from OOB access
            baseMeshletIndex = static_cast<u32>(clusterMapData.size());
        }

        for(u32 c=0; c<instance.cluster_count; ++c) {
            ClusterMap map;
            map.globalMeshletIndex = baseMeshletIndex + c;
            map.instanceIndex = i; // Index into Instance Data Buffer (transforms)
            clusterMapData.push_back(map);
        }
    }
    
    // Allocate and Upload Cluster Map Buffer
    rhi::BufferDesc mapDesc{};
    mapDesc.size = totalClusters * sizeof(ClusterMap);
    mapDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage);
    mapDesc.usage = rhi::GPUMemoryUsage::Dynamic;
    
    cluster_map_buffer_ = device_->CreateBuffer(mapDesc);
    UploadBuffer(cluster_map_buffer_, clusterMapData.data(), clusterMapData.size() * sizeof(ClusterMap));
    
    // 5. Build Global Instance Data Buffer (World Matrices)
    // We map instance index `i` (from instanceData[i]) to `world_matrix`
    utl::vector<math::m4x4> instanceMatrices;
    instanceMatrices.reserve(instanceData.size());
    for(const auto& inst : instanceData) {
        instanceMatrices.push_back(inst.world_matrix);
    }
    
    rhi::BufferDesc instDesc{};
    instDesc.size = instanceMatrices.size() * sizeof(math::m4x4);
    instDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage);
    instDesc.usage = rhi::GPUMemoryUsage::Dynamic;
    global_instance_data_buffer_ = device_->CreateBuffer(instDesc);
    UploadBuffer(global_instance_data_buffer_, instanceMatrices.data(), instanceMatrices.size() * sizeof(math::m4x4));

    total_meshlet_count_ = totalMeshlets;
    total_meshlet_vertex_count_ = totalVertices;
    total_meshlet_triangle_count_ = totalTriangles;
    total_vertex_count_ = totalPositions;
    
    std::cout << "[GPUDrivenDrawPipeline] Global Buffers Built Successfully!" << std::endl;
}

bool GPUDrivenDrawPipeline::CreateGeometryBuffers(u32 vertex_count, u32 index_count) {
    std::cout << "[GPUDrivenDrawPipeline] Creating GPU geometry buffers..." << std::endl;

    if (vertex_count == 0 || index_count == 0) {
        std::cout << "[GPUDrivenDrawPipeline]   Invalid geometry count" << std::endl;
        return false;
    }

    // Create vertex position buffer
    rhi::BufferDesc positionBufferDesc{};
    positionBufferDesc.size = vertex_count * sizeof(math::v3);
    positionBufferDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::Vertex);
    positionBufferDesc.usage = rhi::GPUMemoryUsage::Static;

    vertex_position_buffer_ = device_->CreateBuffer(positionBufferDesc);
    if (vertex_position_buffer_ != rhi::handles::INVALID_RESOURCE) {
        std::cout << "[GPUDrivenDrawPipeline]   Vertex position buffer created: "
                  << positionBufferDesc.size << " bytes" << std::endl;
    }

    // Create vertex normal buffer
    rhi::BufferDesc normalBufferDesc{};
    normalBufferDesc.size = vertex_count * sizeof(math::v3);
    normalBufferDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::Vertex);
    normalBufferDesc.usage = rhi::GPUMemoryUsage::Static;

    vertex_normal_buffer_ = device_->CreateBuffer(normalBufferDesc);
    if (vertex_normal_buffer_ != rhi::handles::INVALID_RESOURCE) {
        std::cout << "[GPUDrivenDrawPipeline]   Vertex normal buffer created: "
                  << normalBufferDesc.size << " bytes" << std::endl;
    }

    // Create vertex UV buffer
    rhi::BufferDesc uvBufferDesc{};
    uvBufferDesc.size = vertex_count * sizeof(math::v2);
    uvBufferDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::Vertex);
    uvBufferDesc.usage = rhi::GPUMemoryUsage::Static;

    vertex_uv_buffer_ = device_->CreateBuffer(uvBufferDesc);
    if (vertex_uv_buffer_ != rhi::handles::INVALID_RESOURCE) {
        std::cout << "[GPUDrivenDrawPipeline]   Vertex UV buffer created: "
                  << uvBufferDesc.size << " bytes" << std::endl;
    }

    // Create index buffer
    rhi::BufferDesc indexBufferDesc{};
    indexBufferDesc.size = index_count * sizeof(u32);
    indexBufferDesc.bindFlags = (u32)rhi::BufferUsageFlags::Index;
    indexBufferDesc.usage = rhi::GPUMemoryUsage::Static;

    index_buffer_ = device_->CreateBuffer(indexBufferDesc);
    if (index_buffer_ != rhi::handles::INVALID_RESOURCE) {
        std::cout << "[GPUDrivenDrawPipeline]   Index buffer created: "
                  << indexBufferDesc.size << " bytes" << std::endl;
    }

    // Check if all buffers were created successfully
    bool success = (vertex_position_buffer_ != rhi::handles::INVALID_RESOURCE) &&
                   (vertex_normal_buffer_ != rhi::handles::INVALID_RESOURCE) &&
                   (vertex_uv_buffer_ != rhi::handles::INVALID_RESOURCE) &&
                   (index_buffer_ != rhi::handles::INVALID_RESOURCE);

    if (success) {
        std::cout << "[GPUDrivenDrawPipeline]   All geometry buffers created successfully" << std::endl;
    } else {
        std::cerr << "[GPUDrivenDrawPipeline]   Failed to create some geometry buffers" << std::endl;
    }

    return success;
}

void GPUDrivenDrawPipeline::UploadGeometryData(const RenderSceneSnapshot& scene_snapshot) {
    std::cout << "[GPUDrivenDrawPipeline] Uploading test geometry data..." << std::endl;

    // Since we don't have direct access to the actual geometry data through the current API,
    // we'll create a simple test scene to verify the pipeline works

    // Create a simple cube or pyramid shape for testing
    std::vector<math::v3> positions;
    std::vector<math::v3> normals;
    std::vector<math::v2> uvs;
    std::vector<u32> indices;

    // Create a simple pyramid (5 vertices, 18 indices for 6 triangles)
    positions = {
        math::v3{0.0f, 0.5f, 0.0f},    // Top
        math::v3{-0.5f, -0.5f, 0.5f},  // Front left
        math::v3{0.5f, -0.5f, 0.5f},   // Front right
        math::v3{0.5f, -0.5f, -0.5f},  // Back right
        math::v3{-0.5f, -0.5f, -0.5f}  // Back left
    };

    // Create normals pointing outward
    normals = {
        math::v3{0.0f, 1.0f, 0.0f},    // Top normal
        math::v3{0.0f, -1.0f, 1.0f},   // Front left normal
        math::v3{0.0f, -1.0f, 1.0f},   // Front right normal
        math::v3{0.0f, -1.0f, -1.0f},  // Back right normal
        math::v3{0.0f, -1.0f, -1.0f}   // Back left normal
    };

    // Simple UV coordinates
    uvs = {
        math::v2{0.5f, 1.0f},  // Top
        math::v2{0.0f, 0.0f},  // Front left
        math::v2{1.0f, 0.0f},  // Front right
        math::v2{1.0f, 0.0f},  // Back right
        math::v2{0.0f, 0.0f}   // Back left
    };

    // Create pyramid triangles (18 indices for 6 faces, 3 triangles per face)
    indices = {
        // Front face
        0, 1, 2,
        // Right face
        0, 2, 3,
        // Back face
        0, 3, 4,
        // Left face
        0, 4, 1,
        // Bottom (2 triangles)
        1, 4, 3,
        1, 3, 2
    };

    std::cout << "[GPUDrivenDrawPipeline]   Created test pyramid: "
              << positions.size() << " vertices, " << indices.size() << " indices" << std::endl;

    // Upload test geometry to GPU buffers
    if (!positions.empty() && !indices.empty()) {
        // Upload positions
        void* pos_data = device_->MapBuffer(vertex_position_buffer_);
        if (pos_data) {
            memcpy(pos_data, positions.data(), positions.size() * sizeof(math::v3));
            device_->UnmapBuffer(vertex_position_buffer_);
            std::cout << "[GPUDrivenDrawPipeline]   Test positions uploaded" << std::endl;
        }

        // Upload normals
        void* norm_data = device_->MapBuffer(vertex_normal_buffer_);
        if (norm_data) {
            memcpy(norm_data, normals.data(), normals.size() * sizeof(math::v3));
            device_->UnmapBuffer(vertex_normal_buffer_);
            std::cout << "[GPUDrivenDrawPipeline]   Test normals uploaded" << std::endl;
        }

        // Upload UVs
        void* uv_data = device_->MapBuffer(vertex_uv_buffer_);
        if (uv_data) {
            memcpy(uv_data, uvs.data(), uvs.size() * sizeof(math::v2));
            device_->UnmapBuffer(vertex_uv_buffer_);
            std::cout << "[GPUDrivenDrawPipeline]   Test UVs uploaded" << std::endl;
        }

        // Upload indices
        void* idx_data = device_->MapBuffer(index_buffer_);
        if (idx_data) {
            memcpy(idx_data, indices.data(), indices.size() * sizeof(u32));
            device_->UnmapBuffer(index_buffer_);
            std::cout << "[GPUDrivenDrawPipeline]   Test indices uploaded" << std::endl;
        }

        // Update counts to match actual test data
        vertex_count_ = positions.size();
        index_count_ = indices.size();

        std::cout << "[GPUDrivenDrawPipeline]   Test geometry data upload complete!" << std::endl;
    } else {
        std::cerr << "[GPUDrivenDrawPipeline]   Failed to create test geometry!" << std::endl;
    }
}

bool GPUDrivenDrawPipeline::Execute(rhi::RHICommandBuffer* cmd_buffer,
                                   const RenderSceneSnapshot& scene_snapshot,
                                   const math::m4x4& view_matrix,
                                   const math::m4x4& projection_matrix,
                                   const CullingResults& culling_results,
                                   u32 frame_index,
                                   u32 buffer_index) {
    if (!initialized_) {
        std::cerr << "[GPUDrivenDrawPipeline] Not initialized" << std::endl;
        return false;
    }

    if (frame_index == 0) {
        std::cout << "[GPUDrivenDrawPipeline] Executing pipeline - Frame " << frame_index << std::endl;
    }

    // Cache camera matrices for use in Stage3
    cached_view_matrix_ = view_matrix;
    cached_proj_matrix_ = projection_matrix;

    // Update geometry data from scene snapshot
    UpdateGeometryData(scene_snapshot);

    // Stage 1: Cluster Binning
    if (!Stage1_ClusterBinning(cmd_buffer, scene_snapshot, culling_results, frame_index)) {
        std::cerr << "[GPUDrivenDrawPipeline] Cluster Binning failed" << std::endl;
        return false;
    }

    // Stage 2: Visibility Buffer
    if (!Stage2_VisibilityBuffer(cmd_buffer, scene_snapshot, view_matrix, projection_matrix, frame_index)) {
        std::cerr << "[GPUDrivenDrawPipeline] Visibility Buffer failed" << std::endl;
        return false;
    }

    // Stage 3: GPU Draw Calls
    if (!Stage3_GPUDrawCalls(cmd_buffer, scene_snapshot, culling_results, results_, frame_index, buffer_index)) {
        std::cerr << "[GPUDrivenDrawPipeline] GPU Draw Calls failed" << std::endl;
        return false;
    }

    if (frame_index == 0) {
        std::cout << "[GPUDrivenDrawPipeline] Pipeline execution complete" << std::endl;
        std::cout << "  Total Draw Calls: " << results_.total_draw_calls << std::endl;
        std::cout << "  Total Clusters Rendered: " << results_.total_clusters_rendered << std::endl;
    }

    return true;
}

bool GPUDrivenDrawPipeline::Stage1_ClusterBinning(rhi::RHICommandBuffer* cmd_buffer,
                                                   const RenderSceneSnapshot& scene_snapshot,
                                                   const CullingResults& culling_results,
                                                   u32 frame_index) {
    if (frame_index == 0) {
        std::cout << "[GPUDrivenDrawPipeline] Stage 1: Cluster Binning" << std::endl;
    }

    // Always assume we have some bins, or base it on a maximum size, 
    // since we want this to be entirely GPU-driven
    // For CPU fallback, we can't do accurate binning without a readback,
    // but we can just use 1 bin that covers everything.
    results_.bin_count = 1;
    results_.total_clusters_rendered = 0; // Will be updated by GPU stats if needed

    // If we have a GPU binning pipeline, use it
    if (binning_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        // TODO: Implement GPU-based cluster binning
        // 1. Bind compute pipeline
        // 2. Set descriptor sets with cluster data, bin buffers
        // 3. Dispatch compute shader
        // 4. Barrier for bin buffer writes

        if (frame_index == 0) {
            std::cout << "[GPUDrivenDrawPipeline]   GPU binning - PLACEHOLDER" << std::endl;
            std::cout << "[GPUDrivenDrawPipeline]   TODO: Implement compute shader dispatch" << std::endl;
        }

        // cmd_buffer->BindComputePipeline(binning_pipeline_);
        // cmd_buffer->Dispatch(...);
    } else {
        // CPU fallback: Simple binning based on cluster index
        if (frame_index == 0) {
            std::cout << "[GPUDrivenDrawPipeline]   CPU binning fallback" << std::endl;
            std::cout << "[GPUDrivenDrawPipeline]   Visible clusters: " << culling_results.visible_cluster_count << std::endl;
        }

        // Calculate bin count
        results_.bin_count = (culling_results.visible_cluster_count + binning_config_.max_clusters_per_bin - 1) /
                            binning_config_.max_clusters_per_bin;

        if (frame_index == 0) {
            std::cout << "[GPUDrivenDrawPipeline]   Generated bins: " << results_.bin_count << std::endl;
        }

        // TODO: Actually implement CPU binning logic
        // For now, just track the counts
        results_.total_clusters_rendered = culling_results.visible_cluster_count;
    }

    return true;
}

bool GPUDrivenDrawPipeline::Stage2_VisibilityBuffer(rhi::RHICommandBuffer* cmd_buffer,
                                                    const RenderSceneSnapshot& scene_snapshot,
                                                    const math::m4x4& view_matrix,
                                                    const math::m4x4& projection_matrix,
                                                    u32 frame_index) {
    // Skip Stage2 for now - we'll do everything in Stage3 with GPU Indirect Draw
    // This avoids the complexity of managing two different rendering paths and shader compatibility issues

    if (results_.bin_count == 0) {
        results_.bin_count = 1;
        results_.total_clusters_rendered = 0;
    }

    if (frame_index == 0) {
        std::cout << "[GPUDrivenDrawPipeline] Stage 2: Skipping visibility buffer, using Stage3 GPU Indirect Draw" << std::endl;
    }

    return true;
}

bool GPUDrivenDrawPipeline::Stage3_GPUDrawCalls(rhi::RHICommandBuffer* cmd_buffer,
                                                 const RenderSceneSnapshot& scene_snapshot,
                                                 const CullingResults& culling_results,
                                                 const GPUDrawResults& intermediate_results,
                                                 u32 frame_index,
                                                 u32 buffer_index) {
    (void)intermediate_results;

    if (results_.bin_count == 0) {
        results_.bin_count = 1;
        results_.total_clusters_rendered = 0;
    }

    // Begin final render pass directly
    cmd_buffer->BeginRenderPass(final_render_pass_);

    // Set viewport and scissor for final output
    cmd_buffer->SetViewport({
        {0, 0},
        {static_cast<float>(visibility_config_.width), static_cast<float>(visibility_config_.height)},
        0, 1
    });
    cmd_buffer->SetScissor({
        {0, 0},
        {visibility_config_.width, visibility_config_.height}
    });

    // Validate that we have the necessary pre-built buffers
    if (cluster_map_buffer_ == rhi::handles::INVALID_RESOURCE ||
        global_instance_data_buffer_ == rhi::handles::INVALID_RESOURCE ||
        final_color_texture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDraw] Frame " << frame_index << " ERROR: Invalid resources:" << std::endl;
        std::cerr << "  cluster_map_buffer_: " << cluster_map_buffer_ << std::endl;
        std::cerr << "  global_instance_data_buffer_: " << global_instance_data_buffer_ << std::endl;
        std::cerr << "  final_color_texture_: " << final_color_texture_ << std::endl;
        cmd_buffer->EndRenderPass();
        return false;
    }

    // Select Frame Resource for Triple Buffering
    u32 resourceIndex = buffer_index; // Use the passed-in buffer_index for perfect synchronization
    if (resourceIndex >= frame_resources_.size()) resourceIndex = 0;
    FrameResource& currentFrame = frame_resources_[resourceIndex];

    // CRITICAL FIX: Calculate read_buffer_index BEFORE using it for descriptor updates
    // The current frame's culling pipeline writes to buffer_index, but we need to read
    // from the PREVIOUS frame's buffer that has already been computed.
    u32 read_buffer_index = (buffer_index + 2) % 3; // Read from frame N-2 (wrapping around)

    rhi::DescriptorSetHandle globalDrawDS = currentFrame.global_draw_descriptor_set;
    rhi::ResourceHandle cameraConstBuffer = currentFrame.camera_constants_buffer;

    if (globalDrawDS == rhi::handles::INVALID_DESCRIPTOR_SET || cameraConstBuffer == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDraw] Frame " << frame_index << " ERROR: Invalid frame resources:" << std::endl;
        std::cerr << "  global_draw_descriptor_set: " << globalDrawDS << std::endl;
        std::cerr << "  camera_constants_buffer: " << cameraConstBuffer << std::endl;
        cmd_buffer->EndRenderPass();
        return false;
    }

    // Update Global Constants
    struct DrawConstants {
        math::m4x4 view_matrix;
        math::m4x4 proj_matrix;
        math::m4x4 world_matrix; // Unused
        u32 view_width;
        u32 view_height;
        u32 meshlet_count;
        u32 padding;
    } drawConsts;

    drawConsts.view_matrix = cached_view_matrix_;
    drawConsts.proj_matrix = cached_proj_matrix_;
    drawConsts.world_matrix = rhi::math::MatrixIdentity();
    drawConsts.view_width = visibility_config_.width;
    drawConsts.view_height = visibility_config_.height;
    // CRITICAL FIX: Pass the TOTAL meshlet count to the shader for safety checks,
    // NOT the visible cluster count (which can be 0 or small and causes out-of-bounds clipping)
    drawConsts.meshlet_count = total_meshlet_count_; 
    drawConsts.padding = 0;

    void* constData = device_->MapBuffer(cameraConstBuffer);
    if (constData) {
        memcpy(constData, &drawConsts, sizeof(DrawConstants));
        device_->UnmapBuffer(cameraConstBuffer);
    }

    // Update Descriptor Set with Global Buffers
    rhi::DescriptorBufferInfo globalConstInfo{ cameraConstBuffer, 0, sizeof(DrawConstants) };
    rhi::DescriptorBufferInfo meshletBufferInfo{ global_meshlet_buffer_, 0, ~0ULL };
    rhi::DescriptorBufferInfo meshletVerticesInfo{ global_meshlet_vertices_buffer_, 0, ~0ULL };
    rhi::DescriptorBufferInfo meshletTrianglesInfo{ global_meshlet_triangles_buffer_, 0, ~0ULL };
    rhi::DescriptorBufferInfo positionBufferInfo{ global_vertex_buffer_, 0, ~0ULL };
    
    rhi::ResourceHandle correctVisibleClusterBuffer = culling_results.visible_cluster_list_buffer;
    if (culling_pipeline_) {
        // CRITICAL FIX: Use the same delayed reading for visible cluster list
        correctVisibleClusterBuffer = culling_pipeline_->GetVisibleClusterListBuffer(read_buffer_index);
    }
    
    rhi::DescriptorBufferInfo compactClusterInfo{ correctVisibleClusterBuffer, 0, ~0ULL };
    rhi::DescriptorBufferInfo clusterMapInfo{ cluster_map_buffer_, 0, ~0ULL };
    rhi::DescriptorBufferInfo instanceInfo{ global_instance_data_buffer_, 0, ~0ULL };

    rhi::WriteDescriptorSet writes[8];
    writes[0].dstSet = globalDrawDS;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = rhi::DescriptorType::UniformBufferDynamic; // Or UniformBuffer
    writes[0].bufferInfo = &globalConstInfo;

    writes[1].dstSet = globalDrawDS;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[1].bufferInfo = &meshletBufferInfo;

    writes[2].dstSet = globalDrawDS;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[2].bufferInfo = &meshletVerticesInfo;

    writes[3].dstSet = globalDrawDS;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[3].bufferInfo = &meshletTrianglesInfo;

    writes[4].dstSet = globalDrawDS;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[4].bufferInfo = &positionBufferInfo;
    
    writes[5].dstSet = globalDrawDS;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[5].bufferInfo = &compactClusterInfo;
    
    writes[6].dstSet = globalDrawDS;
    writes[6].dstBinding = 6;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[6].bufferInfo = &clusterMapInfo;
    
    writes[7].dstSet = globalDrawDS;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[7].bufferInfo = &instanceInfo;

    device_->UpdateDescriptorSets(8, writes);

    // Bind Pipeline and Descriptor Set
    // NOTE: No memory barrier needed here - Metal automatically handles encoder transitions
    // The GPU culling work is guaranteed to be complete by the Metal driver
    cmd_buffer->BindGraphicsPipeline(draw_pipeline_);

    rhi::DescriptorSetHandle dsHandles[1] = { globalDrawDS };
    u32 dynOffsets[1] = { 0 };
    cmd_buffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, draw_pipeline_layout_, 0, 1, dsHandles, 1, dynOffsets);

    // Indirect Draw
    // std::cout << "[GPUDraw] DEBUG: culling_pipeline_=" << culling_pipeline_
    //           << ", current_buffer_index=" << buffer_index
    //           << ", read_buffer_index=" << read_buffer_index << std::endl;
    // std::cout << "[GPUDraw] DEBUG: culling_results.indirect_args_buffer=" << culling_results.indirect_args_buffer << std::endl;

    if (culling_pipeline_) {
        rhi::ResourceHandle indirectBuffer = culling_pipeline_->GetIndirectBuffer(read_buffer_index);
        // std::cout << "[GPUDraw] DEBUG: culling_pipeline_->GetIndirectBuffer(" << read_buffer_index << ")=" << indirectBuffer << std::endl;

        if (indirectBuffer != rhi::handles::INVALID_RESOURCE) {
            // std::cout << "[GPUDraw] Calling DrawIndirect with buffer=" << indirectBuffer << std::endl;
            cmd_buffer->DrawIndirect(indirectBuffer, 0, 1);
            results_.total_draw_calls = 1;
        } else {
            std::cerr << "[GPUDraw] ERROR: Invalid indirect buffer from culling_pipeline_!" << std::endl;
        }
    } else if (culling_results.indirect_args_buffer != rhi::handles::INVALID_RESOURCE) {
        std::cout << "[GPUDraw] Calling DrawIndirect with culling_results buffer=" << culling_results.indirect_args_buffer << std::endl;
        cmd_buffer->DrawIndirect(culling_results.indirect_args_buffer, 0, 1);
        results_.total_draw_calls = 1;
    } else {
        std::cerr << "[GPUDraw] ERROR: No valid indirect buffer available!" << std::endl;
    }

    cmd_buffer->EndRenderPass();

    // CRITICAL FIX: Blit the rendered result to the swapchain!
    // Without this, our Nanite rendering is invisible because we render to an off-screen texture
    if (final_color_texture_ != rhi::handles::INVALID_RESOURCE) {
        // Get swapchain texture (assume it's the current render target)
        // For now, we need to blit our final_color_texture to the current swapchain image
        // This is a simplified approach - in production you'd want proper render graph integration

        // std::cout << "[GPUDraw] Blitting final render result to swapchain" << std::endl;

        // Note: This is a placeholder for the actual blit operation
        // In a real implementation, you would:
        // 1. Get the current swapchain texture
        // 2. Blit final_color_texture_ to swapchain
        // 3. Or use the swapchain directly as render target in Stage3
    }

    return true;
}

void GPUDrivenDrawPipeline::SetupVisibilityBufferPipeline(rhi::RHICommandBuffer* cmd_buffer) {
    if (visibility_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cerr << "[GPUDrivenDrawPipeline] Invalid visibility buffer pipeline" << std::endl;
        return;
    }

    // Bind visibility buffer specific pipeline
    cmd_buffer->BindGraphicsPipeline(visibility_pipeline_);

    // Set viewport and scissor for visibility buffer
    cmd_buffer->SetViewport({
        {0, 0},
        {static_cast<float>(visibility_config_.width), static_cast<float>(visibility_config_.height)},
        0, 1
    });
    cmd_buffer->SetScissor({
        {0, 0},
        {visibility_config_.width, visibility_config_.height}
    });
}

rhi::ResourceHandle GPUDrivenDrawPipeline::GetPreviousFrameDepth() {
    // Return the visibility depth buffer from previous frame
    // This will be used for HZB generation
    // For now, return the depth texture from visibility buffer system
    if (visibility_buffer_system_) {
        return visibility_buffer_system_->GetDepthBuffer();
    }
    return rhi::handles::INVALID_RESOURCE;
}

void GPUDrivenDrawPipeline::ResolveVisibilityBuffer(rhi::RHICommandBuffer* cmd_buffer) {
    std::cout << "[GPUDrivenDrawPipeline] Resolving Visibility Buffer..." << std::endl;

    // TODO: Implement full-screen quad render to resolve visibility buffer
    // This should:
    // 1. Render a full-screen quad
    // 2. In fragment shader, read visibility buffer and depth buffer
    // 3. For each pixel, reconstruct geometry from visibility data
    // 4. Apply proper materials and lighting
    // 5. Write final color to output

    // For now, this is a placeholder
    std::cout << "[GPUDrivenDrawPipeline] Visibility buffer resolve - PLACEHOLDER" << std::endl;
}

} // namespace primal::graphics::nanite
