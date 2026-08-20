#include "PlanarReflectionModule.h"
#include "Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/Utils/ShaderRegistry.h"

#include <fstream>
#include <iostream>
#include <sstream>

namespace primal::graphics {

using namespace rhi;
using namespace rhi::math;

namespace {

// Householder mirror matrix through the plane (N, P) — same math as
// ForwardRenderer::RenderReflections (kept battle-tested there; this copy
// serves the meshlet path).
math::m4x4 MakePlanarReflectionMatrix(math::v3 N, math::v3 P) {
    float d = -dot(N, P);
    float nx = N.x, ny = N.y, nz = N.z;
    math::m4x4 m;
    m.columns[0] = {1.0f - 2.0f*nx*nx, -2.0f*ny*nx, -2.0f*nz*nx, 0.0f};
    m.columns[1] = {-2.0f*nx*ny, 1.0f - 2.0f*ny*ny, -2.0f*nz*ny, 0.0f};
    m.columns[2] = {-2.0f*nx*nz, -2.0f*ny*nz, 1.0f - 2.0f*nz*nz, 0.0f};
    m.columns[3] = {-2.0f*nx*d, -2.0f*ny*d, -2.0f*nz*d, 1.0f};
    return m;
}

bool ReadShaderBytes(const std::string& relPath, std::vector<u8>& out) {
    {
        std::ifstream f(relPath, std::ios::binary | std::ios::ate);
        if (f.is_open()) {
            std::streamsize sz = f.tellg();
            f.seekg(0, std::ios::beg);
            out.resize(static_cast<size_t>(sz));
            if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
            return true;
        }
    }
    static const std::string fallbackRoot =
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/vulkan-rhi/";
    std::ifstream f(fallbackRoot + relPath, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char*>(out.data()), sz);
    return true;
}

// CompositeParams — must match MirrorComposite.vert/.frag std140 layout.
struct CompositeParamsCB {
    math::m4x4 view_projection;            // 0
    math::m4x4 reflected_view_projection;  // 64
    math::v4 plane_position;               // 128
    math::v4 plane_normal;                 // 144
    math::v4 plane_axes;                   // 160 (xy = half extents)
    math::v4 camera_position;              // 176 (w = reflectivity)
};

} // anonymous namespace

bool PlanarReflectionModule::Initialize(RHIDeviceBase* device,
                                        nanite::GPUDrivenDrawPipeline* gpu_draw,
                                        u32 max_clusters) {
    device_ = device;
    gpu_draw_ = gpu_draw;
    if (!device_ || !gpu_draw_) return false;

    if (!gpu_draw_->InitializeReflectionResources(max_clusters)) {
        std::cerr << "[PlanarReflection] reflection resources init failed" << std::endl;
        return false;
    }

    // 1024² reflection RT + depth
    {
        TextureDesc colorDesc{};
        colorDesc.size = {1024, 1024, 1};
        colorDesc.format = DataFormat::RGBA16_Float;
        colorDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        colorDesc.memoryUsage = GPUMemoryUsage::Static;
        reflection_rt_ = device_->CreateTexture(colorDesc);
        if (reflection_rt_ == handles::INVALID_RESOURCE) return false;

        TextureDesc depthDesc{};
        depthDesc.size = {1024, 1024, 1};
        depthDesc.format = DataFormat::D32_Float;
        depthDesc.usage = TextureUsage::DepthStencil;
        depthDesc.memoryUsage = GPUMemoryUsage::Static;
        reflection_depth_ = device_->CreateTexture(depthDesc);
        if (reflection_depth_ == handles::INVALID_RESOURCE) return false;
    }

    if (!LoadCompositeShaders()) return false;

    initialized_ = true;
    return true;
}

bool PlanarReflectionModule::LoadCompositeShaders() {
    const bool isVulkan = (device_->GetPlatform() == RHIPlatform::Vulkan);

    ShaderHandle vs = handles::INVALID_SHADER;
    ShaderHandle fs = handles::INVALID_SHADER;
    if (isVulkan) {
        std::vector<u8> vsBytes, fsBytes;
        if (!ReadShaderBytes("Engine/Graphics/Vulkan/shaders/PostProcess/MirrorComposite.vert.spv", vsBytes) ||
            !ReadShaderBytes("Engine/Graphics/Vulkan/shaders/PostProcess/MirrorComposite.frag.spv", fsBytes)) {
            std::cerr << "[PlanarReflection] Failed to load MirrorComposite SPIR-V" << std::endl;
            return false;
        }
        vs = device_->CreateShader(vsBytes.data(), vsBytes.size(), ShaderStage::Vertex, "main");
        fs = device_->CreateShader(fsBytes.data(), fsBytes.size(), ShaderStage::Pixel, "main");
    } else {
        std::string path = utils::ShaderRegistry::GetShaderPath(device_->GetPlatform(), "MirrorComposite");
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "[PlanarReflection] Failed to open " << path << std::endl;
            return false;
        }
        std::stringstream buf;
        buf << file.rdbuf();
        std::string src = buf.str();
        vs = device_->CreateShader(src.data(), src.size(), ShaderStage::Vertex, "mirror_composite_vs");
        fs = device_->CreateShader(src.data(), src.size(), ShaderStage::Pixel, "mirror_composite_fs");
    }
    if (vs == handles::INVALID_SHADER || fs == handles::INVALID_SHADER) {
        std::cerr << "[PlanarReflection] Composite shader creation failed" << std::endl;
        return false;
    }

    // 3 bindings: 0 UBO (VS|PS), 1 reflection texture (PS), 2 sampler (PS).
    DescriptorSetLayoutBinding b[3];
    b[0] = {0, DescriptorType::UniformBuffer, 1, ShaderStage::Vertex | ShaderStage::Pixel, nullptr};
    b[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};
    b[2] = {2, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr};
    composite_set_layout_ = device_->CreateDescriptorSetLayout({3, b});
    if (composite_set_layout_ == handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;

    composite_layout_ = device_->CreatePipelineLayout({1, &composite_set_layout_});
    if (composite_layout_ == handles::INVALID_PIPELINE_LAYOUT) return false;

    GraphicsPipelineDesc pd{};
    pd.vertexShader = vs;
    pd.pixelShader = fs;
    pd.layout = composite_layout_;
    pd.topology = PrimitiveTopology::TriangleList;
    pd.renderTargetCount = 1;
    pd.renderTargetFormats[0] = DataFormat::RGBA16_Float;  // HDR scene color
    pd.enableBlend = true;
    pd.srcColorBlendFactor = BlendFactor::SrcAlpha;
    pd.dstColorBlendFactor = BlendFactor::InvSrcAlpha;
    pd.srcAlphaBlendFactor = BlendFactor::One;
    pd.dstAlphaBlendFactor = BlendFactor::InvSrcAlpha;
    pd.enableDepthTest = false;
    pd.enableDepthWrite = false;
    pd.cullMode = CullMode::None;
    pd.vertexAttributes.clear();
    pd.vertexBindings.clear();
    composite_pipeline_ = device_->CreateGraphicsPipeline(pd);
    if (composite_pipeline_ == handles::INVALID_PIPELINE) {
        std::cerr << "[PlanarReflection] Composite pipeline creation failed" << std::endl;
        return false;
    }

    for (u32 f = 0; f < 3; ++f) {
        BufferDesc cbDesc{};
        cbDesc.size = sizeof(CompositeParamsCB);
        cbDesc.type = BufferType::Constant;
        cbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        composite_cb_[f] = device_->CreateBuffer(cbDesc);
        if (composite_cb_[f] == handles::INVALID_RESOURCE) return false;

        DescriptorSetDesc dsDesc;
        dsDesc.layout = composite_set_layout_;
        composite_ds_[f] = device_->CreateDescriptorSet(dsDesc);
        if (composite_ds_[f] == handles::INVALID_DESCRIPTOR_SET) return false;
    }

    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;  // VulkanSampler trap
    composite_sampler_ = device_->CreateSampler(samplerDesc);
    return composite_sampler_ != handles::INVALID_SAMPLER;
}

void PlanarReflectionModule::Shutdown() {
    if (!device_) return;
    if (reflection_rt_ != handles::INVALID_RESOURCE) {
        device_->DestroyTexture(reflection_rt_);
        reflection_rt_ = handles::INVALID_RESOURCE;
    }
    if (reflection_depth_ != handles::INVALID_RESOURCE) {
        device_->DestroyTexture(reflection_depth_);
        reflection_depth_ = handles::INVALID_RESOURCE;
    }
    if (composite_pipeline_ != handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(composite_pipeline_);
        composite_pipeline_ = handles::INVALID_PIPELINE;
    }
    if (composite_layout_ != handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(composite_layout_);
        composite_layout_ = handles::INVALID_PIPELINE_LAYOUT;
    }
    if (composite_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device_->DestroyDescriptorSetLayout(composite_set_layout_);
        composite_set_layout_ = handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    for (u32 f = 0; f < 3; ++f) {
        if (composite_cb_[f] != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(composite_cb_[f]);
            composite_cb_[f] = handles::INVALID_RESOURCE;
        }
        if (composite_ds_[f] != handles::INVALID_DESCRIPTOR_SET) {
            device_->DestroyDescriptorSet(composite_ds_[f]);
            composite_ds_[f] = handles::INVALID_DESCRIPTOR_SET;
        }
    }
    if (composite_sampler_ != handles::INVALID_SAMPLER) {
        device_->DestroySampler(composite_sampler_);
        composite_sampler_ = handles::INVALID_SAMPLER;
    }
    if (gpu_draw_) gpu_draw_->ShutdownReflectionResources();
    initialized_ = false;
}

void PlanarReflectionModule::AddPasses(rendergraph::RenderGraph& graph,
                                       rendergraph::RGResourceHandle hdrColorRG,
                                       const PlanarReflectionInputs& inputs) {
    if (!initialized_ || !enabled_ || !hdrColorRG.IsValid()) return;
    if (composite_pipeline_ == handles::INVALID_PIPELINE) return;

    // Reflected view = mainView × Householder(plane). VP = mainProj × view.
    math::v3 N = Normalize(plane_.normal);
    math::m4x4 reflectedView = inputs.view_matrix * MakePlanarReflectionMatrix(N, plane_.position);
    math::m4x4 reflectedVP = inputs.proj_matrix * reflectedView;

    u32 cbIdx = inputs.current_buffer_index % 3;

    struct PassData {};

    // --- Pass 1: render the reflection into the module RTs ---
    graph.AddPass<PassData>("PlanarReflection_Render",
        rendergraph::RGPassType::Graphics, rendergraph::RGPassCategory::Lighting,
        [](PassData&, rendergraph::RenderGraphBuilder& builder) { builder.SideEffect(); },
        [this, reflectedVP, inputs](const PassData&, rendergraph::RenderGraphContext& context) {
            if (!inputs.scene_snapshot) return;
            gpu_draw_->ExecuteReflectionPass(context.cmdBuffer, *inputs.scene_snapshot,
                                             reflectedVP,
                                             inputs.light_dir, inputs.light_color, inputs.ambient,
                                             reflection_rt_, reflection_depth_,
                                             inputs.current_buffer_index);
        }
    );

    // --- Pass 2: composite the mirror quad over the HDR color (Load) ---
    graph.AddPass<PassData>("PlanarReflection_Composite",
        rendergraph::RGPassType::Graphics, rendergraph::RGPassCategory::PostProcess,
        [hdrColorRG](PassData&, rendergraph::RenderGraphBuilder& builder) {
            builder.Write(hdrColorRG, ResourceState::RenderTarget);
            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({
                .texture = hdrColorRG,
                .loadOp = LoadAction::Load,
                .storeOp = StoreAction::Store
            });
            builder.DeclareRenderPass(rpDesc);
        },
        [this, reflectedVP, inputs, cbIdx, hdrColorRG]
        (const PassData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;

            // The reflection RT is module-internal (outside RG tracking) —
            // the render pass left it in RenderTarget; sampling needs
            // ShaderResource. InsertBarrier derives oldLayout from the
            // tracked state, so repeating this every frame is a safe no-op.
            ResourceBarrier reflBarrier{};
            reflBarrier.resource = reflection_rt_;
            reflBarrier.beforeState = ResourceState::RenderTarget;
            reflBarrier.afterState = ResourceState::ShaderResource;
            reflBarrier.subresource = 0xFFFFFFFF;
            reflBarrier.queueFamily = 0xFFFFFFFF;
            cmd->InsertBarrier(&reflBarrier, 1);

            // Upload composite params.
            {
                void* cb = device_->MapBuffer(composite_cb_[cbIdx], 0, sizeof(CompositeParamsCB));
                if (cb) {
                    auto* p = static_cast<CompositeParamsCB*>(cb);
                    p->view_projection = inputs.proj_matrix * inputs.view_matrix;
                    p->reflected_view_projection = reflectedVP;
                    p->plane_position = math::v4{plane_.position.x, plane_.position.y,
                                                 plane_.position.z, 0.0f};
                    p->plane_normal = math::v4{plane_.normal.x, plane_.normal.y,
                                               plane_.normal.z, 0.0f};
                    p->plane_axes = math::v4{plane_.half_extents.x, plane_.half_extents.y,
                                             0.0f, 0.0f};
                    p->camera_position = math::v4{inputs.camera_position.x,
                                                  inputs.camera_position.y,
                                                  inputs.camera_position.z,
                                                  plane_.reflectivity};
                    device_->UnmapBuffer(composite_cb_[cbIdx]);
                }
            }

            DescriptorBufferInfo cbInfo{composite_cb_[cbIdx], 0, sizeof(CompositeParamsCB)};
            DescriptorImageInfo texInfo;
            texInfo.imageView = reflection_rt_;
            texInfo.imageLayout = ResourceState::ShaderResource;
            DescriptorImageInfo sampInfo;
            sampInfo.sampler = composite_sampler_;

            WriteDescriptorSet writes[3];
            writes[0] = {};
            writes[0].dstSet = composite_ds_[cbIdx];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = DescriptorType::UniformBuffer;
            writes[0].bufferInfo = &cbInfo;
            writes[1] = {};
            writes[1].dstSet = composite_ds_[cbIdx];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = DescriptorType::SampledImage;
            writes[1].imageInfo = &texInfo;
            writes[2] = {};
            writes[2].dstSet = composite_ds_[cbIdx];
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = DescriptorType::Sampler;
            writes[2].imageInfo = &sampInfo;

            device_->UpdateDescriptorSets(3, writes);

            cmd->BindGraphicsPipeline(composite_pipeline_);
            DescriptorSetHandle ds = composite_ds_[cbIdx];
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, composite_layout_, 0, 1, &ds, 0, nullptr);
            cmd->Draw(6, 0, 1, 0);
        }
    );
}

} // namespace primal::graphics
