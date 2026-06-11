#include "FluidRenderPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderGraph/RenderGraphPass.h"
#include "Graphics/RenderGraph/RenderGraphResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>
#include <cmath>

namespace primal::graphics::fluid {

using namespace rhi;
using namespace rendergraph;

namespace {

struct DescData {
    u32 binding;
    DescriptorType type;
    ResourceHandle resource;
    u32 count = 1;
};

static void UpdateDesc(RHIDeviceBase* device, DescriptorSetHandle set,
                       const DescData* params, u32 count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorImageInfo> imageInfos(count);
    std::vector<DescriptorBufferInfo> bufferInfos(count);

    for (u32 i = 0; i < count; ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = params[i].binding;
        writes[i].descriptorCount = params[i].count;
        writes[i].descriptorType = params[i].type;

        if (params[i].type == DescriptorType::UniformBuffer ||
            params[i].type == DescriptorType::StorageBuffer) {
            bufferInfos[i].buffer = params[i].resource;
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = ~0ull;
            writes[i].bufferInfo = &bufferInfos[i];
        } else {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

static const std::string SHADER_DIR =
    "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/Engine/Graphics/Metal/shaders/Fluid/";

static std::string ReadFileToString(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static std::vector<u8> LoadShaderBytecode(const char* shaderName, const char* entry,
                                           ShaderStage stage) {
    std::string path = SHADER_DIR + shaderName + std::string(".metal");
    std::string source = ReadFileToString(path);
    if (source.empty()) {
        std::cerr << "[FluidRenderPass] Failed to load shader: " << shaderName << std::endl;
        return {};
    }
    return std::vector<u8>(source.begin(), source.end());
}

struct FluidPassData {
    RGResourceHandle fluid_output;
};

} // anonymous namespace

// ============================================================================
// FluidRenderPass Implementation
// ============================================================================

FluidRenderPass::~FluidRenderPass() { Shutdown(); }

bool FluidRenderPass::Initialize(RHIDeviceBase* device, u32 render_width, u32 render_height,
                                  const FluidConfig& config) {
    if (initialized_) return true;
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    config_ = config;

    CreateDescriptorSetLayouts();
    CreatePipelines();
    CreateConstantBuffers();
    CreateFluidTextures();

    initialized_ = true;

    std::cout << "[FluidRenderPass] Initialized ("
              << render_width << "x" << render_height << ")" << std::endl;
    return true;
}

void FluidRenderPass::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;
    device_ = nullptr;
}

void FluidRenderPass::CreateDescriptorSetLayouts() {
    // Splat (graphics): 1 SSBO (particles) + 1 UBO (FluidParams)
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex, nullptr},
            {1, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex | ShaderStage::Pixel, nullptr},
        };
        splat_set_layout_ = device_->CreateDescriptorSetLayout({2, bindings});
    }

    // Smooth (compute): 1 sampled + 1 storage + 1 UBO
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        smooth_set_layout_ = device_->CreateDescriptorSetLayout({3, bindings});
    }

    // Normal (compute): 1 sampled + 1 storage + 1 UBO
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        normal_set_layout_ = device_->CreateDescriptorSetLayout({3, bindings});
    }

    // Shade (compute): 4 sampled + 1 storage + 1 UBO
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {3, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr},
            {4, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr},
            {0, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr},
        };
        shade_set_layout_ = device_->CreateDescriptorSetLayout({6, bindings});
    }

    // Triple-buffered descriptor sets
    for (int i = 0; i < 3; i++) {
        splat_ds_[i] = device_->CreateDescriptorSet({splat_set_layout_});
        smooth_ds_[i] = device_->CreateDescriptorSet({smooth_set_layout_});
        normal_ds_[i] = device_->CreateDescriptorSet({normal_set_layout_});
        shade_ds_[i] = device_->CreateDescriptorSet({shade_set_layout_});
    }
}

void FluidRenderPass::CreatePipelines() {
    // Splat — graphics pipeline (vertex + fragment)
    {
        auto code = LoadShaderBytecode("FluidSplat", "fluid_splat_vertex", ShaderStage::Vertex);
        auto vs = (code.empty()) ? handles::INVALID_SHADER :
            device_->CreateShader(code.data(), code.size(), ShaderStage::Vertex, "fluid_splat_vertex");
        auto fs = (code.empty()) ? handles::INVALID_SHADER :
            device_->CreateShader(code.data(), code.size(), ShaderStage::Pixel, "fluid_splat_fragment");

        PipelineLayoutDesc plDesc{};
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &splat_set_layout_;
        splat_layout_ = device_->CreatePipelineLayout(plDesc);

        GraphicsPipelineDesc pipeDesc{};
        pipeDesc.layout = splat_layout_;
        pipeDesc.vertexShader = vs;
        pipeDesc.pixelShader = fs;
        pipeDesc.renderTargetFormats[0] = DataFormat::R16_Float;
        pipeDesc.renderTargetCount = 1;
        pipeDesc.depthStencilFormat = DataFormat::D32_Float;
        pipeDesc.enableDepthTest = true;
        pipeDesc.enableDepthWrite = true;
        pipeDesc.depthFunc = ComparisonFunc::Less;
        pipeDesc.enableBlend = true;
        pipeDesc.srcColorBlendFactor = BlendFactor::One;
        pipeDesc.dstColorBlendFactor = BlendFactor::One;
        pipeDesc.colorBlendOp = BlendOp::Add;
        pipeDesc.cullMode = CullMode::None;
        pipeDesc.vertexAttributes.clear();
        pipeDesc.vertexBindings.clear();
        splat_pipeline_ = device_->CreateGraphicsPipeline(pipeDesc);
    }

    // Smooth — compute pipeline
    {
        auto code = LoadShaderBytecode("FluidSmooth", "fluid_smooth", ShaderStage::Compute);
        auto shader = (code.empty()) ? handles::INVALID_SHADER :
            device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, "fluid_smooth");

        PipelineLayoutDesc plDesc{};
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &smooth_set_layout_;
        smooth_layout_ = device_->CreatePipelineLayout(plDesc);

        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = shader;
        pipeDesc.layout = smooth_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        smooth_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }

    // Normal — compute pipeline
    {
        auto code = LoadShaderBytecode("FluidNormal", "fluid_normal", ShaderStage::Compute);
        auto shader = (code.empty()) ? handles::INVALID_SHADER :
            device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, "fluid_normal");

        PipelineLayoutDesc plDesc{};
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &normal_set_layout_;
        normal_layout_ = device_->CreatePipelineLayout(plDesc);

        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = shader;
        pipeDesc.layout = normal_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        normal_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }

    // Shade — compute pipeline
    {
        auto code = LoadShaderBytecode("FluidShade", "fluid_shade", ShaderStage::Compute);
        auto shader = (code.empty()) ? handles::INVALID_SHADER :
            device_->CreateShader(code.data(), code.size(), ShaderStage::Compute, "fluid_shade");

        PipelineLayoutDesc plDesc{};
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &shade_set_layout_;
        shade_layout_ = device_->CreatePipelineLayout(plDesc);

        ComputePipelineDesc pipeDesc{};
        pipeDesc.computeShader = shader;
        pipeDesc.layout = shade_layout_;
        pipeDesc.threadGroupSize = {8, 8, 1};
        shade_pipeline_ = device_->CreateComputePipeline(pipeDesc);
    }
}

void FluidRenderPass::CreateConstantBuffers() {
    for (int i = 0; i < 3; i++) {
        BufferDesc desc{};
        desc.size = 256;
        desc.type = BufferType::Constant;
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        params_cb_[i] = device_->CreateBuffer(desc);
    }
}

void FluidRenderPass::CreateFluidTextures() {
    u32 half_w = render_width_ / 2;
    u32 half_h = render_height_ / 2;

    for (int i = 0; i < 3; i++) {
        // Depth (R32_Float)
        {
            TextureDesc desc{};
            desc.size = {half_w, half_h, 1};
            desc.format = DataFormat::D32_Float;
            desc.type = TextureType::Texture2D;
            desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess | TextureUsage::DepthStencil;
            desc.memoryUsage = GPUMemoryUsage::Static;
            desc.name = "FluidDepth";
            fluid_depth_[i] = device_->CreateTexture(desc);
        }
        // Smoothed depth (R32_Float)
        {
            TextureDesc desc{};
            desc.size = {half_w, half_h, 1};
            desc.format = DataFormat::R32_Float;
            desc.type = TextureType::Texture2D;
            desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            desc.memoryUsage = GPUMemoryUsage::Static;
            desc.name = "FluidDepthSmooth";
            fluid_depth_smooth_[i] = device_->CreateTexture(desc);
        }
        // Thickness (R16_Float)
        {
            TextureDesc desc{};
            desc.size = {half_w, half_h, 1};
            desc.format = DataFormat::R16_Float;
            desc.type = TextureType::Texture2D;
            desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            desc.memoryUsage = GPUMemoryUsage::Static;
            desc.name = "FluidThickness";
            fluid_thickness_[i] = device_->CreateTexture(desc);
        }
        // Normal (RGBA8_UNorm)
        {
            TextureDesc desc{};
            desc.size = {half_w, half_h, 1};
            desc.format = DataFormat::RGBA8_UNorm;
            desc.type = TextureType::Texture2D;
            desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            desc.memoryUsage = GPUMemoryUsage::Static;
            desc.name = "FluidNormal";
            fluid_normal_[i] = device_->CreateTexture(desc);
        }
        // Output color (RGBA16_Float)
        {
            TextureDesc desc{};
            desc.size = {half_w, half_h, 1};
            desc.format = DataFormat::RGBA16_Float;
            desc.type = TextureType::Texture2D;
            desc.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess;
            desc.memoryUsage = GPUMemoryUsage::Static;
            desc.name = "FluidColor";
            fluid_color_[i] = device_->CreateTexture(desc);
        }
    }
}

// ============================================================================
// AddPass
// ============================================================================

FluidOutput FluidRenderPass::AddPass(RenderGraph& graph, const FluidInputs& inputs) {
    FluidOutput output{};
    if (!initialized_ || inputs.particle_count == 0) return output;

    u32 outIdx = 0; // Could use frame index for triple buffering
    u32 half_w = render_width_ / 2;
    u32 half_h = render_height_ / 2;

    auto fluidHandle = graph.ImportResource("FluidColor_" + std::to_string(outIdx), fluid_color_[outIdx]);
    output.fluid_color = fluidHandle;

    graph.AddPass<FluidPassData>("FluidRender",
        RGPassType::Compute, RGPassCategory::Lighting,

        // Setup
        [inputs, fluidHandle](FluidPassData& data, RenderGraphBuilder& builder) {
            if (inputs.scene_color.IsValid())
                builder.Read(inputs.scene_color, ResourceState::ShaderResource);
            data.fluid_output = builder.Write(fluidHandle, ResourceState::UnorderedAccess);
        },

        // Execute: 4 sub-passes
        [this, inputs, outIdx, half_w, half_h](
            const FluidPassData& data, RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            if (!cmd) return;

            // Upload FluidParams
            {
                auto* mapped = static_cast<FluidParams*>(device_->MapBuffer(params_cb_[outIdx]));
                if (mapped) {
                    FluidParams fp{};

                    fp.CameraPos = {inputs.camera_position.x, inputs.camera_position.y,
                                    inputs.camera_position.z, 0.0f};
                    fp.ScreenParams = {(float)half_w, (float)half_h,
                                       1.0f / (float)half_w, 1.0f / (float)half_h};

                    math::m4x4 vp = inputs.view_matrix * inputs.proj_matrix;
                    math::m4x4 invVp = rhi::math::Inverse(vp);
                    for (int c = 0; c < 4; c++) {
                        fp.InvViewProj[c] = invVp.columns[c];
                        fp.ViewProj[c] = vp.columns[c];
                    }

                    fp.FluidParams1 = {config_.particle_radius, config_.splat_scale,
                                       config_.absorption, config_.ior};
                    fp.FluidParams2 = {config_.smooth_sigma_depth, (float)config_.smooth_iterations,
                                       0.707f, -1.0f}; // light dir x,y
                    fp.FluidParams3 = {0.408f, 5.0f, 5.0f, 5.0f}; // light dir z, color
                    fp.DepthParams = {0.1f, 1000.0f, 0.0f, 0.0f};

                    *mapped = fp;
                    device_->UnmapBuffer(params_cb_[outIdx]);
                }
            }

            // TODO: Sub-pass 1 — FluidSplat (graphics pass with particle buffer)
            // Requires render pass setup with depth + thickness render targets
            // Skipping for now — needs graphics encoder integration

            // Sub-pass 2 — FluidSmooth (compute)
            if (smooth_pipeline_ != handles::INVALID_PIPELINE) {
                DescData params[] = {
                    {0, DescriptorType::SampledImage, fluid_depth_smooth_[outIdx]},
                    {1, DescriptorType::StorageImage,  fluid_depth_[outIdx]},
                    {0, DescriptorType::UniformBuffer, params_cb_[outIdx]},
                };
                UpdateDesc(device_, smooth_ds_[outIdx], params, 3);

                cmd->BindComputePipeline(smooth_pipeline_);
                const DescriptorSetHandle sets[] = { smooth_ds_[outIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, smooth_layout_,
                                        0, 1, sets, 0, nullptr);

                u32 gx = (half_w + 7) / 8;
                u32 gy = (half_h + 7) / 8;
                cmd->Dispatch(gx, gy, 1);
            }

            // Sub-pass 3 — FluidNormal (compute)
            if (normal_pipeline_ != handles::INVALID_PIPELINE) {
                DescData params[] = {
                    {0, DescriptorType::SampledImage, fluid_depth_[outIdx]},
                    {1, DescriptorType::StorageImage,  fluid_normal_[outIdx]},
                    {0, DescriptorType::UniformBuffer, params_cb_[outIdx]},
                };
                UpdateDesc(device_, normal_ds_[outIdx], params, 3);

                cmd->BindComputePipeline(normal_pipeline_);
                const DescriptorSetHandle sets[] = { normal_ds_[outIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, normal_layout_,
                                        0, 1, sets, 0, nullptr);

                u32 gx = (half_w + 7) / 8;
                u32 gy = (half_h + 7) / 8;
                cmd->Dispatch(gx, gy, 1);
            }

            // Barrier: fluid_normal + fluid_depth → SRV for shade
            {
                ResourceBarrier barriers[2];
                barriers[0].resource = fluid_normal_[outIdx];
                barriers[0].beforeState = ResourceState::UnorderedAccess;
                barriers[0].afterState = ResourceState::ShaderResource;
                barriers[0].subresource = 0xFFFFFFFF;
                barriers[1].resource = fluid_depth_[outIdx];
                barriers[1].beforeState = ResourceState::UnorderedAccess;
                barriers[1].afterState = ResourceState::ShaderResource;
                barriers[1].subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(barriers, 2);
            }

            // Sub-pass 4 — FluidShade (compute)
            if (shade_pipeline_ != handles::INVALID_PIPELINE) {
                ResourceHandle sceneTex = handles::INVALID_RESOURCE;
                if (inputs.scene_color.IsValid()) {
                    auto* res = context.graph->GetResource(inputs.scene_color);
                    if (res) sceneTex = res->GetPhysicalHandle();
                }

                DescData params[] = {
                    {0, DescriptorType::SampledImage, fluid_depth_[outIdx]},
                    {1, DescriptorType::SampledImage, fluid_normal_[outIdx]},
                    {2, DescriptorType::SampledImage, fluid_thickness_[outIdx]},
                    {3, DescriptorType::SampledImage, sceneTex},
                    {4, DescriptorType::StorageImage,  fluid_color_[outIdx]},
                    {0, DescriptorType::UniformBuffer, params_cb_[outIdx]},
                };
                UpdateDesc(device_, shade_ds_[outIdx], params, 6);

                cmd->BindComputePipeline(shade_pipeline_);
                const DescriptorSetHandle sets[] = { shade_ds_[outIdx] };
                cmd->BindDescriptorSets(PipelineBindPoint::Compute, shade_layout_,
                                        0, 1, sets, 0, nullptr);

                u32 gx = (half_w + 7) / 8;
                u32 gy = (half_h + 7) / 8;
                cmd->Dispatch(gx, gy, 1);
            }

            // Barrier: fluid_color → SRV
            {
                ResourceBarrier barrier{};
                barrier.resource = fluid_color_[outIdx];
                barrier.beforeState = ResourceState::UnorderedAccess;
                barrier.afterState = ResourceState::ShaderResource;
                barrier.subresource = 0xFFFFFFFF;
                cmd->InsertBarrier(&barrier, 1);
            }
        }
    );

    output.valid = true;
    return output;
}

} // namespace primal::graphics::fluid
