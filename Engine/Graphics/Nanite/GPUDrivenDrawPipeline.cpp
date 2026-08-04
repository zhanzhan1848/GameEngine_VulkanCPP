#include "GPUDrivenDrawPipeline.h"
#include "GPUCullingPipeline.h"
#include "NaniteResourceManager.h"
#include "HZBSystem.h"
#include "MeshletSynthesis.h"
#include "VisibilityBufferSystem.h"
#include "../RHI/Core/RHIDevice.h"
#include "../RHI/Core/RHICommand.h"
#include "../RHI/Core/RHIMath.h"
#include "../RHI/Core/RHIGpuMesh.h"
#include "../Scene/RenderSceneSnapshot.h"
#include "../RenderScene.h"
#include "../RenderPipeline/StreamingMesh.h"
#include "../../Content/ContentToEngine.h" // Needed for get_rhi_mesh_asset
#include "../Dawn/ShaderLoader.h"
#include "../Utils/ShaderRegistry.h"
#include "CommonHeaders.h"
#include <cassert>
#include <fstream>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <set>
#include <map>

namespace primal::graphics::nanite {

namespace {
    // N0c: Forks shader loading on backend.
    //   Metal:  loads from EngineTest/shaders/<name>.metal (existing path).
    //   Dawn:   loads from Engine/Graphics/Dawn/shaders/Nanite/<name>.wgsl
    //           (native) or via dawn::LoadWGSL MEMFS lookup (WASM).
    //   Vulkan: loads SPIR-V binary from Engine/Graphics/Vulkan/shaders/Nanite/<name>.spv
    //           via ShaderRegistry. No null terminator (VulkanShader rejects if
    //           size % 4 != 0 — padding bytes would break the SPIR-V parser).
    std::vector<u8> LoadShaderBytecode(const char* shaderName, const char* entryPoint,
                                       rhi::RHIDeviceBase* device) {
        auto platform = device ? device->GetPlatform() : rhi::RHIPlatform::Metal;
        if (platform == rhi::RHIPlatform::Dawn) {
#ifdef __EMSCRIPTEN__
            std::string fullName = std::string("Nanite/") + shaderName;
            std::string src = dawn::LoadWGSL(fullName.c_str());
            if (src.empty()) {
                std::cerr << "[GPUDrivenDrawPipeline] Failed to load WGSL shader: "
                          << shaderName << " (entry: " << entryPoint << ")" << std::endl;
                return {};
            }
            // +1 for null terminator: Dawn's ToWGPUStringView uses WGPU_STRLEN.
            std::vector<u8> bytecode(src.begin(), src.end());
            bytecode.push_back(0);
            return bytecode;
#else
            std::string path = utils::ShaderRegistry::GetNaniteShaderPath(
                rhi::RHIPlatform::Dawn, shaderName);
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/" + path;
                file.open(path, std::ios::binary | std::ios::ate);
            }
            if (!file.is_open()) {
                std::cerr << "[GPUDrivenDrawPipeline] Failed to load WGSL shader: "
                          << shaderName << " (entry: " << entryPoint << ")" << std::endl;
                return {};
            }
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            // +1 for null terminator: Dawn's ToWGPUStringView uses WGPU_STRLEN,
            // which calls strlen on the buffer. Without a null terminator the
            // parser reads into adjacent memory and reports phantom errors.
            std::vector<u8> bytecode(static_cast<size_t>(size) + 1, 0);
            if (!file.read(reinterpret_cast<char*>(bytecode.data()), size)) {
                std::cerr << "[GPUDrivenDrawPipeline] Failed to read WGSL shader: "
                          << shaderName << std::endl;
                return {};
            }
            return bytecode;
#endif
        }

        if (platform == rhi::RHIPlatform::Vulkan) {
            std::string path = utils::ShaderRegistry::GetNaniteShaderPath(platform, shaderName);
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                path = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/" + path;
                file.open(path, std::ios::binary | std::ios::ate);
            }
            if (!file.is_open()) {
                std::cerr << "[GPUDrivenDrawPipeline] Failed to load SPIR-V shader: "
                          << shaderName << " (entry: " << entryPoint << ")" << std::endl;
                return {};
            }
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            // Exact byte count — VulkanShader rejects SPIR-V whose size % 4 != 0.
            std::vector<u8> bytecode(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytecode.data()), size)) {
                std::cerr << "[GPUDrivenDrawPipeline] Failed to read SPIR-V shader: "
                          << shaderName << std::endl;
                return {};
            }
            return bytecode;
        }

        // Metal path (unchanged)
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
//        std::cout << "[GPUDrivenDrawPipeline] Loaded shader: " << shaderName
//                  << " (" << size << " bytes)" << std::endl;
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

//    std::cout << "[GPUDrivenDrawPipeline] CreatePipelines() succeeded, draw_pipeline_layout_: "
//              << draw_pipeline_layout_ << std::endl;

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
        if (global_element_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(global_element_buffer_); // 🔥 NEW
        if (cluster_map_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(cluster_map_buffer_);
        if (global_instance_data_buffer_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(global_instance_data_buffer_);
        if (owns_material_data_buffer_ && global_material_data_buffer_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(global_material_data_buffer_);
        }
        global_material_data_buffer_ = rhi::handles::INVALID_RESOURCE;
        owns_material_data_buffer_ = false;
        
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

        // GBuffer texture cleanup (albedo is same as final_color_texture_, already destroyed above)
        if (gbuffer_normal_texture_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyTexture(gbuffer_normal_texture_);
            gbuffer_normal_texture_ = rhi::handles::INVALID_RESOURCE;
        }
        if (gbuffer_orm_texture_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyTexture(gbuffer_orm_texture_);
            gbuffer_orm_texture_ = rhi::handles::INVALID_RESOURCE;
        }
        if (gbuffer_velocity_texture_ != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyTexture(gbuffer_velocity_texture_);
            gbuffer_velocity_texture_ = rhi::handles::INVALID_RESOURCE;
        }
        // Reset albedo handle (actual resource already destroyed via final_color_texture_)
        gbuffer_albedo_texture_ = rhi::handles::INVALID_RESOURCE;

        // 🎨 Cleanup texture arrays and sampler
        if (albedo_texture_array_ != rhi::handles::INVALID_RESOURCE) device_->DestroyTexture(albedo_texture_array_);
        if (normal_texture_array_ != rhi::handles::INVALID_RESOURCE) device_->DestroyTexture(normal_texture_array_);
        if (orm_texture_array_ != rhi::handles::INVALID_RESOURCE) device_->DestroyTexture(orm_texture_array_);
        if (texture_sampler_ != rhi::handles::INVALID_SAMPLER) device_->DestroySampler(texture_sampler_);

        // T4.6.5 part 20: resolve pipeline resources
        if (resolve_cb_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(resolve_cb_);
        if (resolve_output_texture_ != rhi::handles::INVALID_RESOURCE) device_->DestroyTexture(resolve_output_texture_);
        if (resolve_sampler_ != rhi::handles::INVALID_SAMPLER) device_->DestroySampler(resolve_sampler_);
        resolve_descriptor_written_ = false;

        // T4.6.5 part 22: Stage2 visibility pipeline resources.
        if (visibility_cb_ != rhi::handles::INVALID_RESOURCE) device_->DestroyBuffer(visibility_cb_);
        if (visibility_descriptor_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
            device_->DestroyDescriptorSet(visibility_descriptor_set_);
        }
        if (visibility_descriptor_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
            device_->DestroyDescriptorSetLayout(visibility_descriptor_set_layout_);
        }
        visibility_descriptor_written_ = false;

        // Cleanup shadow resources
        ShutdownShadowResources();
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
    // T4.6.5 part 22: RenderTarget for Stage2 color attachment + CopySource for test readback.
    visibilityDesc.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource
                         | rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySource;

    visibility_buffer_ = device_->CreateTexture(visibilityDesc);
    if (visibility_buffer_ == rhi::handles::INVALID_RESOURCE) {
        // Visibility buffer is only consumed by Stage2_VisibilityBuffer, which is
        // bypassed in the meshlet-draw path (mirrors Metal test, which also
        // disables it). On Dawn the R32_UInt storage texture format isn't always
        // available, so log + continue rather than failing the whole pipeline.
        std::cerr << "[GPUDrivenDrawPipeline] Visibility buffer unavailable (skipped — "
                     "Stage2 is bypassed in this path)" << std::endl;
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
        constantsDesc.size = 352; // sizeof(DrawConstants) with prev matrices
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

    // 🎨 NEW: Create texture sampler
    rhi::SamplerDesc samplerDesc{};
    samplerDesc.minFilter = rhi::FilterMode::Linear;
    samplerDesc.magFilter = rhi::FilterMode::Linear;
    samplerDesc.mipFilter = rhi::FilterMode::Linear;
    samplerDesc.addressU = rhi::TextureAddressMode::Wrap;
    samplerDesc.addressV = rhi::TextureAddressMode::Wrap;
    samplerDesc.addressW = rhi::TextureAddressMode::Wrap;
    samplerDesc.maxLod = 100.0f;
    texture_sampler_ = device_->CreateSampler(samplerDesc);

    if (texture_sampler_ == rhi::handles::INVALID_SAMPLER) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create texture sampler" << std::endl;
        return false;
    }

    // 🎨 NEW: Create placeholder texture arrays (1x1 white textures for now)
    // TODO: Replace with real texture arrays from material data
    rhi::TextureDesc textureArrayDesc{};
    textureArrayDesc.size = { 1, 1, 1 };  // 1x1 texture
    textureArrayDesc.arraySize = 1;        // Array size = 1 (will be increased when real textures are loaded)
    textureArrayDesc.mipLevels = 1;
    textureArrayDesc.format = rhi::DataFormat::RGBA8_UNorm;
    textureArrayDesc.type = rhi::TextureType::Texture2DArray;
    textureArrayDesc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopyDest;
    textureArrayDesc.memoryUsage = rhi::GPUMemoryUsage::Static;

    // Create albedo texture array
    albedo_texture_array_ = device_->CreateTexture(textureArrayDesc);
    if (albedo_texture_array_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create albedo texture array" << std::endl;
        return false;
    }

    // Create normal texture array
    normal_texture_array_ = device_->CreateTexture(textureArrayDesc);
    if (normal_texture_array_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create normal texture array" << std::endl;
        return false;
    }

    // Create ORM texture array
    orm_texture_array_ = device_->CreateTexture(textureArrayDesc);
    if (orm_texture_array_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create ORM texture array" << std::endl;
        return false;
    }

    // 🎨 NOTE: Texture arrays are currently placeholder (1x1)
    // TODO: Initialize with white color or upload actual texture data
    // For now, textures will use default values

    // Material data buffer: if no caller wired one up via SetMaterialDataBuffer,
    // allocate a small dummy SSBO so binding 9 isn't VK_NULL_HANDLE. Without this,
    // vkUpdateDescriptorSets + vkCmdDrawIndirect both fire validation errors
    // (VUID-VkWriteDescriptorSet-descriptorType-00330 + VUID-vkCmdDraw-None-02749).
    // The pipeline doesn't sample material data when materials aren't bound, so a
    // 16-byte zeroed buffer is sufficient.
    if (global_material_data_buffer_ == rhi::handles::INVALID_RESOURCE) {
        rhi::BufferDesc matDesc{};
        matDesc.size = 16;
        matDesc.type = rhi::BufferType::Structured;
        matDesc.bindFlags = static_cast<u32>(rhi::BufferUsageFlags::Storage);
        matDesc.memoryUsage = rhi::GPUMemoryUsage::Static;
        matDesc.name = "GPUDrivenDrawPipeline_dummy_material_data";
        global_material_data_buffer_ = device_->CreateBuffer(matDesc);
        if (global_material_data_buffer_ != rhi::handles::INVALID_RESOURCE) {
            owns_material_data_buffer_ = true;
        } else {
            std::cerr << "[GPUDrivenDrawPipeline] Failed to create dummy material data buffer" << std::endl;
            return false;
        }
    }

    // std::cout << "[GPUDrivenDrawPipeline] Resources created successfully" << std::endl;
    return true;
}

bool GPUDrivenDrawPipeline::CreateRenderPasses() {
    // std::cout << "[GPUDrivenDrawPipeline] Creating render passes..." << std::endl;

    // T4.6.5 part 22.2: Create final_depth_texture_ FIRST so both
    // visibility_render_pass_ (Stage2) and final_render_pass_ (Stage3) can
    // share it as their depth attachment. Previously this was created later
    // (between GBuffer textures and final_render_pass_), which left
    // visibility_render_pass_ with a texture-less depth attachment.
    rhi::TextureDesc finalDepthDesc{};
    finalDepthDesc.size = { visibility_config_.width, visibility_config_.height, 1 };
    finalDepthDesc.format = rhi::DataFormat::D32_Float;
    // DepthStencil = render target for depth. ShaderResource = HZB sampling.
    // (Do NOT add TextureUsage::RenderTarget — that maps to COLOR_ATTACHMENT_BIT
    // in Vulkan, which is illegal on a D32_SFLOAT image. Metal silently accepts
    // the redundant bit; Vulkan rejects with VUID-VkImageCreateInfo-imageCreateMaxMipLevels-02251.)
    finalDepthDesc.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;

    final_depth_texture_ = device_->CreateTexture(finalDepthDesc);
    if (final_depth_texture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create final depth texture" << std::endl;
        return false;
    }

    // Create Visibility Buffer Render Pass — only when the visibility buffer
    // texture was actually created. On Dawn the R32_UInt storage format isn't
    // always supported (see CreateResources), and Stage2 is bypassed in the
    // meshlet-draw path so the render pass isn't needed.
    if (visibility_buffer_ != rhi::handles::INVALID_RESOURCE) {
        rhi::RenderPassDesc visibilityPassDesc{};

        // Color attachment - visibility buffer (R32_UINT format)
        rhi::RenderPassDesc::Attachment visibilityColorAttachment{};
        visibilityColorAttachment.texture = visibility_buffer_;
        visibilityColorAttachment.format = rhi::DataFormat::R32_UInt;
        visibilityColorAttachment.loadOp = rhi::LoadAction::Clear;
        visibilityColorAttachment.storeOp = rhi::StoreAction::Store;
        visibilityColorAttachment.clearValue.color = math::v4{0.0f, 0.0f, 0.0f, 0.0f}; // Clear to 0 (no visible geometry)

        visibilityPassDesc.colorAttachments.push_back(visibilityColorAttachment);

        // Depth attachment — T4.6.5 part 22.2: share final_depth_texture_ with
        // Stage3 so both stages share the same depth buffer convention.
        rhi::RenderPassDesc::Attachment depthAttachment{};
        depthAttachment.texture = final_depth_texture_;
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
    }

    // std::cout << "[GPUDrivenDrawPipeline] Visibility render pass created successfully" << std::endl;

    // Create Final Render Pass (for Stage 3)
    // NEW: Create GBuffer render targets (4 color attachments)
    // CopySource|CopyDest are required so the test can blit meshlet GBuffer
    // outputs into its own GBuffer textures for the deferred lighting pass.
    rhi::TextureDesc gbufferDesc{};
    gbufferDesc.size = { visibility_config_.width, visibility_config_.height, 1 };
    gbufferDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource |
                        rhi::TextureUsage::CopySource | rhi::TextureUsage::CopyDest;

    // GBuffer Target 0: Albedo (BGRA8)
    gbufferDesc.format = rhi::DataFormat::BGRA8_UNorm;
    gbuffer_albedo_texture_ = device_->CreateTexture(gbufferDesc);
    if (gbuffer_albedo_texture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create GBuffer Albedo texture" << std::endl;
        return false;
    }

    // GBuffer Target 1: Normal (BGRA8)
    gbufferDesc.format = rhi::DataFormat::BGRA8_UNorm;
    gbuffer_normal_texture_ = device_->CreateTexture(gbufferDesc);
    if (gbuffer_normal_texture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create GBuffer Normal texture" << std::endl;
        return false;
    }

    // GBuffer Target 2: ORM (BGRA8)
    gbufferDesc.format = rhi::DataFormat::BGRA8_UNorm;
    gbuffer_orm_texture_ = device_->CreateTexture(gbufferDesc);
    if (gbuffer_orm_texture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create GBuffer ORM texture" << std::endl;
        return false;
    }

    // GBuffer Target 3: Velocity — RG16_Float (signed, range [-0.5, 0.5]).
    // RG16_UNorm isn't in WebGPU's core format list; the velocity vector can be
    // negative so a float format is the right call anyway.
    gbufferDesc.format = rhi::DataFormat::RG16_Float;
    gbuffer_velocity_texture_ = device_->CreateTexture(gbufferDesc);
    if (gbuffer_velocity_texture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[GPUDrivenDrawPipeline] Failed to create GBuffer Velocity texture" << std::endl;
        return false;
    }

    // Keep final_color_texture_ for compatibility (use albedo as output)
    final_color_texture_ = gbuffer_albedo_texture_;

    // T4.6.5 part 22.2: final_depth_texture_ is now created at the top of
    // CreateRenderPasses (before visibility_render_pass_) so Stage2 + Stage3
    // can share it. Skip the duplicate creation here.

    rhi::RenderPassDesc finalPassDesc{};

    // 🔥 NEW: GBuffer color attachments
    // Attachment 0: Albedo
    rhi::RenderPassDesc::Attachment albedoAttachment{};
    albedoAttachment.texture = gbuffer_albedo_texture_;
    albedoAttachment.format = rhi::DataFormat::BGRA8_UNorm;
    albedoAttachment.loadOp = rhi::LoadAction::Clear;
    albedoAttachment.storeOp = rhi::StoreAction::Store;
    albedoAttachment.clearValue.color = math::v4{0.0f, 0.0f, 0.0f, 1.0f};
    finalPassDesc.colorAttachments.push_back(albedoAttachment);

    // Attachment 1: Normal
    rhi::RenderPassDesc::Attachment normalAttachment{};
    normalAttachment.texture = gbuffer_normal_texture_;
    normalAttachment.format = rhi::DataFormat::BGRA8_UNorm;
    normalAttachment.loadOp = rhi::LoadAction::Clear;
    normalAttachment.storeOp = rhi::StoreAction::Store;
    normalAttachment.clearValue.color = math::v4{0.5f, 0.5f, 1.0f, 1.0f}; // (0,0,1) in [0,1]
    finalPassDesc.colorAttachments.push_back(normalAttachment);

    // Attachment 2: ORM
    rhi::RenderPassDesc::Attachment ormAttachment{};
    ormAttachment.texture = gbuffer_orm_texture_;
    ormAttachment.format = rhi::DataFormat::BGRA8_UNorm;
    ormAttachment.loadOp = rhi::LoadAction::Clear;
    ormAttachment.storeOp = rhi::StoreAction::Store;
    ormAttachment.clearValue.color = math::v4{1.0f, 0.8f, 0.0f, 1.0f}; // AO=1, Roughness=0.8, Metallic=0
    finalPassDesc.colorAttachments.push_back(ormAttachment);

    // Attachment 3: Velocity
    rhi::RenderPassDesc::Attachment velocityAttachment{};
    velocityAttachment.texture = gbuffer_velocity_texture_;
    velocityAttachment.format = rhi::DataFormat::RG16_Float;
    velocityAttachment.loadOp = rhi::LoadAction::Clear;
    velocityAttachment.storeOp = rhi::StoreAction::Store;
    velocityAttachment.clearValue.color = math::v4{0.0f, 0.0f, 0.0f, 0.0f};
    finalPassDesc.colorAttachments.push_back(velocityAttachment);

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
//    std::cout << "[GPUDrivenDrawPipeline] Creating minimal test pipeline..." << std::endl;
    auto minimalVertexShaderCode = LoadShaderBytecode("MinimalTest", "minimal_test_vertex", device_);
    auto minimalFragmentShaderCode = LoadShaderBytecode("MinimalTest", "minimal_test_fragment", device_);

    if (!minimalVertexShaderCode.empty() && !minimalFragmentShaderCode.empty()) {
//        std::cout << "[GPUDrivenDrawPipeline] Minimal test shaders loaded successfully" << std::endl;

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
//            std::cout << "[GPUDrivenDrawPipeline] Minimal test shaders created successfully" << std::endl;

            // Create minimal pipeline layout (no descriptor sets needed!)
            rhi::PipelineLayoutDesc minimalLayoutDesc{};
            minimalLayoutDesc.setLayoutCount = 0;
            minimalLayoutDesc.setLayouts = nullptr;

            rhi::PipelineLayoutHandle minimalLayout = device_->CreatePipelineLayout(minimalLayoutDesc);
            if (minimalLayout != rhi::handles::INVALID_PIPELINE_LAYOUT) {
//                std::cout << "[GPUDrivenDrawPipeline] Minimal pipeline layout created" << std::endl;

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
//                    std::cout << "[GPUDrivenDrawPipeline] MINIMAL TEST PIPELINE CREATED SUCCESSFULLY!" << std::endl;
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
    auto binningShaderCode = LoadShaderBytecode("ClusterBinning", "cluster_binning_kernel", device_);
    if (!binningShaderCode.empty()) {
        rhi::ShaderHandle binningShader = device_->CreateShader(
            binningShaderCode.data(),
            binningShaderCode.size(),
            rhi::ShaderStage::Compute,
            "cluster_binning_kernel"
        );

        if (binningShader != rhi::handles::INVALID_SHADER) {
            // T4.6.5 part 17.2: Vulkan validation rejects pipelines whose SPIR-V
            // uses descriptors not declared in the layout. Metal is permissive —
            // empty PipelineLayoutDesc still allows buffer(N) binding at runtime.
            // Build a 4-binding DSL (clusters/config/bins/bin_counter) on Vulkan
            // to match ClusterBinning.comp.
            rhi::PipelineLayoutHandle binningLayout = rhi::handles::INVALID_PIPELINE_LAYOUT;
            const bool isVulkan = (device_->GetPlatform() == rhi::RHIPlatform::Vulkan);
            if (isVulkan) {
                rhi::DescriptorSetLayoutBinding binningBindings[4]{};
                binningBindings[0].binding = 0;
                binningBindings[0].descriptorType = rhi::DescriptorType::StorageBuffer;
                binningBindings[0].descriptorCount = 1;
                binningBindings[0].stageFlags = rhi::ShaderStage::Compute;
                binningBindings[1].binding = 1;
                binningBindings[1].descriptorType = rhi::DescriptorType::UniformBuffer;
                binningBindings[1].descriptorCount = 1;
                binningBindings[1].stageFlags = rhi::ShaderStage::Compute;
                binningBindings[2].binding = 2;
                binningBindings[2].descriptorType = rhi::DescriptorType::StorageBuffer;
                binningBindings[2].descriptorCount = 1;
                binningBindings[2].stageFlags = rhi::ShaderStage::Compute;
                binningBindings[3].binding = 3;
                binningBindings[3].descriptorType = rhi::DescriptorType::StorageBuffer;
                binningBindings[3].descriptorCount = 1;
                binningBindings[3].stageFlags = rhi::ShaderStage::Compute;

                rhi::DescriptorSetLayoutDesc dslDesc{};
                dslDesc.bindingCount = 4;
                dslDesc.bindings = binningBindings;
                rhi::DescriptorSetLayoutHandle dsl = device_->CreateDescriptorSetLayout(dslDesc);

                rhi::PipelineLayoutDesc plDesc{};
                plDesc.setLayoutCount = 1;
                plDesc.setLayouts = &dsl;
                binningLayout = device_->CreatePipelineLayout(plDesc);
                // DSL lifecycle: short-lived; safe to release after layout built.
                device_->DestroyDescriptorSetLayout(dsl);
            }

            rhi::ComputePipelineDesc binningPipelineDesc{};
            binningPipelineDesc.computeShader = binningShader;
            binningPipelineDesc.threadGroupSize = {64, 1, 1};
            if (isVulkan) binningPipelineDesc.layout = binningLayout;

            binning_pipeline_ = device_->CreateComputePipeline(binningPipelineDesc);
            if (binning_pipeline_ != rhi::handles::INVALID_PIPELINE) {
                // std::cout << "[GPUDrivenDrawPipeline] Cluster Binning pipeline created successfully" << std::endl;
            } else {
                std::cerr << "[GPUDrivenDrawPipeline] Failed to create Cluster Binning pipeline" << std::endl;
            }
            if (isVulkan) device_->DestroyPipelineLayout(binningLayout);
        }
    } else {
        // std::cout << "[GPUDrivenDrawPipeline] Cluster Binning shader not found, using CPU fallback" << std::endl;
    }

    // Load Visibility Buffer shaders and create graphics pipeline
    auto visibilityVertexShaderCode = LoadShaderBytecode("VisibilityBuffer", "visibility_vertex_shader", device_);
    auto visibilityFragmentShaderCode = LoadShaderBytecode("VisibilityBuffer", "visibility_fragment_shader", device_);

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
            // T4.6.5 part 22.1: Vulkan validation rejects pipelines whose SPIR-V
            // uses descriptors not declared in the layout. Metal is permissive.
            // Build an 8-binding DSL matching the rewritten VisibilityBuffer.vert
            // (meshlet-aware vertex pulling, mirrors Stage3).
            // T4.6.5 part 22.4: Retain the DSL as a member so we can allocate
            // descriptor sets from it after pipeline creation. Was destroyed
            // immediately before, which made it impossible to write Stage2 descriptors.
            const bool isVulkan = (device_->GetPlatform() == rhi::RHIPlatform::Vulkan);
            if (isVulkan) {
                rhi::DescriptorSetLayoutBinding visBindings[8]{};
                visBindings[0].binding = 0;  // UBO  DrawConstants
                visBindings[0].descriptorType = rhi::DescriptorType::UniformBuffer;
                visBindings[0].descriptorCount = 1;
                visBindings[0].stageFlags = rhi::ShaderStage::Vertex | rhi::ShaderStage::Pixel;
                visBindings[1].binding = 1;  // SSBO meshlets
                visBindings[1].descriptorType = rhi::DescriptorType::StorageBuffer;
                visBindings[1].descriptorCount = 1;
                visBindings[1].stageFlags = rhi::ShaderStage::Vertex;
                visBindings[2].binding = 2;  // SSBO meshlet_vertices
                visBindings[2].descriptorType = rhi::DescriptorType::StorageBuffer;
                visBindings[2].descriptorCount = 1;
                visBindings[2].stageFlags = rhi::ShaderStage::Vertex;
                visBindings[3].binding = 3;  // SSBO meshlet_triangles
                visBindings[3].descriptorType = rhi::DescriptorType::StorageBuffer;
                visBindings[3].descriptorCount = 1;
                visBindings[3].stageFlags = rhi::ShaderStage::Vertex;
                visBindings[4].binding = 4;  // SSBO positions (tightly packed float[])
                visBindings[4].descriptorType = rhi::DescriptorType::StorageBuffer;
                visBindings[4].descriptorCount = 1;
                visBindings[4].stageFlags = rhi::ShaderStage::Vertex;
                visBindings[5].binding = 5;  // SSBO compact_cluster_ids (visible cluster list)
                visBindings[5].descriptorType = rhi::DescriptorType::StorageBuffer;
                visBindings[5].descriptorCount = 1;
                visBindings[5].stageFlags = rhi::ShaderStage::Vertex;
                visBindings[6].binding = 6;  // SSBO cluster_map (uvec4 per entry)
                visBindings[6].descriptorType = rhi::DescriptorType::StorageBuffer;
                visBindings[6].descriptorCount = 1;
                visBindings[6].stageFlags = rhi::ShaderStage::Vertex;
                visBindings[7].binding = 7;  // SSBO instance_data (192B per entry)
                visBindings[7].descriptorType = rhi::DescriptorType::StorageBuffer;
                visBindings[7].descriptorCount = 1;
                visBindings[7].stageFlags = rhi::ShaderStage::Vertex;

                rhi::DescriptorSetLayoutDesc dslDesc{};
                dslDesc.bindingCount = 8;
                dslDesc.bindings = visBindings;
                visibility_descriptor_set_layout_ = device_->CreateDescriptorSetLayout(dslDesc);
            }

            rhi::PipelineLayoutDesc visibilityLayoutDesc{};
            if (isVulkan && visibility_descriptor_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
                visibilityLayoutDesc.setLayoutCount = 1;
                visibilityLayoutDesc.setLayouts = &visibility_descriptor_set_layout_;
            }
            visibility_pipeline_layout_ = device_->CreatePipelineLayout(visibilityLayoutDesc);
            // T4.6.5 part 22.4: Do NOT destroy visibility_descriptor_set_layout_ here.
            // It must remain alive for descriptor set allocation below + each frame.

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
                    // T4.6.5 part 22.4: Allocate per-frame CB + descriptor set for Stage2.
                    if (isVulkan && visibility_descriptor_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
                        rhi::BufferDesc visCbDesc{};
                        visCbDesc.size = 256;  // DrawConstants is 208B (3 m4x4 + 4 u32); 256B padded
                        visCbDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Uniform | rhi::BufferUsageFlags::TransferDst);
                        visCbDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
                        visibility_cb_ = device_->CreateBuffer(visCbDesc);

                        visibility_descriptor_set_ = device_->CreateDescriptorSet({ visibility_descriptor_set_layout_ });
                        if (visibility_descriptor_set_ == rhi::handles::INVALID_DESCRIPTOR_SET) {
                            std::cerr << "[GPUDrivenDrawPipeline] Failed to allocate visibility descriptor set" << std::endl;
                        }
                    }
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
    auto gpuDrawVertexShaderCode = LoadShaderBytecode("GPUDrivenDraw", "gpu_driven_vertex_shader", device_);
    auto gpuDrawFragmentShaderCode = LoadShaderBytecode("GPUDrivenDraw", "gpu_driven_fragment_shader", device_);

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
            rhi::DescriptorSetLayoutBinding gpuDrawBindings[14];  // Updated to 9 for element buffer
            gpuDrawBindings[0].binding = 0;
            gpuDrawBindings[0].descriptorType = rhi::DescriptorType::UniformBufferDynamic;
            gpuDrawBindings[0].descriptorCount = 1;
            gpuDrawBindings[0].stageFlags = rhi::ShaderStage::Vertex | rhi::ShaderStage::Pixel;

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

            // 🔥 NEW: Element buffer binding
            gpuDrawBindings[8].binding = 8;
            gpuDrawBindings[8].descriptorType = rhi::DescriptorType::StorageBuffer;
            gpuDrawBindings[8].descriptorCount = 1;
            gpuDrawBindings[8].stageFlags = rhi::ShaderStage::Vertex;

            // 🎨 NEW: Material data buffer (vertex + fragment shader)
            // Both stages need access: vertex reads materialID, fragment reads full material data
            gpuDrawBindings[9].binding = 9;
            gpuDrawBindings[9].descriptorType = rhi::DescriptorType::StorageBuffer;
            gpuDrawBindings[9].descriptorCount = 1;
            gpuDrawBindings[9].stageFlags = rhi::ShaderStage::Vertex | rhi::ShaderStage::Pixel;

            // 🎨 NEW: Texture arrays (fragment shader) — shader declares texture_2d_array<f32>,
            // so isArray must be true so DawnDescriptorSetLayout emits WGPUTextureViewDimension_2DArray.
            gpuDrawBindings[10].binding = 10;
            gpuDrawBindings[10].descriptorType = rhi::DescriptorType::SampledImage;
            gpuDrawBindings[10].descriptorCount = 1;
            gpuDrawBindings[10].stageFlags = rhi::ShaderStage::Pixel;
            gpuDrawBindings[10].isArray = true;

            gpuDrawBindings[11].binding = 11;
            gpuDrawBindings[11].descriptorType = rhi::DescriptorType::SampledImage;
            gpuDrawBindings[11].descriptorCount = 1;
            gpuDrawBindings[11].stageFlags = rhi::ShaderStage::Pixel;
            gpuDrawBindings[11].isArray = true;

            gpuDrawBindings[12].binding = 12;
            gpuDrawBindings[12].descriptorType = rhi::DescriptorType::SampledImage;
            gpuDrawBindings[12].descriptorCount = 1;
            gpuDrawBindings[12].stageFlags = rhi::ShaderStage::Pixel;
            gpuDrawBindings[12].isArray = true;

            // 🎨 NEW: Texture sampler (fragment shader)
            gpuDrawBindings[13].binding = 13;
            gpuDrawBindings[13].descriptorType = rhi::DescriptorType::Sampler;
            gpuDrawBindings[13].descriptorCount = 1;
            gpuDrawBindings[13].stageFlags = rhi::ShaderStage::Pixel;

            rhi::DescriptorSetLayoutDesc gpuDrawLayoutDesc{};
            gpuDrawLayoutDesc.bindingCount = 14;  // Updated from 9 to 14
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

            // 🔥 NEW: GBuffer render targets (4 outputs)
            gpuDrawPipelineDesc.renderTargetCount = 4;
            gpuDrawPipelineDesc.renderTargetFormats[0] = rhi::DataFormat::BGRA8_UNorm; // Albedo
            gpuDrawPipelineDesc.renderTargetFormats[1] = rhi::DataFormat::BGRA8_UNorm; // Normal (packed)
            gpuDrawPipelineDesc.renderTargetFormats[2] = rhi::DataFormat::BGRA8_UNorm; // ORM
            gpuDrawPipelineDesc.renderTargetFormats[3] = rhi::DataFormat::RG16_Float;  // Velocity

            // DEPTH SETTINGS - CRITICAL FOR PROPER RENDERING
            // Based on UE5 Nanite depth rendering practices
            // Relaxed depth testing to ensure geometry visibility
            gpuDrawPipelineDesc.depthStencilFormat = rhi::DataFormat::D32_Float;
            gpuDrawPipelineDesc.enableDepthTest = true;
            gpuDrawPipelineDesc.enableDepthWrite = true;
            gpuDrawPipelineDesc.depthFunc = rhi::ComparisonFunc::Less;

            // Match Metal TestNaniteStreamingPipeline:1435 — double-sided so walls/
            // banners/floor visible from any angle. Default CullMode::Back culls
            // back-facing triangles, hiding walls viewed from inside the atrium.
            gpuDrawPipelineDesc.cullMode = rhi::CullMode::None;

            // No blending for opaque geometry
            gpuDrawPipelineDesc.enableBlend = false;

            gpuDrawPipelineDesc.cullMode = rhi::CullMode::None; // Keep disabled - meshlet winding may vary

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

    auto resolveShaderCode = LoadShaderBytecode("VisibilityBufferResolve", "ComputeMain", device_);

    if (resolveShaderCode.empty()) {
        // VisibilityBufferResolve is part of the visibility-buffer path, which is bypassed
        // in this port (mirrors the Metal test where it's disabled). Skip resolve pipeline
        // creation and return — the main gpu_driven_draw_pipeline_ already handles rendering.
        return true;
    }
    
    // std::cout << "[GPUDrivenDrawPipeline] Creating Visibility Buffer Resolve compute pipeline..." << std::endl;
    
    rhi::DescriptorSetLayoutBinding resolveBindings[5];
    // T4.6.5 part 17.4: Vulkan unified buffer+texture namespace. Mirror the
    // Metal layout (3 textures + 2 buffers) into a single binding sequence:
    //   0: visibility_buffer (usampler2D / R32UINT combined image sampler)
    //   1: depth_buffer (sampler2D / D32 combined image sampler)
    //   2: output_color (storage image RGBA8, write-only)
    //   3: DrawConstants (UBO)
    //   4: ClusterData[] (SSBO)
    resolveBindings[0].binding = 0;
    resolveBindings[0].descriptorType = rhi::DescriptorType::CombinedImageSampler;
    resolveBindings[0].descriptorCount = 1;
    resolveBindings[0].stageFlags = rhi::ShaderStage::Compute;
    resolveBindings[1].binding = 1;
    resolveBindings[1].descriptorType = rhi::DescriptorType::CombinedImageSampler;
    resolveBindings[1].descriptorCount = 1;
    resolveBindings[1].stageFlags = rhi::ShaderStage::Compute;
    resolveBindings[2].binding = 2;
    resolveBindings[2].descriptorType = rhi::DescriptorType::StorageImage;
    resolveBindings[2].descriptorCount = 1;
    resolveBindings[2].stageFlags = rhi::ShaderStage::Compute;
    resolveBindings[3].binding = 3;
    resolveBindings[3].descriptorType = rhi::DescriptorType::UniformBuffer;
    resolveBindings[3].descriptorCount = 1;
    resolveBindings[3].stageFlags = rhi::ShaderStage::Compute;
    resolveBindings[4].binding = 4;
    resolveBindings[4].descriptorType = rhi::DescriptorType::StorageBuffer;
    resolveBindings[4].descriptorCount = 1;
    resolveBindings[4].stageFlags = rhi::ShaderStage::Compute;
    
    rhi::DescriptorSetLayoutDesc resolveLayoutDesc{};
    resolveLayoutDesc.bindingCount = 5;
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
    // T4.6.5 part 20: R32_UINT visibility buffer has no SAMPLED_IMAGE_FILTER_LINEAR_BIT format
    // feature — mip filter must also be Nearest or vkCmdDispatch validation fires.
    resolveSamplerDesc.mipFilter = rhi::FilterMode::Nearest;
    resolveSamplerDesc.addressU = rhi::TextureAddressMode::Clamp;
    resolveSamplerDesc.addressV = rhi::TextureAddressMode::Clamp;
    // T4.6.5 part 20: MoltenVK portability requires compareEnable=FALSE on non-comparison samplers.
    // SamplerDesc defaults comparisonFunc=Always which VulkanSampler treats as compareEnable=TRUE
    // (VUID-VkDescriptorImageInfo-mutableComparisonSamplers-04450).
    resolveSamplerDesc.comparisonFunc = rhi::ComparisonFunc::Never;
    resolve_sampler_ = device_->CreateSampler(resolveSamplerDesc);

    rhi::TextureDesc resolveOutputDesc{};
    resolveOutputDesc.size = { visibility_config_.width, visibility_config_.height, 1 };
    resolveOutputDesc.format = rhi::DataFormat::RGBA8_UNorm;
    // T4.6.5 part 20: UnorderedAccess for imageStore + CopySource for test readback.
    resolveOutputDesc.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopySource;
    resolve_output_texture_ = device_->CreateTexture(resolveOutputDesc);

    // T4.6.5 part 20: per-frame DrawConstants CB for the resolve compute shader.
    // Layout: 3 m4x4 (192B) + 4 u32 (16B) = 208 bytes; pad to 256 for 16-byte alignment safety.
    rhi::BufferDesc resolveCbDesc{};
    resolveCbDesc.size = 256;
    resolveCbDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Uniform | rhi::BufferUsageFlags::TransferDst);
    resolveCbDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
    resolve_cb_ = device_->CreateBuffer(resolveCbDesc);

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
//        std::cout << "[GPUDrivenDrawPipeline] Creating GPU Draw descriptor sets..." << std::endl;
//        std::cout << "  draw_descriptor_layout_: " << draw_descriptor_layout_ << std::endl;
//        std::cout << "  frame_resources_.size(): " << frame_resources_.size() << std::endl;

        // Allocate 3 descriptor sets for triple buffering
        for (u32 i = 0; i < 3; ++i) {
            if (i < frame_resources_.size()) {
                frame_resources_[i].global_draw_descriptor_set = device_->CreateDescriptorSet({draw_descriptor_layout_});
//                std::cout << "  Frame " << i << " descriptor set: " << frame_resources_[i].global_draw_descriptor_set << std::endl;
                if (frame_resources_[i].global_draw_descriptor_set == rhi::handles::INVALID_DESCRIPTOR_SET) {
                    std::cerr << "[GPUDrivenDrawPipeline] Failed to create descriptor set for frame " << i << std::endl;
                    return false;
                }
//                std::cout << "  Frame " << i << " camera buffer: " << frame_resources_[i].camera_constants_buffer << std::endl;
            }
        }
//        std::cout << "[GPUDrivenDrawPipeline] GPU Draw descriptor sets created successfully" << std::endl;
    }

    return true;
}

void GPUDrivenDrawPipeline::UpdateGeometryData(const RenderSceneSnapshot& scene_snapshot) {
    if (total_meshlet_count_ > 0) {
        // Already built global buffers
        return;
    }

    const auto& instanceData = scene_snapshot.GetInstanceData();
    std::set<id::id_type> processedGeometries;

    // std::cout << "[GPUDrivenDrawPipeline] Building Global Geometry Buffers..." << std::endl;

    // 1. Calculate total sizes
    struct PackedV3 { float x, y, z; };

    u32 totalMeshlets = 0;
    u32 totalVertices = 0;
    u32 totalTriangles = 0; // total meshlet triangle indices (bytes or count?) - count
    u32 totalPositions = 0;
    u32 totalElements = 0; // 🔥 NEW: For element buffer (Normal, Tangent, UV)

    // Use packed float3 for positions to match Metal's packed_float3 and typical disk format

    // Cache synthesized meshlet data per-geometry so the fill loop below can
    // re-use it without recomputing. Sponza.model has no MSHL sections — we
    // generate meshlets on the fly from the index buffer.
    std::unordered_map<id::id_type, SynthesizedMeshlets> synthesized;

    for (const auto& instance : instanceData) {
        if (processedGeometries.count(instance.geometry_id)) continue;

        graphics::rhi::RHIMeshAsset meshAsset;
        if (primal::content::get_rhi_mesh_asset(instance.geometry_id, meshAsset)) {
            const bool needsSynthesis = meshAsset.meshlets.empty();
            if (needsSynthesis) {
                SynthesizedMeshlets& synth = synthesized[instance.geometry_id];
                SynthesizeMeshlets(meshAsset, synth);
                totalMeshlets += (u32)synth.meshlets.size();
                totalVertices += (u32)synth.meshlet_vertices.size();
                totalTriangles += (u32)synth.meshlet_triangles.size();
            } else {
                totalMeshlets += (u32)meshAsset.meshlets.size();
                totalVertices += (u32)meshAsset.meshlet_vertices.size();
                totalTriangles += (u32)meshAsset.meshlet_triangles.size();
            }

            // RHIMeshAsset stores positions as bytes
            // Assuming the asset data is tightly packed float3 (12 bytes)
            totalPositions += (u32)(meshAsset.position_buffer.size() / sizeof(PackedV3));

            // 🔥 NEW: Element buffer (Normal, Tangent, UV)
            // Element buffer size should match position buffer size (one element per vertex)
            totalElements += (u32)(meshAsset.position_buffer.size() / sizeof(PackedV3));

            processedGeometries.insert(instance.geometry_id);
        } else {
            // Asset resolution failed for this instance — skip silently,
            // the geometry simply won't appear in the merged buffers.
        }
    }

    if (totalMeshlets == 0) {
        return;
    }

    std::cout << "[GPUDrivenDrawPipeline] Building Global Buffers:" << std::endl;
    std::cout << "  Meshlets: " << totalMeshlets << std::endl;
    std::cout << "  Vertices: " << totalVertices << std::endl;
    std::cout << "  Triangles: " << totalTriangles << std::endl;
    std::cout << "  Positions: " << totalPositions << std::endl;
    std::cout << "  Elements: " << totalElements << std::endl;

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
    // WebGPU requires storage buffer sizes to be multiples of 4. Pad up.
    rhi::BufferDesc meshletTrianglesDesc{};
    const u32 triangleBytesPadded = (totalTriangles + 3u) & ~3u;
    meshletTrianglesDesc.size = triangleBytesPadded;
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

    // 🔥 NEW: Global Elements (Normal, Tangent, UV)
    // Each element is 20 bytes: ColorTSign(4) + Normal(4) + Tangent(4) + UV(8)
    // Matches TestParticleSponza VertexElement format (NO padding)
    rhi::BufferDesc elementDesc{};
    elementDesc.size = totalElements * 24; // 24 bytes per vertex element
    elementDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage);
    elementDesc.usage = rhi::GPUMemoryUsage::Dynamic;
    global_element_buffer_ = device_->CreateBuffer(elementDesc);

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

    // 🔥 Vertex elements (Normal, Tangent, UV) - 24 bytes per vertex
    // Uses padding to match traditional rendering format
    struct VertexElement {
        u32 colorTSign;       // 4 bytes
        u32 normal;           // 4 bytes (stored as uint, unpacked to vec2)
        u32 tangent;          // 4 bytes (stored as uint, unpacked to vec2)
        u32 padding;          // 4 bytes (padding for alignment)
        math::v2 uv;          // 8 bytes
        // Total: 4+4+4+4+8 = 24 bytes
    };
    utl::vector<VertexElement> mergedElements;
    mergedElements.reserve(totalElements);

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
        if (primal::content::get_rhi_mesh_asset(
                primal::content::get_rhi_mesh_id(instance.geometry_id), meshAsset)) {
            
            geometryToGlobalMeshletBase[instance.geometry_id] = currentMeshletOffset;
            
            // Copy Positions
            // Assume input is packed float3 (12 bytes)
            u32 posCount = (u32)(meshAsset.position_buffer.size() / sizeof(PackedV3));
            const PackedV3* posSrc = reinterpret_cast<const PackedV3*>(meshAsset.position_buffer.data());
            for(u32 i=0; i<posCount; ++i) mergedPositions.push_back(posSrc[i]);

            // Copy Elements (Normal, Tangent, UV)
            // Source is static_normal_texture (Geometry.h). Layout depends on
            // platform: Mac's simd::float2 has alignof=8 forcing sizeof=24,
            // other platforms sizeof=20. Either way the leading 12 bytes are
            //   [0..3]  u8 color[3] + u8 t_sign
            //   [4..7]  u16 normal[2]   (LE: low16 = normal[0]=X, high16 = normal[1]=Y)
            //   [8..11] u16 tangent[2]  (same layout as normal)
            // UV follows: 20-byte src → offset 12; 24-byte src → offset 16.
            //
            // Dest VertexElement packs normal as u32 with X in HIGH 16 and Y in
            // LOW 16 — that's what GPUDrivenDraw.wgsl::unpack_normal expects
            // (hi = (packed>>16)&0xFFFF → f.x; lo = packed&0xFFFF → f.y).
            //
            // The previous DIRECT COPY branch blitted the raw source u32 at
            // bytes 4-7, which on LE is (Y<<16)|X — silently swapping X/Y in
            // the shader. For a (0,1,0) floor normal that yields f≈(1,0), d>1
            // fallback fires, and the resulting tilted N breaks deferred light
            // direction. Field-by-field extraction is the only correct path.
            u32 elemCount = posCount;
            const u64 srcTotal = meshAsset.element_buffer.size();
            const u8* srcData = meshAsset.element_buffer.data();
            const u64 srcStride = (elemCount > 0) ? (srcTotal / elemCount) : 0;
            if (srcStride == 24 || srcStride == 20) {
                const u64 uvOffset = (srcStride == 24) ? 16 : 12;
                for(u32 i=0; i<elemCount; ++i) {
                    const u8* src = srcData + i * srcStride;
                    VertexElement dest{};
                    dest.colorTSign = (u32)src[0] | ((u32)src[1] << 8) |
                                      ((u32)src[2] << 16) | ((u32)src[3] << 24);
                    // memcpy avoids alignment UB — element_buffer.data() is
                    // only 1-byte aligned, so reinterpret_cast<u16*> is UB on
                    // strict-alignment ISAs even though x86/ARM tolerate it.
                    u16 n0, n1, t0, t1;
                    memcpy(&n0, src + 4, sizeof(u16));
                    memcpy(&n1, src + 6, sizeof(u16));
                    memcpy(&t0, src + 8, sizeof(u16));
                    memcpy(&t1, src + 10, sizeof(u16));
                    dest.normal = ((u32)n0 << 16) | (u32)n1;
                    dest.tangent = ((u32)t0 << 16) | (u32)t1;
                    dest.padding = 0;
                    memcpy(&dest.uv, src + uvOffset, sizeof(math::v2));
                    mergedElements.push_back(dest);
                }
            } else if (meshAsset.element_buffer.size() >= elemCount * 8) {
                // 8-byte source format (static_normal: color[3]+t_sign+normal[2]) - convert to 24-byte
                const u8* srcData = meshAsset.element_buffer.data();
                for(u32 i=0; i<elemCount; ++i) {
                    const u8* src = srcData + i * 8;
                    VertexElement dest{};
                    dest.colorTSign = (u32)src[0] | ((u32)src[1] << 8) | ((u32)src[2] << 16) | ((u32)src[3] << 24);
                    u16 n0 = *reinterpret_cast<const u16*>(src + 4);
                    u16 n1 = *reinterpret_cast<const u16*>(src + 6);
                    dest.normal = ((u32)n0 << 16) | (u32)n1;
                    dest.tangent = 0; // No tangent data
                    dest.padding = 0;
                    dest.uv = math::v2{0.0f, 0.0f};
                    mergedElements.push_back(dest);
                }
            } else {
                // Element buffer missing or wrong size - fill with defaults
                VertexElement defaultElem{};
                defaultElem.colorTSign = 0xFFFFFFFF;
                defaultElem.normal = 0x80008000; // {0, 0} → Z=1 (up)
                defaultElem.tangent = 0x80008000;
                defaultElem.uv = math::v2{0.0f, 0.0f};
                for(u32 i=0; i<elemCount; ++i) mergedElements.push_back(defaultElem);
            }

            // Copy MeshletVertices (Global Vertex Indices)
            // Need to offset by currentPositionOffset
            const bool hasSynth = synthesized.find(instance.geometry_id) != synthesized.end();
            if (hasSynth) {
                const auto& synth = synthesized[instance.geometry_id];
                for (u32 idx : synth.meshlet_vertices) {
                    mergedVertices.push_back(idx + currentPositionOffset);
                }
                for (u8 idx : synth.meshlet_triangles) {
                    mergedTriangles.push_back(idx);
                }
                for (const auto& m : synth.meshlets) {
                    rhi::RHIMeshlet newM = m;
                    newM.vertex_offset += currentVertexOffset;
                    newM.triangle_offset += currentTriangleOffset;
                    mergedMeshlets.push_back(newM);
                }
                currentMeshletOffset += (u32)synth.meshlets.size();
                currentVertexOffset += (u32)synth.meshlet_vertices.size();
                currentTriangleOffset += (u32)synth.meshlet_triangles.size();
            } else {
                for(u32 idx : meshAsset.meshlet_vertices) {
                    mergedVertices.push_back(idx + currentPositionOffset);
                }
                for(u8 idx : meshAsset.meshlet_triangles) {
                    mergedTriangles.push_back(idx);
                }
                for(const auto& m : meshAsset.meshlets) {
                    rhi::RHIMeshlet newM = m;
                    newM.vertex_offset += currentVertexOffset;
                    newM.triangle_offset += currentTriangleOffset;
                    mergedMeshlets.push_back(newM);
                }
                currentMeshletOffset += (u32)meshAsset.meshlets.size();
                currentVertexOffset += (u32)meshAsset.meshlet_vertices.size();
                currentTriangleOffset += (u32)meshAsset.meshlet_triangles.size();
            }
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
    UploadBuffer(global_element_buffer_, mergedElements.data(), mergedElements.size() * sizeof(VertexElement));

    // Dawn Storage buffers are GPU-only; MapBuffer returns zeroed staging and
    // Unmap uploads those zeros back, clobbering the data. Cache the CPU-side
    // meshlets so ExecuteShadowCulling can do CPU frustum culling without
    // touching the GPU buffer.
    cpu_meshlet_cache_ = std::move(mergedMeshlets);

    // Meshlet data quality checks removed - not needed for debugging material mapping

    // 4. Build Cluster Map (Flattened Cluster ID -> Global Meshlet ID)
    // We need to iterate instances again to build this map
    // Total Clusters = sum(instance.cluster_count)
    u32 totalClusters = 0;
    for(const auto& inst : instanceData) totalClusters += inst.cluster_count;

    // 🔥 NEW DEBUG: Print material mapping info
//    std::cout << "[GPUDrivenDrawPipeline] Building ClusterMap with " << totalClusters
//              << " clusters from " << instanceData.size() << " instances..." << std::endl;
    
    struct ClusterMap {
        u32 globalMeshletIndex;
        u32 instanceIndex;
        u32 materialID;  // 🔥 NEW: Per-cluster material ID
        u32 padding;     // Maintain alignment
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

        // 🔥 NEW DEBUG: Print material info for each instance
//        std::cout << "[GPUDrivenDrawPipeline]   Instance " << i
//                  << " (geometry_id=" << instance.geometry_id
//                  << ", material_id=" << instance.material_id
//                  << ", clusters=" << instance.cluster_count << ")" << std::endl;

        // 🔥 CRITICAL DEBUG: Print baseMeshletIndex and range
//        std::cout << "    baseMeshletIndex=" << baseMeshletIndex
//                  << ", cluster range=[" << baseMeshletIndex
//                  << "-" << (baseMeshletIndex + instance.cluster_count - 1) << "]" << std::endl;

        // 🔍 NEW DEBUG: 显示每个cluster使用的material ID（前3个和最后3个）
//        std::cout << "    Cluster material assignments (showing first 3 and last 3):" << std::endl;
        for(u32 c=0; c<instance.cluster_count; ++c) {
            ClusterMap map;
            map.globalMeshletIndex = baseMeshletIndex + c;
            map.instanceIndex = i; // Index into Instance Data Buffer (transforms)
            // 🔥 NEW: Use instance material_id for now
            // TODO: This needs to be per-cluster material_id from meshlet/submesh mapping
            map.materialID = instance.material_id;
            map.padding = 0;
            clusterMapData.push_back(map);

            // 只显示前3个和后3个cluster的详细信息
            if (c < 3 || c >= instance.cluster_count - 3) {
//                std::cout << "      Cluster " << c << " (meshlet " << map.globalMeshletIndex
//                          << ") → material_id=" << map.materialID << std::endl;
            } else if (c == 3) {
//                std::cout << "      ... (skipping " << (instance.cluster_count - 6) << " clusters)" << std::endl;
            }
        }
    }
    
    // Allocate and Upload Cluster Map Buffer
    rhi::BufferDesc mapDesc{};
    mapDesc.size = totalClusters * sizeof(ClusterMap);
    mapDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage);
    mapDesc.usage = rhi::GPUMemoryUsage::Dynamic;
    
    cluster_map_buffer_ = device_->CreateBuffer(mapDesc);
    UploadBuffer(cluster_map_buffer_, clusterMapData.data(), clusterMapData.size() * sizeof(ClusterMap));

    // 5. Build Global Instance Data Buffer (Full InstanceData for material access)
    // We need material_id from InstanceData, so upload full 192-byte structure
    utl::vector<graphics::InstanceData> instanceDataFull;
    instanceDataFull.reserve(instanceData.size());
    for(const auto& inst : instanceData) {
        instanceDataFull.push_back(inst);  // Copy full InstanceData (includes material_id)
    }

    rhi::BufferDesc instDesc{};
    instDesc.size = instanceDataFull.size() * sizeof(graphics::InstanceData);
    instDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage);
    instDesc.usage = rhi::GPUMemoryUsage::Dynamic;
    global_instance_data_buffer_ = device_->CreateBuffer(instDesc);
    UploadBuffer(global_instance_data_buffer_, instanceDataFull.data(), instanceDataFull.size() * sizeof(graphics::InstanceData));

    std::cout << "[GPUDraw] Global buffers built: totalMeshlets=" << totalMeshlets
              << " totalPositions=" << totalPositions
              << " totalElements=" << totalElements
              << " totalInstances=" << instanceData.size() << std::endl;
//              << " instances (full InstanceData with material_id)" << std::endl;

    total_meshlet_count_ = totalMeshlets;
    total_meshlet_vertex_count_ = totalVertices;
    total_meshlet_triangle_count_ = totalTriangles;
    total_vertex_count_ = totalPositions;
    
//    std::cout << "[GPUDrivenDrawPipeline] Global Buffers Built Successfully!" << std::endl;

    // 🔥 NEW DEBUG: Print critical buffer statistics
//    std::cout << "[GPUDrivenDrawPipeline] Buffer Statistics:" << std::endl;
//    std::cout << "  Total meshlets: " << totalMeshlets << std::endl;
//    std::cout << "  Total meshlet_vertices: " << totalVertices << std::endl;
//    std::cout << "  Total meshlet_triangles: " << totalTriangles << std::endl;
//    std::cout << "  Total positions: " << totalPositions << std::endl;
//    std::cout << "  Total instances: " << instanceDataFull.size() << std::endl;
//    std::cout << "  Total clusters: " << clusterMapData.size() << std::endl;
}

bool GPUDrivenDrawPipeline::CreateGeometryBuffers(u32 vertex_count, u32 index_count) {
//    std::cout << "[GPUDrivenDrawPipeline] Creating GPU geometry buffers..." << std::endl;

    if (vertex_count == 0 || index_count == 0) {
//        std::cout << "[GPUDrivenDrawPipeline]   Invalid geometry count" << std::endl;
        return false;
    }

    // Create vertex position buffer
    rhi::BufferDesc positionBufferDesc{};
    positionBufferDesc.size = vertex_count * sizeof(math::v3);
    positionBufferDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::Vertex);
    positionBufferDesc.usage = rhi::GPUMemoryUsage::Static;

    vertex_position_buffer_ = device_->CreateBuffer(positionBufferDesc);
    if (vertex_position_buffer_ != rhi::handles::INVALID_RESOURCE) {
//        std::cout << "[GPUDrivenDrawPipeline]   Vertex position buffer created: "
//                  << positionBufferDesc.size << " bytes" << std::endl;
    }

    // Create vertex normal buffer
    rhi::BufferDesc normalBufferDesc{};
    normalBufferDesc.size = vertex_count * sizeof(math::v3);
    normalBufferDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::Vertex);
    normalBufferDesc.usage = rhi::GPUMemoryUsage::Static;

    vertex_normal_buffer_ = device_->CreateBuffer(normalBufferDesc);
    if (vertex_normal_buffer_ != rhi::handles::INVALID_RESOURCE) {
//        std::cout << "[GPUDrivenDrawPipeline]   Vertex normal buffer created: "
//                  << normalBufferDesc.size << " bytes" << std::endl;
    }

    // Create vertex UV buffer
    rhi::BufferDesc uvBufferDesc{};
    uvBufferDesc.size = vertex_count * sizeof(math::v2);
    uvBufferDesc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::Vertex);
    uvBufferDesc.usage = rhi::GPUMemoryUsage::Static;

    vertex_uv_buffer_ = device_->CreateBuffer(uvBufferDesc);
    if (vertex_uv_buffer_ != rhi::handles::INVALID_RESOURCE) {
//        std::cout << "[GPUDrivenDrawPipeline]   Vertex UV buffer created: "
//                  << uvBufferDesc.size << " bytes" << std::endl;
    }

    // Create index buffer
    rhi::BufferDesc indexBufferDesc{};
    indexBufferDesc.size = index_count * sizeof(u32);
    indexBufferDesc.bindFlags = (u32)rhi::BufferUsageFlags::Index;
    indexBufferDesc.usage = rhi::GPUMemoryUsage::Static;

    index_buffer_ = device_->CreateBuffer(indexBufferDesc);
    if (index_buffer_ != rhi::handles::INVALID_RESOURCE) {
//        std::cout << "[GPUDrivenDrawPipeline]   Index buffer created: "
//                  << indexBufferDesc.size << " bytes" << std::endl;
    }

    // Check if all buffers were created successfully
    bool success = (vertex_position_buffer_ != rhi::handles::INVALID_RESOURCE) &&
                   (vertex_normal_buffer_ != rhi::handles::INVALID_RESOURCE) &&
                   (vertex_uv_buffer_ != rhi::handles::INVALID_RESOURCE) &&
                   (index_buffer_ != rhi::handles::INVALID_RESOURCE);

    if (success) {
//        std::cout << "[GPUDrivenDrawPipeline]   All geometry buffers created successfully" << std::endl;
    } else {
        std::cerr << "[GPUDrivenDrawPipeline]   Failed to create some geometry buffers" << std::endl;
    }

    return success;
}

void GPUDrivenDrawPipeline::UploadGeometryData(const RenderSceneSnapshot& scene_snapshot) {
//    std::cout << "[GPUDrivenDrawPipeline] Uploading test geometry data..." << std::endl;

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

//    std::cout << "[GPUDrivenDrawPipeline]   Created test pyramid: "
//              << positions.size() << " vertices, " << indices.size() << " indices" << std::endl;

    // Upload test geometry to GPU buffers
    if (!positions.empty() && !indices.empty()) {
        // Upload positions
        void* pos_data = device_->MapBuffer(vertex_position_buffer_);
        if (pos_data) {
            memcpy(pos_data, positions.data(), positions.size() * sizeof(math::v3));
            device_->UnmapBuffer(vertex_position_buffer_);
//            std::cout << "[GPUDrivenDrawPipeline]   Test positions uploaded" << std::endl;
        }

        // Upload normals
        void* norm_data = device_->MapBuffer(vertex_normal_buffer_);
        if (norm_data) {
            memcpy(norm_data, normals.data(), normals.size() * sizeof(math::v3));
            device_->UnmapBuffer(vertex_normal_buffer_);
//            std::cout << "[GPUDrivenDrawPipeline]   Test normals uploaded" << std::endl;
        }

        // Upload UVs
        void* uv_data = device_->MapBuffer(vertex_uv_buffer_);
        if (uv_data) {
            memcpy(uv_data, uvs.data(), uvs.size() * sizeof(math::v2));
            device_->UnmapBuffer(vertex_uv_buffer_);
//            std::cout << "[GPUDrivenDrawPipeline]   Test UVs uploaded" << std::endl;
        }

        // Upload indices
        void* idx_data = device_->MapBuffer(index_buffer_);
        if (idx_data) {
            memcpy(idx_data, indices.data(), indices.size() * sizeof(u32));
            device_->UnmapBuffer(index_buffer_);
//            std::cout << "[GPUDrivenDrawPipeline]   Test indices uploaded" << std::endl;
        }

        // Update counts to match actual test data
        vertex_count_ = positions.size();
        index_count_ = indices.size();

//        std::cout << "[GPUDrivenDrawPipeline]   Test geometry data upload complete!" << std::endl;
    } else {
        std::cerr << "[GPUDrivenDrawPipeline]   Failed to create test geometry!" << std::endl;
    }
}

// Phase 9.3b: Draw streaming meshes via non-indexed DrawIndirect.
// Each StreamingMesh's indirect_args is a MTLDrawPrimitivesIndirectCommand
// {vertexStart, vertexCount, instanceCount, instanceStart} written by Pass 4 of
// the SurfaceNets GPU meshing kernel. v1 binds positions to slot 0 and elements
// (packed normal+uv) to slot 1, then issues one DrawIndirect per visible mesh.
// No index buffer is bound — the draw is non-indexed (vertexCount in the indirect
// args equals the index count from generation; the vertices are read sequentially).
// v1: relies on whatever material/pipeline is currently bound (Task 12 validates).
void GPUDrivenDrawPipeline::DrawStreamingMeshes(rhi::RHICommandBuffer* cmd_buffer) {
    if (render_scene_ == nullptr) return;

    // Iterate under RenderScene::mutex_ — a PCG node thread may concurrently
    // RegisterStreamingMesh / UpdateStreamingMesh / UnregisterStreamingMesh and
    // invalidate the vector reference returned by GetStreamingMeshes().
    render_scene_->ForEachStreamingMesh([&](const StreamingMeshRecord& sm) {
        if (!sm.visible || sm.tombstoned) return;
        if (sm.mesh == nullptr || !sm.mesh->IsValid()) return;

        // NOTE: v1 — draw_pipeline_ uses storage-buffer vertex pulling (no vertex
        // input attributes in the pipeline desc). This BindVertexBuffers call has
        // no effect until Task 12 introduces a dedicated vertex-input pipeline for
        // streaming meshes. Kept here so the binding site is already correct.
        rhi::ResourceHandle vb_handles[2] = { sm.mesh->positions, sm.mesh->elements };
        u64 vb_offsets[2] = { 0, 0 };
        cmd_buffer->BindVertexBuffers(0, 2, vb_handles, vb_offsets);

        // Non-indexed indirect draw. The indirect_args buffer contains a
        // MTLDrawPrimitivesIndirectCommand (4 x u32) written by the GPU meshing pass.
        cmd_buffer->DrawIndirect(sm.mesh->indirect_args, 0, 1);
    });
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
//        std::cout << "[GPUDrivenDrawPipeline] Executing pipeline - Frame " << frame_index << std::endl;
    }

    // Cache camera matrices for use in Stage3
    cached_view_matrix_ = view_matrix;
    cached_proj_matrix_ = projection_matrix;

    // First-frame: transition placeholder texture arrays (albedo/normal/ORM, 1×1
    // RGBA8_UNorm) from VK_IMAGE_LAYOUT_UNDEFINED to SHADER_READ_ONLY_OPTIMAL.
    // Without this, descriptor writes at Stage3 reference SHADER_READ_ONLY but the
    // images are still in UNDEFINED — vkQueueSubmit validation fires VUID for
    // each texture (3 errors). The pipeline owns these placeholder textures so
    // the transition belongs here, not in callers.
    if (!placeholder_textures_layout_done_) {
        rhi::ResourceBarrier barriers[3];
        barriers[0].resource = albedo_texture_array_;
        barriers[0].beforeState = rhi::ResourceState::Unknown;
        barriers[0].afterState  = rhi::ResourceState::ShaderResource;
        barriers[1].resource = normal_texture_array_;
        barriers[1].beforeState = rhi::ResourceState::Unknown;
        barriers[1].afterState  = rhi::ResourceState::ShaderResource;
        barriers[2].resource = orm_texture_array_;
        barriers[2].beforeState = rhi::ResourceState::Unknown;
        barriers[2].afterState  = rhi::ResourceState::ShaderResource;
        cmd_buffer->InsertBarrier(barriers, 3);
        placeholder_textures_layout_done_ = true;
    }

    // T4.6.5 part 24.1 (B4 fix): empty-scene no-op. UpdateGeometryData
    // early-returns leaving global buffers INVALID when the snapshot has no
    // instances; Stage2/3 would cascade-fail on INVALID descriptor writes.
    // Skip GPU stages cleanly + let StandardRenderPipeline continue to other
    // passes (HZB/Deferred/Blit) which don't depend on geometry.
    //
    // T4.6.5 part 24.8 (B7 fix): fire Stage3's render pass + transition GBuffer
    // textures to ShaderResource. Without this, the GBuffer textures stay in
    // UNDEFINED layout and DeferredLighting's descriptor reads (expecting
    // SHADER_READ_ONLY_OPTIMAL) trigger VUID-vkCmdDraw-None-09600. RenderGraph
    // skips barriers for imported resources (RenderGraph.cpp:337-340), so the
    // transition must happen here in the producer.
    if (scene_snapshot.GetInstanceCount() == 0 ||
        scene_snapshot.GetClusterRefCount() == 0) {
        if (final_render_pass_ != rhi::handles::INVALID_RENDER_PASS) {
            cmd_buffer->BeginRenderPass(final_render_pass_);
            cmd_buffer->EndRenderPass();

            // T4.6.5 part 24.8 (B7 fix): transition GBuffer color textures
            // (albedo/normal/orm/velocity) to ShaderResource. Depth is left
            // in DEPTH_STENCIL_ATTACHMENT_OPTIMAL from final_render_pass_ —
            // downstream HZB read needs ShaderResource but a separate
            // barrier fires VUID-vkCmdPipelineBarrier-None-01224 because the
            // render pass auto-transition already put the texture in a known
            // state. The depth UNDEFINED errors come from shadow cascade
            // textures, not final_depth_texture_.
            rhi::ResourceBarrier barriers[4];
            barriers[0].resource = gbuffer_albedo_texture_;
            barriers[0].beforeState = rhi::ResourceState::RenderTarget;
            barriers[0].afterState  = rhi::ResourceState::ShaderResource;
            barriers[0].subresource = rhi::RHI_ALL_SUBRESOURCES;
            barriers[0].queueFamily = 0xFFFFFFFF;
            barriers[1] = barriers[0]; barriers[1].resource = gbuffer_normal_texture_;
            barriers[2] = barriers[0]; barriers[2].resource = gbuffer_orm_texture_;
            barriers[3] = barriers[0]; barriers[3].resource = gbuffer_velocity_texture_;
            cmd_buffer->InsertBarrier(barriers, 4);
        }
        results_ = {};
        has_prev_frame_ = false;
        return true;
    }

    // Update geometry data from scene snapshot
    UpdateGeometryData(scene_snapshot);

    // Stage 1: Cluster Binning
    if (!Stage1_ClusterBinning(cmd_buffer, scene_snapshot, culling_results, frame_index)) {
        std::cerr << "[GPUDrivenDrawPipeline] Cluster Binning failed" << std::endl;
        return false;
    }

    // Stage 2: Visibility Buffer
    if (!Stage2_VisibilityBuffer(cmd_buffer, scene_snapshot, view_matrix, projection_matrix,
                                  culling_results, frame_index, buffer_index)) {
        std::cerr << "[GPUDrivenDrawPipeline] Visibility Buffer failed" << std::endl;
        return false;
    }

    // Stage 3: GPU Draw Calls
    if (!Stage3_GPUDrawCalls(cmd_buffer, scene_snapshot, culling_results, results_, frame_index, buffer_index)) {
        std::cerr << "[GPUDrivenDrawPipeline] GPU Draw Calls failed" << std::endl;
        return false;
    }

    // T4.6.5 part 23: auto-resolve. Execute() now leaves resolve_output_texture_
    // populated; callers no longer need a separate ResolveVisibilityBuffer() call.
    // Self-gates on INVALID handles, so this is safe even when Stage2 was skipped.
    ResolveVisibilityBuffer(cmd_buffer);

    if (frame_index == 0) {
//        std::cout << "[GPUDrivenDrawPipeline] Pipeline execution complete" << std::endl;
//        std::cout << "  Total Draw Calls: " << results_.total_draw_calls << std::endl;
//        std::cout << "  Total Clusters Rendered: " << results_.total_clusters_rendered << std::endl;
    }

    // Store current frame matrices for next frame's velocity computation
    prev_view_matrix_ = cached_view_matrix_;
    prev_proj_matrix_ = cached_proj_matrix_;
    has_prev_frame_ = true;

    return true;
}

bool GPUDrivenDrawPipeline::Stage1_ClusterBinning(rhi::RHICommandBuffer* cmd_buffer,
                                                   const RenderSceneSnapshot& scene_snapshot,
                                                   const CullingResults& culling_results,
                                                   u32 frame_index) {
    if (frame_index == 0) {
//        std::cout << "[GPUDrivenDrawPipeline] Stage 1: Cluster Binning" << std::endl;
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
//            std::cout << "[GPUDrivenDrawPipeline]   GPU binning - PLACEHOLDER" << std::endl;
//            std::cout << "[GPUDrivenDrawPipeline]   TODO: Implement compute shader dispatch" << std::endl;
        }

        // cmd_buffer->BindComputePipeline(binning_pipeline_);
        // cmd_buffer->Dispatch(...);
    } else {
        // CPU fallback: Simple binning based on cluster index
        if (frame_index == 0) {
//            std::cout << "[GPUDrivenDrawPipeline]   CPU binning fallback" << std::endl;
//            std::cout << "[GPUDrivenDrawPipeline]   Visible clusters: " << culling_results.visible_cluster_count << std::endl;
        }

        // Calculate bin count
        results_.bin_count = (culling_results.visible_cluster_count + binning_config_.max_clusters_per_bin - 1) /
                            binning_config_.max_clusters_per_bin;

        if (frame_index == 0) {
//            std::cout << "[GPUDrivenDrawPipeline]   Generated bins: " << results_.bin_count << std::endl;
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
                                                    const CullingResults& culling_results,
                                                    u32 frame_index,
                                                    u32 buffer_index) {
    (void)view_matrix;
    (void)projection_matrix;

    // T4.6.5 part 22.5: Stage2 rasterizes meshlets into visibility_buffer_
    // (R32_UINT packed (meshlet_id<<24)|primitive_id). Stage3 then rasterizes
    // the same meshlets to GBuffer using a separate pipeline. Both stages
    // share final_depth_texture_ as their depth attachment.
    //
    // Bail-out conditions: Stage2 only runs if the visibility pipeline,
    // render pass, per-frame CB, and descriptor set are all initialized.
    // On Dawn the R32_UInt storage format isn't always supported, so
    // visibility_buffer_ may be INVALID — skip silently in that case.
    if (visibility_pipeline_ == rhi::handles::INVALID_PIPELINE) return true;
    if (visibility_render_pass_ == rhi::handles::INVALID_RENDER_PASS) return true;
    if (visibility_buffer_ == rhi::handles::INVALID_RESOURCE) return true;
    if (visibility_cb_ == rhi::handles::INVALID_RESOURCE) return true;
    if (visibility_descriptor_set_ == rhi::handles::INVALID_DESCRIPTOR_SET) return true;

    if (results_.bin_count == 0) {
        results_.bin_count = 1;
        results_.total_clusters_rendered = 0;
    }

    // 1. Fill DrawConstants CB (matches VisibilityBuffer.vert struct layout:
    // 3 m4x4 + 4 u32 = 208 bytes).
    struct VisDrawConstants {
        math::m4x4 view_matrix;
        math::m4x4 proj_matrix;
        math::m4x4 world_matrix;
        u32 view_width;
        u32 view_height;
        u32 meshlet_count;
        u32 _pad;
    };
    VisDrawConstants dc{};
    dc.view_matrix = cached_view_matrix_;
    dc.proj_matrix = cached_proj_matrix_;
    dc.world_matrix = rhi::math::MatrixIdentity();
    dc.view_width = visibility_config_.width;
    dc.view_height = visibility_config_.height;
    dc.meshlet_count = total_meshlet_count_;

    void* mapped = device_->MapBuffer(visibility_cb_);
    if (mapped) {
        memcpy(mapped, &dc, sizeof(dc));
        device_->UnmapBuffer(visibility_cb_);
    }

    // 2. Resolve read_buffer_index + per-frame buffers FIRST.
    // compact_cluster_ids (visible cluster list) changes every frame, so the
    // descriptor set must be rewritten unconditionally — not one-shot.
    u32 read_buffer_index = (buffer_index + frame_resources_.size() - 1) % frame_resources_.size();
    rhi::ResourceHandle indirectBuffer = rhi::handles::INVALID_RESOURCE;
    if (culling_pipeline_) {
        indirectBuffer = culling_pipeline_->GetIndirectBuffer(read_buffer_index);
    } else if (culling_results.indirect_args_buffer != rhi::handles::INVALID_RESOURCE) {
        indirectBuffer = culling_results.indirect_args_buffer;
    }
    if (indirectBuffer == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[Stage2] No valid indirect buffer — skipping visibility raster" << std::endl;
        return false;
    }
    rhi::ResourceHandle correctVisibleClusterBuffer = culling_results.visible_cluster_list_buffer;
    if (culling_pipeline_) {
        correctVisibleClusterBuffer = culling_pipeline_->GetVisibleClusterListBuffer(read_buffer_index);
    }

    // 3. Write descriptor set every frame (8 bindings).
    rhi::DescriptorBufferInfo cbInfo{ visibility_cb_, 0, ~0ULL };
    rhi::DescriptorBufferInfo meshletInfo{ global_meshlet_buffer_, 0, ~0ULL };
    rhi::DescriptorBufferInfo meshletVertsInfo{ global_meshlet_vertices_buffer_, 0, ~0ULL };
    rhi::DescriptorBufferInfo meshletTrisInfo{ global_meshlet_triangles_buffer_, 0, ~0ULL };
    rhi::DescriptorBufferInfo positionsInfo{ global_vertex_buffer_, 0, ~0ULL };
    rhi::DescriptorBufferInfo compactClusterInfo{ correctVisibleClusterBuffer, 0, ~0ULL };
    rhi::DescriptorBufferInfo clusterMapInfo{ cluster_map_buffer_, 0, ~0ULL };
    rhi::DescriptorBufferInfo instanceInfo{ global_instance_data_buffer_, 0, ~0ULL };

    rhi::WriteDescriptorSet writes[8];
    writes[0].dstSet = visibility_descriptor_set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = rhi::DescriptorType::UniformBuffer;
    writes[0].bufferInfo = &cbInfo;

    writes[1].dstSet = visibility_descriptor_set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[1].bufferInfo = &meshletInfo;

    writes[2].dstSet = visibility_descriptor_set_;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[2].bufferInfo = &meshletVertsInfo;

    writes[3].dstSet = visibility_descriptor_set_;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[3].bufferInfo = &meshletTrisInfo;

    writes[4].dstSet = visibility_descriptor_set_;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[4].bufferInfo = &positionsInfo;

    writes[5].dstSet = visibility_descriptor_set_;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[5].bufferInfo = &compactClusterInfo;

    writes[6].dstSet = visibility_descriptor_set_;
    writes[6].dstBinding = 6;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[6].bufferInfo = &clusterMapInfo;

    writes[7].dstSet = visibility_descriptor_set_;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[7].bufferInfo = &instanceInfo;

    device_->UpdateDescriptorSets(8, writes);
    visibility_descriptor_written_ = true;

    // 4. BeginRenderPass + viewport + bind + draw.
    cmd_buffer->BeginRenderPass(visibility_render_pass_);
    cmd_buffer->SetViewport({
        {0, 0},
        {static_cast<float>(visibility_config_.width), static_cast<float>(visibility_config_.height)},
        0, 1
    });
    cmd_buffer->SetScissor({
        {0, 0},
        {visibility_config_.width, visibility_config_.height}
    });
    cmd_buffer->BindGraphicsPipeline(visibility_pipeline_);

    const rhi::DescriptorSetHandle dsHandles[] = { visibility_descriptor_set_ };
    cmd_buffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics,
                                    visibility_pipeline_layout_,
                                    0, 1, dsHandles, 0, nullptr);

    cmd_buffer->DrawIndirect(indirectBuffer, 0, 1);
    cmd_buffer->EndRenderPass();

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

    // CRITICAL FIX: Use N-2 delay for reading culling results.
    // The culling pipeline writes to buffer[N], but the GPU may still be reading
    // from buffer[N] when frame N+3's Stage 0 resets it. Reading from buffer[(N+1)%3]
    // (1-frame delay) ensures the GPU has finished reading before we write again.
    // This is the standard triple-buffer synchronization pattern.
    u32 read_buffer_index = (buffer_index + frame_resources_.size() - 1) % frame_resources_.size();

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
        // --- Existing fields (DO NOT REORDER) ---
        math::m4x4 view_matrix;
        math::m4x4 proj_matrix;
        math::m4x4 world_matrix;       // Unused
        u32 view_width;
        u32 view_height;
        u32 meshlet_count;
        u32 padding;
        // --- New fields appended at end ---
        math::m4x4 prev_view_matrix;   // Previous frame view matrix
        math::m4x4 prev_proj_matrix;   // Previous frame proj matrix
        u32 has_prev_frame;            // 1 if previous frame data is available
        u32 debug_mode;                // 0=off, 1=meshlet, 2=triangle, 3=mesh
        u32 padding2[2];               // Alignment padding to 16 bytes
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
    drawConsts.prev_view_matrix = prev_view_matrix_;
    drawConsts.prev_proj_matrix = prev_proj_matrix_;
    drawConsts.has_prev_frame = has_prev_frame_ ? 1u : 0u;
    drawConsts.debug_mode = meshlet_debug_mode_;
    drawConsts.padding2[0] = drawConsts.padding2[1] = 0;

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


    // 🔥 NEW: Element buffer (Normal, Tangent, UV)
    rhi::DescriptorBufferInfo elementBufferInfo{ global_element_buffer_, 0, ~0ULL };

    // 🎨 NEW: Material data buffer (for fragment shader)
    rhi::DescriptorBufferInfo materialDataBufferInfo{ global_material_data_buffer_, 0, ~0ULL };

    // 🎨 NEW: Texture arrays and sampler (for fragment shader)
    // Note: DescriptorImageInfo order is sampler, imageView, imageLayout
    rhi::DescriptorImageInfo albedoTextureInfo{ texture_sampler_, albedo_texture_array_, rhi::ResourceState::ShaderResource };
    rhi::DescriptorImageInfo normalTextureInfo{ texture_sampler_, normal_texture_array_, rhi::ResourceState::ShaderResource };
    rhi::DescriptorImageInfo ormTextureInfo{ texture_sampler_, orm_texture_array_, rhi::ResourceState::ShaderResource };
    rhi::DescriptorImageInfo samplerInfo{ texture_sampler_, rhi::handles::INVALID_RESOURCE, rhi::ResourceState::General };

    rhi::WriteDescriptorSet writes[14];
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

    // 🔥 NEW: Bind Element Buffer to binding 8
    writes[8].dstSet = globalDrawDS;
    writes[8].dstBinding = 8;
    writes[8].descriptorCount = 1;
    writes[8].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[8].bufferInfo = &elementBufferInfo;

    // 🎨 NEW: Material data buffer to binding 9
    writes[9].dstSet = globalDrawDS;
    writes[9].dstBinding = 9;
    writes[9].descriptorCount = 1;
    writes[9].descriptorType = rhi::DescriptorType::StorageBuffer;
    writes[9].bufferInfo = &materialDataBufferInfo;

    // 🎨 NEW: Texture arrays to bindings 10, 11, 12
    writes[10].dstSet = globalDrawDS;
    writes[10].dstBinding = 10;
    writes[10].descriptorCount = 1;
    writes[10].descriptorType = rhi::DescriptorType::SampledImage;
    writes[10].imageInfo = &albedoTextureInfo;

    writes[11].dstSet = globalDrawDS;
    writes[11].dstBinding = 11;
    writes[11].descriptorCount = 1;
    writes[11].descriptorType = rhi::DescriptorType::SampledImage;
    writes[11].imageInfo = &normalTextureInfo;

    writes[12].dstSet = globalDrawDS;
    writes[12].dstBinding = 12;
    writes[12].descriptorCount = 1;
    writes[12].descriptorType = rhi::DescriptorType::SampledImage;
    writes[12].imageInfo = &ormTextureInfo;

    // 🎨 NEW: Texture sampler to binding 13
    writes[13].dstSet = globalDrawDS;
    writes[13].dstBinding = 13;
    writes[13].descriptorCount = 1;
    writes[13].descriptorType = rhi::DescriptorType::Sampler;
    writes[13].imageInfo = &samplerInfo;

    device_->UpdateDescriptorSets(14, writes);

    // Bind Pipeline and Descriptor Set
    // NOTE: No memory barrier needed here - Metal automatically handles encoder transitions
    // The GPU culling work is guaranteed to be complete by the Metal driver
    cmd_buffer->BindGraphicsPipeline(draw_pipeline_);

    rhi::DescriptorSetHandle dsHandles[1] = { globalDrawDS };
    u32 dynOffsets[1] = { 0 };
    cmd_buffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, draw_pipeline_layout_, 0, 1, dsHandles, 1, dynOffsets);

    // Phase 9.3b: Draw streaming meshes (GPU SurfaceNets terrain) before the
    // Nanite cluster draw. Uses the same render pass and currently-bound pipeline
    // (v1 limitation — Task 12 will validate visually and wire a proper default
    // material pipeline if needed).
    DrawStreamingMeshes(cmd_buffer);

    // Indirect Draw

    // DrawIndirect — use the culling pipeline's indirect args from the SAME
    // frame slot as the visible cluster list (read_buffer_index). Mixing
    // current-frame indirect args with previous-frame cluster list causes
    // instanceCount/visible_count mismatch → meshlets read OOB/stale entries.
    rhi::ResourceHandle indirectBuffer = rhi::handles::INVALID_RESOURCE;
    if (culling_pipeline_) {
        indirectBuffer = culling_pipeline_->GetIndirectBuffer(read_buffer_index);
    } else if (culling_results.indirect_args_buffer != rhi::handles::INVALID_RESOURCE) {
        indirectBuffer = culling_results.indirect_args_buffer;
    }

    if (indirectBuffer != rhi::handles::INVALID_RESOURCE) {
        cmd_buffer->DrawIndirect(indirectBuffer, 0, 1);
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
    if (!initialized_) return;
    if (resolve_pipeline_ == rhi::handles::INVALID_PIPELINE) return;
    if (visibility_buffer_ == rhi::handles::INVALID_RESOURCE) return;
    if (final_depth_texture_ == rhi::handles::INVALID_RESOURCE) return;
    if (resolve_cb_ == rhi::handles::INVALID_RESOURCE) return;
    if (resolve_descriptor_set_ == rhi::handles::INVALID_DESCRIPTOR_SET) return;

    // 1. Fill DrawConstants CB (matches shader layout at VisibilityBufferResolve.comp:23-31).
    struct ResolveDrawConstants {
        math::m4x4 view_matrix;
        math::m4x4 proj_matrix;
        math::m4x4 world_matrix;
        u32 view_width;
        u32 view_height;
        u32 meshlet_count;
        u32 _pad;
    };
    ResolveDrawConstants dc{
        cached_view_matrix_,
        cached_proj_matrix_,
        rhi::math::MatrixIdentity(),
        visibility_config_.width,
        visibility_config_.height,
        total_meshlet_count_,
        0
    };
    void* mapped = device_->MapBuffer(resolve_cb_);
    if (mapped) {
        memcpy(mapped, &dc, sizeof(dc));
        device_->UnmapBuffer(resolve_cb_);
    }

    // 2. Write descriptor set (once — resource handles don't change across frames).
    if (!resolve_descriptor_written_) {
        rhi::DescriptorImageInfo visInfo{ resolve_sampler_, visibility_buffer_, rhi::ResourceState::ShaderResource };
        rhi::DescriptorImageInfo depthInfo{ resolve_sampler_, final_depth_texture_, rhi::ResourceState::ShaderResource };
        rhi::DescriptorImageInfo outInfo{ rhi::handles::INVALID_SAMPLER, resolve_output_texture_, rhi::ResourceState::UnorderedAccess };
        rhi::DescriptorBufferInfo cbInfo{ resolve_cb_, 0, ~0ULL };
        rhi::DescriptorBufferInfo meshletInfo{ global_meshlet_buffer_, 0, ~0ULL };

        rhi::WriteDescriptorSet writes[5];
        writes[0].dstSet = resolve_descriptor_set_;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = rhi::DescriptorType::CombinedImageSampler;
        writes[0].imageInfo = &visInfo;
        writes[1] = writes[0];
        writes[1].dstBinding = 1;
        writes[1].imageInfo = &depthInfo;
        writes[2] = writes[0];
        writes[2].dstBinding = 2;
        writes[2].descriptorType = rhi::DescriptorType::StorageImage;
        writes[2].imageInfo = &outInfo;
        writes[3] = writes[0];
        writes[3].dstBinding = 3;
        writes[3].descriptorType = rhi::DescriptorType::UniformBuffer;
        writes[3].imageInfo = nullptr;
        writes[3].bufferInfo = &cbInfo;
        writes[4] = writes[3];
        writes[4].dstBinding = 4;
        writes[4].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[4].bufferInfo = &meshletInfo;

        device_->UpdateDescriptorSets(5, writes);
        resolve_descriptor_written_ = true;
    }

    // 3. Layout transitions before dispatch.
    //    visibility_buffer_ (T4.6.5 part 22): Stage2 now rasterizes into it, so
    //    before-state is RenderTarget (COLOR_ATTACHMENT_OPTIMAL). Using Unknown
    //    (UNDEFINED) discards Stage2's content and resolve reads zeros.
    //    final_depth_texture_: Stage3 leaves it in DepthStencil (after
    //    storeOp=Store). Transition to ShaderResource.
    //    resolve_output_texture_ → UnorderedAccess (storage image, GENERAL).
    rhi::ResourceBarrier barriers[3];
    barriers[0].resource = visibility_buffer_;
    barriers[0].beforeState = rhi::ResourceState::RenderTarget;
    barriers[0].afterState = rhi::ResourceState::ShaderResource;
    barriers[1].resource = final_depth_texture_;
    barriers[1].beforeState = rhi::ResourceState::DepthStencil;
    barriers[1].afterState = rhi::ResourceState::ShaderResource;
    barriers[2].resource = resolve_output_texture_;
    barriers[2].beforeState = rhi::ResourceState::Unknown;
    barriers[2].afterState = rhi::ResourceState::UnorderedAccess;
    cmd_buffer->InsertBarrier(barriers, 3);

    // 4. Bind + dispatch.
    cmd_buffer->BindComputePipeline(resolve_pipeline_);
    const rhi::DescriptorSetHandle sets[] = { resolve_descriptor_set_ };
    cmd_buffer->BindDescriptorSets(
        rhi::PipelineBindPoint::Compute,
        resolve_pipeline_layout_,
        0, 1, sets,
        0, nullptr
    );
    const u32 gx = (visibility_config_.width + 7) / 8;
    const u32 gy = (visibility_config_.height + 7) / 8;
    cmd_buffer->Dispatch(gx, gy, 1);

    // 5. Transition output to CopySource so the test/caller can read it back.
    rhi::ResourceBarrier after;
    after.resource = resolve_output_texture_;
    after.beforeState = rhi::ResourceState::UnorderedAccess;
    after.afterState = rhi::ResourceState::CopySource;
    cmd_buffer->InsertBarrier(&after, 1);
}

// ============================================================================
// Shadow Mapping Implementation
// ============================================================================

bool GPUDrivenDrawPipeline::InitializeShadowResources(u32 num_instances, u32 max_clusters) {
    if (shadow_initialized_) return true;
    if (!device_) return false;

    shadow_max_clusters_ = max_clusters;

    // --- Load & create shadow shaders ---

    // Shadow culling compute shader
    auto cullCode = LoadShaderBytecode("ShadowCulling", "shadow_cluster_culling", device_);
    auto finalizeCode = LoadShaderBytecode("ShadowCulling", "shadow_finalize_indirect", device_);
    auto depthVSCode = LoadShaderBytecode("ShadowDepth", "shadow_depth_vs", device_);
    auto blitCode = LoadShaderBytecode("ShadowBlit", "shadow_depth_blit", device_);

    if (cullCode.empty() || finalizeCode.empty() || depthVSCode.empty() || blitCode.empty()) {
        std::cerr << "[Shadow] Failed to load shadow shaders" << std::endl;
        return false;
    }

    auto cullShader = device_->CreateShader(cullCode.data(), cullCode.size(), rhi::ShaderStage::Compute, "shadow_cluster_culling");
    auto finalizeShader = device_->CreateShader(finalizeCode.data(), finalizeCode.size(), rhi::ShaderStage::Compute, "shadow_finalize_indirect");
    auto depthVS = device_->CreateShader(depthVSCode.data(), depthVSCode.size(), rhi::ShaderStage::Vertex, "shadow_depth_vs");
    auto blitShader = device_->CreateShader(blitCode.data(), blitCode.size(), rhi::ShaderStage::Compute, "shadow_depth_blit");

    if (cullShader == rhi::handles::INVALID_SHADER || finalizeShader == rhi::handles::INVALID_SHADER ||
        depthVS == rhi::handles::INVALID_SHADER || blitShader == rhi::handles::INVALID_SHADER) {
        std::cerr << "[Shadow] Failed to create shadow shader handles" << std::endl;
        return false;
    }

    // --- Shadow cull descriptor layout (7 bindings) ---
    {
        rhi::DescriptorSetLayoutBinding cullBindings[7];
        for (int i = 0; i < 7; ++i) {
            cullBindings[i].binding = i;
            cullBindings[i].descriptorCount = 1;
            cullBindings[i].stageFlags = rhi::ShaderStage::Compute;
        }
        cullBindings[0].descriptorType = rhi::DescriptorType::StorageBuffer; // instances
        cullBindings[0].readonly = true;  // shader: var<storage, read>
        cullBindings[1].descriptorType = rhi::DescriptorType::StorageBuffer; // meshlets
        cullBindings[1].readonly = true;  // shader: var<storage, read>
        cullBindings[2].descriptorType = rhi::DescriptorType::StorageBuffer; // cluster_map
        cullBindings[2].readonly = true;  // shader: var<storage, read>
        cullBindings[3].descriptorType = rhi::DescriptorType::UniformBuffer; // uniforms
        cullBindings[4].descriptorType = rhi::DescriptorType::StorageBuffer; // visible_counter
        cullBindings[5].descriptorType = rhi::DescriptorType::StorageBuffer; // visible_clusters
        cullBindings[6].descriptorType = rhi::DescriptorType::StorageBuffer; // indirect_args

        rhi::DescriptorSetLayoutDesc cullLayoutDesc{ .bindingCount = 7, .bindings = cullBindings };
        shadow_cull_set_layout_ = device_->CreateDescriptorSetLayout(cullLayoutDesc);
        if (shadow_cull_set_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;

        rhi::PipelineLayoutDesc cullPLDesc{ .setLayoutCount = 1, .setLayouts = &shadow_cull_set_layout_ };
        shadow_cull_layout_ = device_->CreatePipelineLayout(cullPLDesc);
        if (shadow_cull_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) return false;
    }

    // Shadow cull pipeline
    {
        rhi::ComputePipelineDesc desc{};
        desc.layout = shadow_cull_layout_;
        desc.computeShader = cullShader;
        desc.threadGroupSize = {64, 1, 1};
        shadow_cull_pipeline_ = device_->CreateComputePipeline(desc);
        if (shadow_cull_pipeline_ == rhi::handles::INVALID_PIPELINE) return false;
    }

    // Shadow finalize pipeline (reuses same layout as cull)
    {
        rhi::ComputePipelineDesc desc{};
        desc.layout = shadow_cull_layout_;
        desc.computeShader = finalizeShader;
        desc.threadGroupSize = {64, 1, 1};
        shadow_finalize_pipeline_ = device_->CreateComputePipeline(desc);
        if (shadow_finalize_pipeline_ == rhi::handles::INVALID_PIPELINE) return false;
    }

    // --- Shadow depth descriptor layout (8 bindings) ---
    {
        rhi::DescriptorSetLayoutBinding depthBindings[8];
        for (int i = 0; i < 8; ++i) {
            depthBindings[i].binding = i;
            depthBindings[i].descriptorCount = 1;
            depthBindings[i].stageFlags = rhi::ShaderStage::Vertex;
        }
        depthBindings[0].descriptorType = rhi::DescriptorType::UniformBuffer; // ShadowDepthUniforms
        depthBindings[1].descriptorType = rhi::DescriptorType::StorageBuffer; // visible_clusters
        depthBindings[2].descriptorType = rhi::DescriptorType::StorageBuffer; // cluster_map
        depthBindings[3].descriptorType = rhi::DescriptorType::StorageBuffer; // instances
        depthBindings[4].descriptorType = rhi::DescriptorType::StorageBuffer; // meshlets
        depthBindings[5].descriptorType = rhi::DescriptorType::StorageBuffer; // meshlet_vertex_indices
        depthBindings[6].descriptorType = rhi::DescriptorType::StorageBuffer; // meshlet_triangle_indices
        depthBindings[7].descriptorType = rhi::DescriptorType::StorageBuffer; // vertex_positions

        rhi::DescriptorSetLayoutDesc depthLayoutDesc{ .bindingCount = 8, .bindings = depthBindings };
        shadow_depth_set_layout_ = device_->CreateDescriptorSetLayout(depthLayoutDesc);
        if (shadow_depth_set_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;

        rhi::PipelineLayoutDesc depthPLDesc{ .setLayoutCount = 1, .setLayouts = &shadow_depth_set_layout_ };
        shadow_depth_layout_ = device_->CreatePipelineLayout(depthPLDesc);
        if (shadow_depth_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) return false;
    }

    // Shadow depth graphics pipeline (depth-only, no fragment shader)
    {
        rhi::GraphicsPipelineDesc desc{};
        desc.layout = shadow_depth_layout_;
        desc.vertexShader = depthVS;
        desc.pixelShader = rhi::handles::INVALID_SHADER; // depth-only pass
        desc.depthStencilFormat = rhi::DataFormat::D32_Float;
        desc.enableDepthTest = true;
        desc.enableDepthWrite = true;
        desc.depthFunc = rhi::ComparisonFunc::Less;
        desc.renderTargetCount = 0;
        desc.cullMode = rhi::CullMode::Back;  // Cull back faces for stable shadow map depth
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();

        shadow_depth_pipeline_ = device_->CreateGraphicsPipeline(desc);
        if (shadow_depth_pipeline_ == rhi::handles::INVALID_PIPELINE) {
            std::cerr << "[Shadow] Failed to create shadow depth graphics pipeline" << std::endl;
            return false;
        }
    }

    // --- Shadow blit descriptor layout (3 bindings) ---
    // N1a: On Dawn the source is texture_depth_2d (D32_Float depth target) and
    // must be bound as SampledDepthImage. Metal treats depth and color textures
    // uniformly via SampledImage.
    {
        bool isDawn = (device_->GetPlatform() == rhi::RHIPlatform::Dawn ||
                   device_->GetPlatform() == rhi::RHIPlatform::Vulkan);
        rhi::DescriptorSetLayoutBinding blitBindings[3];
        blitBindings[0].binding = 0;
        blitBindings[0].descriptorType = isDawn ? rhi::DescriptorType::SampledDepthImage
                                                 : rhi::DescriptorType::SampledImage;
        blitBindings[0].descriptorCount = 1;
        blitBindings[0].stageFlags = rhi::ShaderStage::Compute;
        blitBindings[1].binding = 1;
        blitBindings[1].descriptorType = rhi::DescriptorType::StorageImage;
        blitBindings[1].descriptorCount = 1;
        blitBindings[1].stageFlags = rhi::ShaderStage::Compute;
        blitBindings[1].format = rhi::DataFormat::R32_Float;  // matches dst_depth: texture_storage_2d<r32float, write>
        blitBindings[2].binding = 2;
        blitBindings[2].descriptorType = rhi::DescriptorType::UniformBuffer;
        blitBindings[2].descriptorCount = 1;
        blitBindings[2].stageFlags = rhi::ShaderStage::Compute;

        rhi::DescriptorSetLayoutDesc blitLayoutDesc{ 3, blitBindings };
        shadow_blit_set_layout_ = device_->CreateDescriptorSetLayout(blitLayoutDesc);
        if (shadow_blit_set_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;

        rhi::PipelineLayoutDesc blitPLDesc{ 1, &shadow_blit_set_layout_, 0, nullptr };
        shadow_blit_layout_ = device_->CreatePipelineLayout(blitPLDesc);
        if (shadow_blit_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) return false;
    }

    // Shadow blit compute pipeline
    {
        rhi::ComputePipelineDesc desc{};
        desc.layout = shadow_blit_layout_;
        desc.computeShader = blitShader;
        desc.threadGroupSize = {8, 8, 1};
        shadow_blit_pipeline_ = device_->CreateComputePipeline(desc);
        if (shadow_blit_pipeline_ == rhi::handles::INVALID_PIPELINE) return false;
    }

    // --- Per-frame per-cascade resources ---
    for (u32 f = 0; f < 3; ++f) {
        auto& frame = shadow_frames_[f];

        for (u32 cascade = 0; cascade < 2; ++cascade) {
            // Shadow depth render target (D32_Float, 2048x2048)
            {
                rhi::TextureDesc desc{};
                desc.size = {2048, 2048, 1};
                desc.format = rhi::DataFormat::D32_Float;
                desc.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::ShaderResource;
                desc.memoryUsage = rhi::GPUMemoryUsage::Static;
                auto& rt = (cascade == 0) ? frame.shadow_depth_rt_0 : frame.shadow_depth_rt_1;
                rt = device_->CreateTexture(desc);
                if (rt == rhi::handles::INVALID_RESOURCE) return false;
            }

            // Shadow map sampleable (R32_Float, 2048x2048)
            {
                rhi::TextureDesc desc{};
                desc.size = {2048, 2048, 1};
                desc.format = rhi::DataFormat::R32_Float;
                desc.usage = rhi::TextureUsage::UnorderedAccess | rhi::TextureUsage::ShaderResource;
                desc.memoryUsage = rhi::GPUMemoryUsage::Static;
                auto& sm = (cascade == 0) ? frame.shadow_map_0 : frame.shadow_map_1;
                sm = device_->CreateTexture(desc);
                if (sm == rhi::handles::INVALID_RESOURCE) return false;
            }

            // Visible clusters buffer
            {
                rhi::BufferDesc desc{};
                desc.size = max_clusters * sizeof(u32);
                desc.bindFlags = (u32)rhi::BufferUsageFlags::Storage;
                desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
                frame.visible_clusters_buffer[cascade] = device_->CreateBuffer(desc);
                if (frame.visible_clusters_buffer[cascade] == rhi::handles::INVALID_RESOURCE) return false;
            }

            // Per-cascade atomic counter (no longer shared between cascades/frames)
            {
                rhi::BufferDesc desc{};
                desc.size = sizeof(u32); // atomic_uint
                desc.bindFlags = (u32)(rhi::BufferUsageFlags::Storage | rhi::BufferUsageFlags::TransferDst);
                desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
                frame.visible_counter_buffer[cascade] = device_->CreateBuffer(desc);
                if (frame.visible_counter_buffer[cascade] == rhi::handles::INVALID_RESOURCE) return false;
            }

            // Indirect draw buffer
            {
                rhi::BufferDesc desc{};
                desc.size = sizeof(u32) * 4; // IndirectDrawArgs
                desc.bindFlags = (u32)(rhi::BufferUsageFlags::Indirect | rhi::BufferUsageFlags::Storage);
                desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
                frame.indirect_draw_buffer[cascade] = device_->CreateBuffer(desc);
                if (frame.indirect_draw_buffer[cascade] == rhi::handles::INVALID_RESOURCE) return false;
            }

            // Light frustum CB
            {
                rhi::BufferDesc desc{};
                desc.size = 256; // ShadowCullUniforms + padding
                desc.bindFlags = (u32)rhi::BufferUsageFlags::Uniform;
                desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
                frame.light_frustum_cb[cascade] = device_->CreateBuffer(desc);
                if (frame.light_frustum_cb[cascade] == rhi::handles::INVALID_RESOURCE) return false;
            }

            // Shadow depth CB (ShadowDepthUniforms)
            {
                rhi::BufferDesc desc{};
                desc.size = 128;
                desc.bindFlags = (u32)rhi::BufferUsageFlags::Uniform;
                desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
                frame.shadow_depth_cb[cascade] = device_->CreateBuffer(desc);
                if (frame.shadow_depth_cb[cascade] == rhi::handles::INVALID_RESOURCE) return false;
            }

            // Blit resolution CB
            {
                rhi::BufferDesc desc{};
                desc.size = 16; // uint2 + padding
                desc.bindFlags = (u32)rhi::BufferUsageFlags::Uniform;
                desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
                frame.blit_resolution_cb[cascade] = device_->CreateBuffer(desc);
                if (frame.blit_resolution_cb[cascade] == rhi::handles::INVALID_RESOURCE) return false;
            }

            // Shadow cull descriptor set
            {
                rhi::DescriptorSetDesc dsDesc{ .layout = shadow_cull_set_layout_ };
                frame.shadow_cull_descriptor_set[cascade] = device_->CreateDescriptorSet(dsDesc);
                if (frame.shadow_cull_descriptor_set[cascade] == rhi::handles::INVALID_DESCRIPTOR_SET) return false;
            }

            // Shadow depth descriptor set
            {
                rhi::DescriptorSetDesc dsDesc{ .layout = shadow_depth_set_layout_ };
                frame.shadow_depth_descriptor_set[cascade] = device_->CreateDescriptorSet(dsDesc);
                if (frame.shadow_depth_descriptor_set[cascade] == rhi::handles::INVALID_DESCRIPTOR_SET) return false;
            }

            // Shadow blit descriptor set
            {
                rhi::DescriptorSetDesc dsDesc{ .layout = shadow_blit_set_layout_ };
                frame.shadow_blit_descriptor_set[cascade] = device_->CreateDescriptorSet(dsDesc);
                if (frame.shadow_blit_descriptor_set[cascade] == rhi::handles::INVALID_DESCRIPTOR_SET) return false;
            }
        }
    }

    // --- GBuffer depth blit resources (reuse shadow_blit_pipeline_) ---
    {
        rhi::DescriptorSetDesc dsDesc{ .layout = shadow_blit_set_layout_ };
        gbuffer_depth_blit_descriptor_set_ = device_->CreateDescriptorSet(dsDesc);

        rhi::BufferDesc cbDesc{};
        cbDesc.size = 16;
        cbDesc.bindFlags = (u32)rhi::BufferUsageFlags::Uniform;
        cbDesc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        gbuffer_depth_blit_cb_ = device_->CreateBuffer(cbDesc);
    }

    shadow_initialized_ = true;
    return true;
}

void GPUDrivenDrawPipeline::ShutdownShadowResources() {
    if (!shadow_initialized_ || !device_) return;

    for (u32 f = 0; f < 3; ++f) {
        auto& frame = shadow_frames_[f];

        // Cascade 0 resources
        if (frame.shadow_depth_rt_0 != rhi::handles::INVALID_RESOURCE) { device_->DestroyTexture(frame.shadow_depth_rt_0); frame.shadow_depth_rt_0 = rhi::handles::INVALID_RESOURCE; }
        if (frame.shadow_map_0 != rhi::handles::INVALID_RESOURCE) { device_->DestroyTexture(frame.shadow_map_0); frame.shadow_map_0 = rhi::handles::INVALID_RESOURCE; }
        // Cascade 1 resources
        if (frame.shadow_depth_rt_1 != rhi::handles::INVALID_RESOURCE) { device_->DestroyTexture(frame.shadow_depth_rt_1); frame.shadow_depth_rt_1 = rhi::handles::INVALID_RESOURCE; }
        if (frame.shadow_map_1 != rhi::handles::INVALID_RESOURCE) { device_->DestroyTexture(frame.shadow_map_1); frame.shadow_map_1 = rhi::handles::INVALID_RESOURCE; }

        for (u32 cascade = 0; cascade < 2; ++cascade) {
            if (frame.visible_counter_buffer[cascade] != rhi::handles::INVALID_RESOURCE) { device_->DestroyBuffer(frame.visible_counter_buffer[cascade]); frame.visible_counter_buffer[cascade] = rhi::handles::INVALID_RESOURCE; }
            if (frame.visible_clusters_buffer[cascade] != rhi::handles::INVALID_RESOURCE) { device_->DestroyBuffer(frame.visible_clusters_buffer[cascade]); frame.visible_clusters_buffer[cascade] = rhi::handles::INVALID_RESOURCE; }
            if (frame.indirect_draw_buffer[cascade] != rhi::handles::INVALID_RESOURCE) { device_->DestroyBuffer(frame.indirect_draw_buffer[cascade]); frame.indirect_draw_buffer[cascade] = rhi::handles::INVALID_RESOURCE; }
            if (frame.light_frustum_cb[cascade] != rhi::handles::INVALID_RESOURCE) { device_->DestroyBuffer(frame.light_frustum_cb[cascade]); frame.light_frustum_cb[cascade] = rhi::handles::INVALID_RESOURCE; }
            if (frame.shadow_depth_cb[cascade] != rhi::handles::INVALID_RESOURCE) { device_->DestroyBuffer(frame.shadow_depth_cb[cascade]); frame.shadow_depth_cb[cascade] = rhi::handles::INVALID_RESOURCE; }
            if (frame.blit_resolution_cb[cascade] != rhi::handles::INVALID_RESOURCE) { device_->DestroyBuffer(frame.blit_resolution_cb[cascade]); frame.blit_resolution_cb[cascade] = rhi::handles::INVALID_RESOURCE; }
            if (frame.shadow_cull_descriptor_set[cascade] != rhi::handles::INVALID_DESCRIPTOR_SET) { device_->DestroyDescriptorSet(frame.shadow_cull_descriptor_set[cascade]); frame.shadow_cull_descriptor_set[cascade] = rhi::handles::INVALID_DESCRIPTOR_SET; }
            if (frame.shadow_depth_descriptor_set[cascade] != rhi::handles::INVALID_DESCRIPTOR_SET) { device_->DestroyDescriptorSet(frame.shadow_depth_descriptor_set[cascade]); frame.shadow_depth_descriptor_set[cascade] = rhi::handles::INVALID_DESCRIPTOR_SET; }
            if (frame.shadow_blit_descriptor_set[cascade] != rhi::handles::INVALID_DESCRIPTOR_SET) { device_->DestroyDescriptorSet(frame.shadow_blit_descriptor_set[cascade]); frame.shadow_blit_descriptor_set[cascade] = rhi::handles::INVALID_DESCRIPTOR_SET; }
        }
    }

    if (shadow_cull_pipeline_ != rhi::handles::INVALID_PIPELINE) { device_->DestroyPipeline(shadow_cull_pipeline_); shadow_cull_pipeline_ = rhi::handles::INVALID_PIPELINE; }
    if (shadow_finalize_pipeline_ != rhi::handles::INVALID_PIPELINE) { device_->DestroyPipeline(shadow_finalize_pipeline_); shadow_finalize_pipeline_ = rhi::handles::INVALID_PIPELINE; }
    if (shadow_depth_pipeline_ != rhi::handles::INVALID_PIPELINE) { device_->DestroyPipeline(shadow_depth_pipeline_); shadow_depth_pipeline_ = rhi::handles::INVALID_PIPELINE; }
    if (shadow_blit_pipeline_ != rhi::handles::INVALID_PIPELINE) { device_->DestroyPipeline(shadow_blit_pipeline_); shadow_blit_pipeline_ = rhi::handles::INVALID_PIPELINE; }
    if (shadow_cull_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) { device_->DestroyPipelineLayout(shadow_cull_layout_); shadow_cull_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT; }
    if (shadow_depth_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) { device_->DestroyPipelineLayout(shadow_depth_layout_); shadow_depth_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT; }
    if (shadow_blit_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) { device_->DestroyPipelineLayout(shadow_blit_layout_); shadow_blit_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT; }
    if (shadow_cull_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device_->DestroyDescriptorSetLayout(shadow_cull_set_layout_); shadow_cull_set_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT; }
    if (shadow_depth_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device_->DestroyDescriptorSetLayout(shadow_depth_set_layout_); shadow_depth_set_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT; }
    if (shadow_blit_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) { device_->DestroyDescriptorSetLayout(shadow_blit_set_layout_); shadow_blit_set_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT; }

    if (gbuffer_depth_blit_descriptor_set_ != rhi::handles::INVALID_DESCRIPTOR_SET) {
        device_->DestroyDescriptorSet(gbuffer_depth_blit_descriptor_set_);
        gbuffer_depth_blit_descriptor_set_ = rhi::handles::INVALID_DESCRIPTOR_SET;
    }
    if (gbuffer_depth_blit_cb_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(gbuffer_depth_blit_cb_);
        gbuffer_depth_blit_cb_ = rhi::handles::INVALID_RESOURCE;
    }

    shadow_initialized_ = false;
}

namespace {
    /// Extract 6 frustum planes from a VP matrix (row-major, Metal convention).
    /// Planes are normalized: (xyz=normal, w=signed distance from origin).
    void ExtractFrustumPlanes(const math::m4x4& vp, math::v4 planes[6]) {
        // Left
        planes[0] = math::v4{
            vp.columns[0][3] + vp.columns[0][0],
            vp.columns[1][3] + vp.columns[1][0],
            vp.columns[2][3] + vp.columns[2][0],
            vp.columns[3][3] + vp.columns[3][0]
        };
        // Right
        planes[1] = math::v4{
            vp.columns[0][3] - vp.columns[0][0],
            vp.columns[1][3] - vp.columns[1][0],
            vp.columns[2][3] - vp.columns[2][0],
            vp.columns[3][3] - vp.columns[3][0]
        };
        // Bottom
        planes[2] = math::v4{
            vp.columns[0][3] + vp.columns[0][1],
            vp.columns[1][3] + vp.columns[1][1],
            vp.columns[2][3] + vp.columns[2][1],
            vp.columns[3][3] + vp.columns[3][1]
        };
        // Top
        planes[3] = math::v4{
            vp.columns[0][3] - vp.columns[0][1],
            vp.columns[1][3] - vp.columns[1][1],
            vp.columns[2][3] - vp.columns[2][1],
            vp.columns[3][3] - vp.columns[3][1]
        };
        // Near
        planes[4] = math::v4{
            vp.columns[0][2],
            vp.columns[1][2],
            vp.columns[2][2],
            vp.columns[3][2]
        };
        // Far
        planes[5] = math::v4{
            vp.columns[0][3] - vp.columns[0][2],
            vp.columns[1][3] - vp.columns[1][2],
            vp.columns[2][3] - vp.columns[2][2],
            vp.columns[3][3] - vp.columns[3][2]
        };

        // Normalize
        for (int i = 0; i < 6; ++i) {
            float len = sqrtf(planes[i].x * planes[i].x + planes[i].y * planes[i].y + planes[i].z * planes[i].z);
            if (len > 1e-6f) {
                float invLen = 1.0f / len;
                planes[i].x *= invLen;
                planes[i].y *= invLen;
                planes[i].z *= invLen;
                planes[i].w *= invLen;
            }
        }
    }
} // anonymous namespace

bool GPUDrivenDrawPipeline::ExecuteShadowCulling(rhi::RHICommandBuffer* cmd_buffer,
                                                   const RenderSceneSnapshot& scene_snapshot,
                                                   const DirectionalLightData& light_data,
                                                   u32 cascade_index,
                                                   u32 buffer_index) {
    if (!shadow_initialized_ || !cmd_buffer) return false;
    if (cascade_index > 1) return false;

    // T4.6.5 part 24.1 (B4 fix): mirror Execute's empty-scene guard. Without
    // this, ExecuteShadowRaster's "geometry buffers not ready" path fires per
    // cascade per frame even though Execute() is now clean.
    if (scene_snapshot.GetInstanceCount() == 0) return true;

    // Ensure global geometry buffers are built before shadow passes run
    UpdateGeometryData(scene_snapshot);

    u32 bi = buffer_index % 3;
    auto& frame = shadow_frames_[bi];

    const auto& instances = scene_snapshot.GetInstanceData();
    const math::m4x4& lightVP = (cascade_index == 0) ? light_data.shadowMatrix0 : light_data.shadowMatrix1;

    // --- Extract light frustum planes from VP matrix ---
    math::v4 frustumPlanes[6];
    ExtractFrustumPlanes(lightVP, frustumPlanes);

    // --- CPU frustum culling: instance-level + meshlet-level ---
    auto sphereVsFrustum = [&](const math::v3& center, float radius) -> bool {
        for (int i = 0; i < 6; ++i) {
            float dist = frustumPlanes[i].x*center.x + frustumPlanes[i].y*center.y +
                         frustumPlanes[i].z*center.z + frustumPlanes[i].w;
            if (dist < -radius) return false;
        }
        return true;
    };

    // CPU-side meshlet culling using cached meshlet data.
    // Dawn Storage buffers are GPU-only; MapBuffer returns zeroed staging,
    // and Unmap uploads those zeros back — clobbering the meshlet data.
    u32 totalMeshlets = 0;
    {
        if (!cpu_meshlet_cache_.empty()) {
            const auto* meshlets = cpu_meshlet_cache_.data();

            utl::vector<u32> visibleClusters;
            visibleClusters.reserve(shadow_max_clusters_);

            for (const auto& inst : instances) {
                math::v3 instCenter = inst.bounds_center;
                float instRadius = inst.bounds_radius;

                // Instance-level cull
                if (!sphereVsFrustum(instCenter, instRadius)) continue;

                // Per-cluster cull
                for (u32 c = 0; c < inst.cluster_count; ++c) {
                    u32 globalIdx = inst.cluster_start + c;
                    if (globalIdx >= cpu_meshlet_cache_.size()) continue;
                    const auto& meshlet = meshlets[globalIdx];

                    // Transform meshlet center to world space
                    math::v3 localCenter{meshlet.center[0], meshlet.center[1], meshlet.center[2]};
                    math::v4 worldCenter4 = inst.world_matrix * math::v4{localCenter.x, localCenter.y, localCenter.z, 1.0f};
                    math::v3 worldCenter{worldCenter4.x, worldCenter4.y, worldCenter4.z};

                    if (!sphereVsFrustum(worldCenter, meshlet.radius)) continue;

                    visibleClusters.push_back(globalIdx);
                }
            }

            // Sort for deterministic ordering
            std::sort(visibleClusters.begin(), visibleClusters.end());

            totalMeshlets = static_cast<u32>(visibleClusters.size());

            // TEMP DIAGNOSTIC
            static u32 cullDiag = 0;
            if (cullDiag < 6) {
                std::cout << "[CullDiag] cascade=" << cascade_index << " instances=" << instances.size()
                          << " visibleClusters=" << totalMeshlets << std::endl;
                cullDiag++;
            }

            // Write visible clusters to GPU buffer
            void* clusterMapped = device_->MapBuffer(frame.visible_clusters_buffer[cascade_index],
                                                      0, totalMeshlets * sizeof(u32));
            if (clusterMapped) {
                memcpy(clusterMapped, visibleClusters.data(), totalMeshlets * sizeof(u32));
                device_->UnmapBuffer(frame.visible_clusters_buffer[cascade_index]);
            }
        }
    }

    // Set indirect draw args
    {
        void* args = device_->MapBuffer(frame.indirect_draw_buffer[cascade_index], 0, sizeof(u32) * 4);
        if (args) {
            u32* p = static_cast<u32*>(args);
            p[0] = 126 * 3;           // vertexCount
            p[1] = totalMeshlets;      // instanceCount
            p[2] = 0;                  // vertexStart
            p[3] = 0;                  // baseInstance
            device_->UnmapBuffer(frame.indirect_draw_buffer[cascade_index]);
        }
    }

    return true;
}

bool GPUDrivenDrawPipeline::ExecuteShadowRaster(rhi::RHICommandBuffer* cmd_buffer,
                                                  const math::m4x4& light_view_projection,
                                                  u32 cascade_index,
                                                  u32 buffer_index) {
    if (!shadow_initialized_ || !cmd_buffer) return false;
    if (cascade_index > 1) return false;

    // TEMP DIAGNOSTIC
    static u32 rasterDiag = 0;
    if (rasterDiag < 4) {
        std::cout << "[RasterDiag] cascade=" << cascade_index
                  << " pipeline=" << shadow_depth_pipeline_
                  << " layout=" << shadow_depth_layout_ << std::endl;
        rasterDiag++;
    }

    u32 bi = buffer_index % 3;
    auto& frame = shadow_frames_[bi];
    auto& ds = frame.shadow_depth_descriptor_set[cascade_index];
    auto& depthRT = (cascade_index == 0) ? frame.shadow_depth_rt_0 : frame.shadow_depth_rt_1;

    // Validate critical geometry buffers before proceeding.
    // T4.6.5 part 24.1 (B4 fix): post-empty-scene-guard, INVALID buffers here
    // means the cascade from ExecuteShadowCulling's empty-scene early-return —
    // not a real error. Silently skip; logging every frame floods production
    // output. Real "geometry not ready" bugs surface via Stage3's explicit
    // INVALID-buffer guard which still logs.
    if (cluster_map_buffer_ == rhi::handles::INVALID_RESOURCE ||
        global_instance_data_buffer_ == rhi::handles::INVALID_RESOURCE ||
        global_meshlet_buffer_ == rhi::handles::INVALID_RESOURCE ||
        global_meshlet_vertices_buffer_ == rhi::handles::INVALID_RESOURCE ||
        global_meshlet_triangles_buffer_ == rhi::handles::INVALID_RESOURCE ||
        global_vertex_buffer_ == rhi::handles::INVALID_RESOURCE) {
        return true;
    }

    // Upload ShadowDepthUniforms
    struct ShadowDepthCB {
        float light_view_projection[16];
        u32 visible_cluster_count;
        u32 padding[3];
    } depthCB;

    // Copy m4x4 to float[16] (column-major)
    memcpy(depthCB.light_view_projection, &light_view_projection, sizeof(float) * 16);
    depthCB.visible_cluster_count = shadow_max_clusters_; // Will be clamped by indirect draw
    depthCB.padding[0] = depthCB.padding[1] = depthCB.padding[2] = 0;

    void* mapped = device_->MapBuffer(frame.shadow_depth_cb[cascade_index]);
    if (mapped) {
        memcpy(mapped, &depthCB, sizeof(ShadowDepthCB));
        device_->UnmapBuffer(frame.shadow_depth_cb[cascade_index]);
    }

    // Update descriptor set (8 bindings)
    rhi::DescriptorBufferInfo bufInfos[8];
    rhi::WriteDescriptorSet writes[8];
    for (int i = 0; i < 8; ++i) {
        writes[i] = {};  // Zero-initialize to avoid uninitialized fields
        writes[i].dstSet = ds;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = rhi::DescriptorType::StorageBuffer;
        writes[i].bufferInfo = &bufInfos[i];
    }
    writes[0].descriptorType = rhi::DescriptorType::UniformBuffer;

    bufInfos[0] = { frame.shadow_depth_cb[cascade_index], 0, sizeof(ShadowDepthCB) };
    bufInfos[1] = { frame.visible_clusters_buffer[cascade_index], 0, ~0ULL };
    bufInfos[2] = { cluster_map_buffer_, 0, ~0ULL };
    bufInfos[3] = { global_instance_data_buffer_, 0, ~0ULL };
    bufInfos[4] = { global_meshlet_buffer_, 0, ~0ULL };
    bufInfos[5] = { global_meshlet_vertices_buffer_, 0, ~0ULL };
    bufInfos[6] = { global_meshlet_triangles_buffer_, 0, ~0ULL };
    bufInfos[7] = { global_vertex_buffer_, 0, ~0ULL };

    device_->UpdateDescriptorSets(8, writes);

    // Begin depth-only render pass
    rhi::RenderPassDesc rpDesc{};
    rpDesc.depthAttachment.texture = depthRT;
    rpDesc.depthAttachment.format = rhi::DataFormat::D32_Float;
    rpDesc.depthAttachment.loadOp = rhi::LoadAction::Clear;
    rpDesc.depthAttachment.storeOp = rhi::StoreAction::Store;
    rpDesc.depthAttachment.clearValue.depth = 1.0f;

    cmd_buffer->BeginRenderPass(rpDesc);
    cmd_buffer->SetViewport({{0, 0}, {2048.0f, 2048.0f}, 0, 1});
    cmd_buffer->SetScissor({{0, 0}, {2048, 2048}});

    cmd_buffer->BindGraphicsPipeline(shadow_depth_pipeline_);
    rhi::DescriptorSetHandle dsHandle = ds;
    cmd_buffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, shadow_depth_layout_, 0, 1, &dsHandle, 0, nullptr);

    // DrawIndirect using the indirect args from culling pass
    cmd_buffer->DrawIndirect(frame.indirect_draw_buffer[cascade_index], 0, 1);

    cmd_buffer->EndRenderPass();

    return true;
}

bool GPUDrivenDrawPipeline::ExecuteShadowDepthBlit(rhi::RHICommandBuffer* cmd_buffer,
                                                     u32 cascade_index,
                                                     u32 buffer_index) {
    if (!shadow_initialized_ || !cmd_buffer) return false;
    if (cascade_index > 1) return false;

    // T4.6.5 part 24.9 (B7 fix): mirror ExecuteShadowRaster's empty-scene
    // guard. In empty scene, ExecuteShadowRaster early-returns leaving
    // shadow_depth_rt_X in UNDEFINED layout; without this guard, the blit
    // shader reads UNDEFINED depth + writes UNDEFINED shadow_map_X via
    // StorageImage, triggering VUID-vkCmdDraw-None-09600 (2 depth + 1 color
    // layout UNDEFINED errors per frame).
    if (cluster_map_buffer_ == rhi::handles::INVALID_RESOURCE ||
        global_instance_data_buffer_ == rhi::handles::INVALID_RESOURCE ||
        global_meshlet_buffer_ == rhi::handles::INVALID_RESOURCE) {
        return true;
    }

    u32 bi = buffer_index % 3;
    auto& frame = shadow_frames_[bi];
    auto& ds = frame.shadow_blit_descriptor_set[cascade_index];
    auto& depthRT = (cascade_index == 0) ? frame.shadow_depth_rt_0 : frame.shadow_depth_rt_1;
    auto& shadowMap = (cascade_index == 0) ? frame.shadow_map_0 : frame.shadow_map_1;

    // Upload resolution
    struct { u32 x, y; } resolution = {2048, 2048};
    void* mapped = device_->MapBuffer(frame.blit_resolution_cb[cascade_index]);
    if (mapped) {
        memcpy(mapped, &resolution, 8);
        device_->UnmapBuffer(frame.blit_resolution_cb[cascade_index]);
    }

    // Update descriptor set (3 bindings)
    rhi::DescriptorImageInfo srcInfo{ rhi::handles::INVALID_SAMPLER, depthRT, rhi::ResourceState::ShaderResource };
    rhi::DescriptorImageInfo dstInfo{ rhi::handles::INVALID_SAMPLER, shadowMap, rhi::ResourceState::UnorderedAccess };
    rhi::DescriptorBufferInfo resInfo{ frame.blit_resolution_cb[cascade_index], 0, 8 };

    bool isDawn = (device_->GetPlatform() == rhi::RHIPlatform::Dawn ||
                   device_->GetPlatform() == rhi::RHIPlatform::Vulkan);
    rhi::DescriptorType srcType = isDawn ? rhi::DescriptorType::SampledDepthImage
                                         : rhi::DescriptorType::SampledImage;

    rhi::WriteDescriptorSet writes[3];
    writes[0] = { ds, 0, 0, 1, srcType, &srcInfo, nullptr };
    writes[1] = { ds, 1, 0, 1, rhi::DescriptorType::StorageImage, &dstInfo, nullptr };
    writes[2] = { ds, 2, 0, 1, rhi::DescriptorType::UniformBuffer, nullptr, &resInfo };

    device_->UpdateDescriptorSets(3, writes);

    // Dispatch
    u32 groups = (2048 + 7) / 8;
    cmd_buffer->BindComputePipeline(shadow_blit_pipeline_);
    rhi::DescriptorSetHandle dsHandle = ds;
    cmd_buffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute, shadow_blit_layout_, 0, 1, &dsHandle, 0, nullptr);
    cmd_buffer->Dispatch(groups, groups, 1);

    return true;
}

bool GPUDrivenDrawPipeline::ExecuteGBufferDepthBlit(rhi::RHICommandBuffer* cmd_buffer) {
    if (!shadow_initialized_ || !cmd_buffer) return false;
    if (final_depth_texture_ == rhi::handles::INVALID_RESOURCE) return false;
    if (gbuffer_depth_sampleable_ == rhi::handles::INVALID_RESOURCE) return false;

    // Upload resolution
    struct { u32 x, y; } resolution = { visibility_config_.width, visibility_config_.height };
    void* mapped = device_->MapBuffer(gbuffer_depth_blit_cb_);
    if (mapped) {
        memcpy(mapped, &resolution, 8);
        device_->UnmapBuffer(gbuffer_depth_blit_cb_);
    }

    // Update descriptor set
    rhi::DescriptorImageInfo srcInfo{ rhi::handles::INVALID_SAMPLER, final_depth_texture_, rhi::ResourceState::ShaderResource };
    rhi::DescriptorImageInfo dstInfo{ rhi::handles::INVALID_SAMPLER, gbuffer_depth_sampleable_, rhi::ResourceState::UnorderedAccess };
    rhi::DescriptorBufferInfo resInfo{ gbuffer_depth_blit_cb_, 0, 8 };

    bool isDawn = (device_->GetPlatform() == rhi::RHIPlatform::Dawn ||
                   device_->GetPlatform() == rhi::RHIPlatform::Vulkan);
    rhi::DescriptorType srcType = isDawn ? rhi::DescriptorType::SampledDepthImage
                                         : rhi::DescriptorType::SampledImage;

    rhi::WriteDescriptorSet writes[3];
    writes[0] = { gbuffer_depth_blit_descriptor_set_, 0, 0, 1, srcType, &srcInfo, nullptr };
    writes[1] = { gbuffer_depth_blit_descriptor_set_, 1, 0, 1, rhi::DescriptorType::StorageImage, &dstInfo, nullptr };
    writes[2] = { gbuffer_depth_blit_descriptor_set_, 2, 0, 1, rhi::DescriptorType::UniformBuffer, nullptr, &resInfo };

    device_->UpdateDescriptorSets(3, writes);

    u32 groupsX = (visibility_config_.width + 7) / 8;
    u32 groupsY = (visibility_config_.height + 7) / 8;
    cmd_buffer->BindComputePipeline(shadow_blit_pipeline_);
    rhi::DescriptorSetHandle dsHandle = gbuffer_depth_blit_descriptor_set_;
    cmd_buffer->BindDescriptorSets(rhi::PipelineBindPoint::Compute, shadow_blit_layout_, 0, 1, &dsHandle, 0, nullptr);
    cmd_buffer->Dispatch(groupsX, groupsY, 1);

    return true;
}

} // namespace primal::graphics::nanite
