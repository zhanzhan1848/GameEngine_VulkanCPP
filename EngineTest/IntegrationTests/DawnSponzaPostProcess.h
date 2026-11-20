#pragma once

#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "Engine/Common/CoreDiag.h"
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>

namespace primal::tests::dawn_sponza {
using namespace graphics::rhi;

// Byte contracts match the frozen ToneMappingPass.cpp / native shaders.
struct ToneParameters {
    float exposure = 0.1f, autoAdapt = 1.f, key = 0.45f, contrast = 1.f;
    float saturation = 1.15f, vignette = 0.45f, bloomIntensity = 4.f;
    u32 padding = 0;
};
struct AdaptParameters {
    float rateUp, rateDown, minLogLum = -6.f, maxLogLum = 6.f;
    float brightClip = 40.f, cutSnap = 4.f, padding[2]{};
};
static_assert(sizeof(ToneParameters) == 32 && offsetof(ToneParameters, bloomIntensity) == 24);
static_assert(sizeof(AdaptParameters) == 32 && offsetof(AdaptParameters, brightClip) == 16);

// Explicit test-host owner. No shared static pipelines or cross-device state.
// The caller ends the scene pass before Record and submits the same command.
class PostProcess {
public:
    PostProcess() = default;
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;
    ~PostProcess() { Shutdown(); }

    bool Initialize(DawnDevice& device, u32 width, u32 height) {
        if (device_ || !width || !height) return false;
        device_ = &device; width_ = width; height_ = height;
        auto load = [&](const char* path, ShaderStage stage, const char* entry) {
            std::ifstream input(path, std::ios::binary);
            const std::string code((std::istreambuf_iterator<char>(input)), {});
            return code.empty() ? handles::INVALID_SHADER : device.CreateShader(code.data(), code.size(), stage, entry);
        };
        shaders_[0] = load("Engine/Graphics/Dawn/shaders/EyeAdaptation.wgsl", ShaderStage::Compute, "eye_adaptation");
        shaders_[1] = load("Engine/Graphics/Dawn/shaders/ToneMapping.wgsl", ShaderStage::Vertex, "tonemap_vs");
        shaders_[2] = load("Engine/Graphics/Dawn/shaders/ToneMapping.wgsl", ShaderStage::Pixel, "tonemap_fs");
        for (auto shader : shaders_) if (shader == handles::INVALID_SHADER) return false;

        DescriptorSetLayoutBinding adapt[4]{};
        adapt[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr};
        adapt[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr};
        adapt[1].unfilterableFloat = true;
        adapt[2] = {2, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr};
        adapt[2].format = DataFormat::R32_Float;
        adapt[3] = {3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr};
        layouts_[0] = device.CreateDescriptorSetLayout({4, adapt});
        DescriptorSetLayoutBinding tone[8]{};
        for (u32 i = 0; i < 8; ++i) {
            tone[i] = {i, i == 2 ? DescriptorType::Sampler : i == 6 ? DescriptorType::UniformBuffer
                : DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};
        }
        tone[7].unfilterableFloat = true;
        layouts_[1] = device.CreateDescriptorSetLayout({8, tone});
        for (u32 i = 0; i < 2; ++i) {
            if (layouts_[i] == handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;
            PipelineLayoutDesc desc{}; desc.setLayouts = &layouts_[i]; desc.setLayoutCount = 1;
            pipelineLayouts_[i] = device.CreatePipelineLayout(desc);
            if (pipelineLayouts_[i] == handles::INVALID_PIPELINE_LAYOUT) return false;
        }
        ComputePipelineDesc cp{}; cp.computeShader = shaders_[0]; cp.layout = pipelineLayouts_[0];
        cp.threadGroupSize = {16,16,1};
        pipelines_[0] = device.CreateComputePipeline(cp);
        GraphicsPipelineDesc gp{}; gp.vertexShader = shaders_[1]; gp.pixelShader = shaders_[2];
        gp.layout = pipelineLayouts_[1]; gp.renderTargetCount = 1;
        gp.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        gp.enableDepthTest = false; gp.enableDepthWrite = false; gp.cullMode = CullMode::None;
        pipelines_[1] = device.CreateGraphicsPipeline(gp);
        for (auto pipe : pipelines_) if (pipe == handles::INVALID_PIPELINE) return false;
        SamplerDesc sampler{}; sampler.minFilter = sampler.magFilter = FilterMode::Linear;
        sampler.addressU = sampler.addressV = sampler.addressW = TextureAddressMode::Clamp;
        sampler.comparisonFunc = ComparisonFunc::Never;
        sampler_ = device.CreateSampler(sampler);
        if (sampler_ == handles::INVALID_SAMPLER) return false;
        TextureDesc td{}; td.size = {1,1,1}; td.type = TextureType::Texture2D;
        td.format = DataFormat::RGBA8_UNorm; td.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
        black_ = device.CreateTexture(td);
        if (black_ == handles::INVALID_RESOURCE) return false;
        const unsigned char black[4]{};
        device.UpdateTextureData(black_, black, 0,0,0,1,1,1,4);
        td.format = DataFormat::R32_Float;
        td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess | TextureUsage::CopyDest | TextureUsage::CopySource;
        for (auto& state : states_) {
            state = device.CreateTexture(td);
            if (state == handles::INVALID_RESOURCE) return false;
            const float zero = 0;
            device.UpdateTextureData(state, &zero, 0,0,0,1,1,1,4);
        }
        for (u32 frame = 0; frame < 3; ++frame) for (u32 pass = 0; pass < 2; ++pass) {
            BufferDesc bd{}; bd.size = 32; bd.type = BufferType::Constant; bd.memoryUsage = GPUMemoryUsage::Dynamic;
            buffers_[frame][pass] = device.CreateBuffer(bd);
            DescriptorSetDesc ds{}; ds.layout = layouts_[pass];
            sets_[frame][pass] = device.CreateDescriptorSet(ds);
            if (buffers_[frame][pass] == handles::INVALID_RESOURCE || sets_[frame][pass] == handles::INVALID_DESCRIPTOR_SET) return false;
        }
        return true;
    }

    bool Record(DawnCommandBuffer& cmd, ResourceHandle hdr, ResourceHandle target, u32 frame,
                const ToneParameters& tone, const AdaptParameters& adapt, ResourceHandle bloom = handles::INVALID_RESOURCE) {
        const u32 slot = frame % 3, next = 1u - ping_;
        if (!device_ || hdr == handles::INVALID_RESOURCE || target == handles::INVALID_RESOURCE) return false;
        if (!device_->UpdateBufferData(buffers_[slot][0], &adapt, sizeof(adapt)) ||
            !device_->UpdateBufferData(buffers_[slot][1], &tone, sizeof(tone))) return false;
        DescriptorImageInfo ai[3]{{sampler_,hdr,ResourceState::ShaderResource},
            {sampler_,states_[ping_],ResourceState::ShaderResource},
            {sampler_,states_[next],ResourceState::UnorderedAccess}};
        DescriptorBufferInfo abi{buffers_[slot][0],0,sizeof(adapt)};
        WriteDescriptorSet aw[4]{};
        for (u32 i = 0; i < 4; ++i) {
            aw[i].dstSet = sets_[slot][0]; aw[i].dstBinding = i; aw[i].descriptorCount = 1;
            aw[i].descriptorType = i == 3 ? DescriptorType::UniformBuffer : i == 2 ? DescriptorType::StorageImage : DescriptorType::SampledImage;
            if (i < 3) aw[i].imageInfo = &ai[i]; else aw[i].bufferInfo = &abi;
        }
        device_->UpdateDescriptorSets(4, aw);
        cmd.BindComputePipeline(pipelines_[0]);
        cmd.BindDescriptorSets(PipelineBindPoint::Compute, pipelineLayouts_[0], 0, 1, &sets_[slot][0], 0, nullptr);
        cmd.Dispatch(1,1,1);
        ping_ = next;
        // WebGPU establishes visibility when the compute pass ends and the
        // render pass begins. The two ping-pong textures never alias.
        ResourceHandle textures[8]{hdr, bloom == handles::INVALID_RESOURCE ? black_ : bloom,
            black_, black_, black_, black_, black_, states_[ping_]};
        DescriptorImageInfo ti[8]{};
        DescriptorBufferInfo tbi{buffers_[slot][1],0,sizeof(tone)};
        WriteDescriptorSet tw[8]{};
        for (u32 i = 0; i < 8; ++i) {
            ti[i] = {sampler_, textures[i], ResourceState::ShaderResource};
            tw[i].dstSet = sets_[slot][1]; tw[i].dstBinding = i; tw[i].descriptorCount = 1;
            tw[i].descriptorType = i == 2 ? DescriptorType::Sampler : i == 6 ? DescriptorType::UniformBuffer : DescriptorType::SampledImage;
            if (i == 6) tw[i].bufferInfo = &tbi; else tw[i].imageInfo = &ti[i];
        }
        device_->UpdateDescriptorSets(8, tw);
        RenderPassDesc pass{}; pass.colorAttachments.resize(1);
        pass.colorAttachments[0].texture = target; pass.colorAttachments[0].format = DataFormat::BGRA8_UNorm;
        pass.colorAttachments[0].loadOp = LoadAction::Clear; pass.colorAttachments[0].storeOp = StoreAction::Store;
        pass.colorAttachments[0].clearValue.color = {0,0,0,1};
        pass.viewport = {{0,0},{float(width_),float(height_)},0,1};
        pass.scissor = {{0,0},{width_,height_}};
        cmd.BeginRenderPass(pass);
        cmd.BindGraphicsPipeline(pipelines_[1]);
        cmd.BindDescriptorSets(PipelineBindPoint::Graphics, pipelineLayouts_[1], 0, 1, &sets_[slot][1], 0, nullptr);
        cmd.Draw(3,0,1,0);
        cmd.EndRenderPass();
        return true;
    }
    ResourceHandle CurrentState() const { return states_[ping_]; }
    void Shutdown() {
        if (!device_) return;
        for (auto& row : sets_) for (auto& h : row) { if (h != handles::INVALID_DESCRIPTOR_SET) device_->DestroyDescriptorSet(h); h = handles::INVALID_DESCRIPTOR_SET; }
        for (auto& row : buffers_) for (auto& h : row) { if (h != handles::INVALID_RESOURCE) device_->DestroyBuffer(h); h = handles::INVALID_RESOURCE; }
        for (auto& h : states_) { if (h != handles::INVALID_RESOURCE) device_->DestroyTexture(h); h = handles::INVALID_RESOURCE; }
        if (black_ != handles::INVALID_RESOURCE) device_->DestroyTexture(black_);
        if (sampler_ != handles::INVALID_SAMPLER) device_->DestroySampler(sampler_);
        black_ = handles::INVALID_RESOURCE; sampler_ = handles::INVALID_SAMPLER;
        for (auto& h : pipelines_) { if (h != handles::INVALID_PIPELINE) device_->DestroyPipeline(h); h = handles::INVALID_PIPELINE; }
        for (auto& h : pipelineLayouts_) { if (h != handles::INVALID_PIPELINE_LAYOUT) device_->DestroyPipelineLayout(h); h = handles::INVALID_PIPELINE_LAYOUT; }
        for (auto& h : layouts_) { if (h != handles::INVALID_DESCRIPTOR_SET_LAYOUT) device_->DestroyDescriptorSetLayout(h); h = handles::INVALID_DESCRIPTOR_SET_LAYOUT; }
        for (auto& h : shaders_) { if (h != handles::INVALID_SHADER) device_->DestroyShader(h); h = handles::INVALID_SHADER; }
        width_ = height_ = ping_ = 0;
        device_ = nullptr;
    }
private:
    DawnDevice* device_ = nullptr;
    u32 width_ = 0, height_ = 0, ping_ = 0;
    std::array<ShaderHandle,3> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER,handles::INVALID_SHADER};
    std::array<PipelineHandle,2> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
    std::array<PipelineLayoutHandle,2> pipelineLayouts_{handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT};
    std::array<DescriptorSetLayoutHandle,2> layouts_{handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    std::array<ResourceHandle,2> states_{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};
    ResourceHandle black_ = handles::INVALID_RESOURCE;
    SamplerHandle sampler_ = handles::INVALID_SAMPLER;
    ResourceHandle buffers_[3][2]{{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE},{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE},{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE}};
    DescriptorSetHandle sets_[3][2]{{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET},{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET},{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET}};
};
} // namespace primal::tests::dawn_sponza
