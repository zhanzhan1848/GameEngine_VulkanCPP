#include "VisibilityBufferSystem.h"
#include "GPUCullingPipeline.h"
#include "NaniteResourceManager.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include "../RHI/Core/RHIMath.h"
#include "../RHI/Core/RHIGpuMesh.h"
#include "../Scene/RenderSceneSnapshot.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>

namespace primal::graphics::nanite {

namespace {
    std::vector<u8> LoadShaderBytecode(const char* shaderName, const char* entryPoint) {
        std::string shaderPath = std::string("shaders/") + shaderName + ".metal";

        std::ifstream file(shaderPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            shaderPath = std::string("Darwin/Debug/shaders/") + shaderName + ".metal";
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

        return bytecode;
    }
} // anonymous namespace

VisibilityBufferSystem::~VisibilityBufferSystem() {
    Shutdown();
}

bool VisibilityBufferSystem::Initialize(rhi::RHIDeviceBase* device, const Config& config) {
    if (initialized_) {
        std::cerr << "[VisibilityBufferSystem] Already initialized" << std::endl;
        return false;
    }

    device_ = device;
    config_ = config;

    std::cout << "[VisibilityBufferSystem] Initializing..." << std::endl;
    std::cout << "  Resolution: " << config_.width << "x" << config_.height << std::endl;
    std::cout << "  Format: " << static_cast<u32>(config_.format) << std::endl;
    std::cout << "  Depth: " << (config_.enable_depth ? "Enabled" : "Disabled") << std::endl;

    if (!CreateVisibilityResources()) {
        std::cerr << "[VisibilityBufferSystem] Failed to create visibility resources" << std::endl;
        return false;
    }

    if (!CreateVisibilityRenderPass()) {
        std::cerr << "[VisibilityBufferSystem] Failed to create visibility render pass" << std::endl;
        return false;
    }

    if (!CreateVisibilityPipeline()) {
        std::cerr << "[VisibilityBufferSystem] Failed to create visibility pipeline" << std::endl;
        return false;
    }

    if (!CreateDescriptorSets()) {
        std::cerr << "[VisibilityBufferSystem] Failed to create descriptor sets" << std::endl;
        return false;
    }

    initialized_ = true;
    std::cout << "[VisibilityBufferSystem] Initialized successfully" << std::endl;

    return true;
}

void VisibilityBufferSystem::Shutdown() {
    if (!initialized_) return;

    std::cout << "[VisibilityBufferSystem] Shutting down..." << std::endl;

    // Cleanup resources
    if (device_) {
        // TODO: Properly destroy resources via RHI
        visibility_buffer_ = rhi::handles::INVALID_RESOURCE;
        depth_buffer_ = rhi::handles::INVALID_RESOURCE;
        visibility_render_pass_ = rhi::handles::INVALID_RENDER_PASS;
        visibility_pipeline_ = rhi::handles::INVALID_PIPELINE;
        visibility_pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
        descriptor_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
        descriptor_set_ = rhi::handles::INVALID_DESCRIPTOR_SET;
    }

    initialized_ = false;
}

bool VisibilityBufferSystem::CreateVisibilityResources() {
    std::cout << "[VisibilityBufferSystem] Creating visibility resources..." << std::endl;

    // Create visibility buffer texture
    rhi::TextureDesc visibilityDesc{};
    visibilityDesc.size = { config_.width, config_.height, 1 };
    visibilityDesc.format = config_.format;
    visibilityDesc.type = rhi::TextureType::Texture2D;
    visibilityDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;

    visibility_buffer_ = device_->CreateTexture(visibilityDesc);
    if (visibility_buffer_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[VisibilityBufferSystem] Failed to create visibility buffer" << std::endl;
        return false;
    }

    std::cout << "[VisibilityBufferSystem] Visibility buffer created: "
              << config_.width << "x" << config_.height << std::endl;

    // Create depth buffer texture
    if (config_.enable_depth) {
        rhi::TextureDesc depthDesc{};
        depthDesc.size = { config_.width, config_.height, 1 };
        depthDesc.format = rhi::DataFormat::D32_Float;
        depthDesc.type = rhi::TextureType::Texture2D;
        depthDesc.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;

        depth_buffer_ = device_->CreateTexture(depthDesc);
        if (depth_buffer_ == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[VisibilityBufferSystem] Failed to create depth buffer" << std::endl;
            return false;
        }

        std::cout << "[VisibilityBufferSystem] Depth buffer created" << std::endl;
    }

    return true;
}

bool VisibilityBufferSystem::CreateVisibilityRenderPass() {
    std::cout << "[VisibilityBufferSystem] Creating visibility render pass..." << std::endl;

    rhi::RenderPassDesc passDesc{};

    // Color attachment - visibility buffer
    rhi::RenderPassDesc::Attachment colorAttachment{};
    colorAttachment.texture = visibility_buffer_;
    colorAttachment.format = config_.format;
    colorAttachment.loadOp = rhi::LoadAction::Clear;
    colorAttachment.storeOp = rhi::StoreAction::Store;
    colorAttachment.clearValue.color = math::v4{0.0f, 0.0f, 0.0f, 0.0f}; // Clear to invalid ID

    passDesc.colorAttachments.push_back(colorAttachment);

    // Depth attachment
    if (config_.enable_depth) {
        rhi::RenderPassDesc::Attachment depthAttachment{};
        depthAttachment.texture = depth_buffer_;
        depthAttachment.format = rhi::DataFormat::D32_Float;
        depthAttachment.loadOp = rhi::LoadAction::Clear;
        depthAttachment.storeOp = rhi::StoreAction::Store;
        depthAttachment.clearValue.depth = 1.0f;
        depthAttachment.clearValue.stencil = 0;

        passDesc.depthAttachment = depthAttachment;
    }

    // Set viewport
    passDesc.viewport.size.x = static_cast<float>(config_.width);
    passDesc.viewport.size.y = static_cast<float>(config_.height);
    passDesc.scissor.extent.x = config_.width;
    passDesc.scissor.extent.y = config_.height;

    visibility_render_pass_ = device_->CreateRenderPass(passDesc);
    if (visibility_render_pass_ == rhi::handles::INVALID_RENDER_PASS) {
        std::cerr << "[VisibilityBufferSystem] Failed to create visibility render pass" << std::endl;
        return false;
    }

    std::cout << "[VisibilityBufferSystem] Visibility render pass created" << std::endl;
    return true;
}

bool VisibilityBufferSystem::CreateVisibilityPipeline() {
    std::cout << "[VisibilityBufferSystem] Creating visibility pipeline..." << std::endl;

    // Load visibility buffer shaders
    auto vertexShaderCode = LoadShaderBytecode("VisibilityBuffer", "visibility_vertex_shader");
    auto fragmentShaderCode = LoadShaderBytecode("VisibilityBuffer", "visibility_fragment_shader");

    if (vertexShaderCode.empty() || fragmentShaderCode.empty()) {
        std::cout << "[VisibilityBufferSystem] Visibility buffer shaders not found, skipping pipeline creation" << std::endl;
        return false;
    }

    rhi::ShaderHandle vertexShader = device_->CreateShader(
        vertexShaderCode.data(),
        vertexShaderCode.size(),
        rhi::ShaderStage::Vertex,
        "visibility_vertex_shader"
    );

    rhi::ShaderHandle fragmentShader = device_->CreateShader(
        fragmentShaderCode.data(),
        fragmentShaderCode.size(),
        rhi::ShaderStage::Pixel,
        "visibility_fragment_shader"
    );

    if (vertexShader == rhi::handles::INVALID_SHADER ||
        fragmentShader == rhi::handles::INVALID_SHADER) {
        std::cerr << "[VisibilityBufferSystem] Failed to create visibility shaders" << std::endl;
        return false;
    }

    // Create pipeline layout
    rhi::PipelineLayoutDesc layoutDesc{};
    layoutDesc.setLayoutCount = 0;
    layoutDesc.setLayouts = nullptr;

    visibility_pipeline_layout_ = device_->CreatePipelineLayout(layoutDesc);
    if (visibility_pipeline_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) {
        std::cerr << "[VisibilityBufferSystem] Failed to create pipeline layout" << std::endl;
        return false;
    }

    // Create graphics pipeline
    rhi::GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = vertexShader;
    pipelineDesc.pixelShader = fragmentShader;
    pipelineDesc.layout = visibility_pipeline_layout_;

    // Render target format
    pipelineDesc.renderTargetCount = 1;
    pipelineDesc.renderTargetFormats[0] = config_.format;

    // Depth settings
    if (config_.enable_depth) {
        pipelineDesc.depthStencilFormat = rhi::DataFormat::D32_Float;
        pipelineDesc.enableDepthTest = true;
        pipelineDesc.enableDepthWrite = true;
        pipelineDesc.depthFunc = rhi::ComparisonFunc::Less;
    }

    // Rasterizer settings
    pipelineDesc.cullMode = rhi::CullMode::Back;
    pipelineDesc.fillMode = rhi::FillMode::Solid;

    // Disable blending for visibility buffer
    pipelineDesc.enableBlend = false;

    visibility_pipeline_ = device_->CreateGraphicsPipeline(pipelineDesc);
    if (visibility_pipeline_ == rhi::handles::INVALID_PIPELINE) {
        std::cerr << "[VisibilityBufferSystem] Failed to create visibility pipeline" << std::endl;
        return false;
    }

    std::cout << "[VisibilityBufferSystem] Visibility pipeline created successfully" << std::endl;
    return true;
}

bool VisibilityBufferSystem::CreateDescriptorSets() {
    std::cout << "[VisibilityBufferSystem] Creating descriptor sets..." << std::endl;

    // TODO: Implement descriptor set creation for visibility buffer resources
    // For now, return true as placeholder

    return true;
}

VisibilityBufferSystem::RenderResult VisibilityBufferSystem::RenderVisibilityBuffer(
    rhi::RHICommandBuffer* cmd_buffer,
    const RenderSceneSnapshot& scene_snapshot,
    const math::m4x4& view_matrix,
    const math::m4x4& projection_matrix,
    const CullingResults& culling_results,
    u32 frame_index) {

    if (!initialized_) {
        std::cerr << "[VisibilityBufferSystem] Not initialized" << std::endl;
        return {};
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    RenderResult result;
    result.visibility_buffer = visibility_buffer_;
    result.depth_buffer = depth_buffer_;

    std::cout << "[VisibilityBufferSystem] Rendering visibility buffer - Frame " << frame_index << std::endl;

    if (!RenderToVisibilityBuffer(cmd_buffer, scene_snapshot, view_matrix, projection_matrix, culling_results)) {
        std::cerr << "[VisibilityBufferSystem] Visibility buffer rendering failed" << std::endl;
        return result;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.render_time_ms = std::chrono::duration<float, std::milli>(end_time - start_time).count();

    last_result_ = result;

    std::cout << "[VisibilityBufferSystem] Visibility buffer rendered in " << result.render_time_ms << " ms" << std::endl;
    std::cout << "  Visible triangles: " << result.visible_triangles << std::endl;
    std::cout << "  Visible clusters: " << result.visible_clusters << std::endl;

    return result;
}

bool VisibilityBufferSystem::RenderToVisibilityBuffer(
    rhi::RHICommandBuffer* cmd_buffer,
    const RenderSceneSnapshot& scene_snapshot,
    const math::m4x4& view_matrix,
    const math::m4x4& projection_matrix,
    const CullingResults& culling_results) {

    std::cout << "[VisibilityBufferSystem] Rendering to visibility buffer..." << std::endl;

    // TODO: Implement proper visibility buffer rendering
    // For now, this is a placeholder that shows the structure

    // Begin visibility render pass
    cmd_buffer->BeginRenderPass(visibility_render_pass_);

    // Bind visibility pipeline
    if (visibility_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        cmd_buffer->BindGraphicsPipeline(visibility_pipeline_);

        // TODO: Set viewport and scissor
        // TODO: Bind descriptor sets with scene data
        // TODO: Render visible clusters from culling results

        // Placeholder: Render nothing for now
        std::cout << "[VisibilityBufferSystem] TODO: Implement actual visibility buffer rendering" << std::endl;
    } else {
        std::cout << "[VisibilityBufferSystem] Visibility pipeline not available" << std::endl;
    }

    // End visibility render pass
    cmd_buffer->EndRenderPass();

    // Update result statistics
    // TODO: Extract actual statistics from rendering
    last_result_.visible_triangles = culling_results.visible_cluster_count * 100; // Placeholder
    last_result_.visible_clusters = culling_results.visible_cluster_count;

    return true;
}

bool VisibilityBufferSystem::UpdateConfig(const Config& new_config) {
    std::cout << "[VisibilityBufferSystem] Updating configuration..." << std::endl;

    // Check if dimensions changed
    if (new_config.width != config_.width ||
        new_config.height != config_.height ||
        new_config.format != config_.format) {

        // Need to recreate resources
        Shutdown();
        return Initialize(device_, new_config);
    }

    // Manual member copy since Config has non-copyable members
    config_.width = new_config.width;
    config_.height = new_config.height;
    config_.format = new_config.format;
    config_.enable_depth = new_config.enable_depth;
    config_.enable_conservative = new_config.enable_conservative;
    config_.enable_msaa = new_config.enable_msaa;
    return true;
}

// Visibility Buffer Reader Implementation

bool VisibilityBufferReader::ResolveVisibilityBuffer(
    rhi::ResourceHandle visibility_buffer,
    rhi::ResourceHandle depth_buffer,
    rhi::ResourceHandle output_texture,
    rhi::RHICommandBuffer* cmd_buffer,
    const RenderSceneSnapshot& scene_snapshot) {

    // TODO: Implement visibility buffer resolve pass
    // This should read the visibility buffer and render actual geometry with materials
    std::cout << "[VisibilityBufferReader] Resolve - PLACEHOLDER" << std::endl;

    return false;
}

VisibilityBufferReader::VisibilityStats VisibilityBufferReader::ExtractStats(
    rhi::ResourceHandle visibility_buffer,
    rhi::RHIDeviceBase* device) {

    VisibilityStats stats{};

    // TODO: Implement visibility statistics extraction
    // This would read back the visibility buffer and count visible/occluded pixels

    return stats;
}

} // namespace primal::graphics::nanite