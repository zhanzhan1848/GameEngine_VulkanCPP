#pragma once
#include "DawnSponzaTAA.h"
#include "DawnSponzaGBufferFormats.h"

namespace primal::tests::dawn_sponza {
// Two ordered native WGSL dispatches on the caller's queue. The owner retains
// its indirect writeback; sampled inputs and the final output are borrowed.
class Fusion {
public:
    enum Input : u32 { SSGI, DDGI, SPGI, Albedo, AO, Direct, Volume, SSR, ORM,
                       Specular, Offscreen, Environment, Visibility, InputCount };
    struct Params {
        float gainMode[4]{1, 1, 0, 0};
        float environment[4]{1, .5f, 0, 1};
    };
    static_assert(sizeof(Params) == 32);
    using Inputs = std::array<ResourceHandle, InputCount>;
    static Inputs EmptyInputs() { Inputs result; result.fill(handles::INVALID_RESOURCE); return result; }
    Fusion() = default;
    Fusion(const Fusion&) = delete;
    Fusion& operator=(const Fusion&) = delete;
    ~Fusion() { Shutdown(); }

    bool Initialize(DawnDevice& device, u32 width, u32 height, ResourceHandle output) {
        if (device_ || !width || !height) return false;
        const auto* texture = device.GetTexture(output);
        if (!texture || texture->GetTextureDesc().format != DataFormat::RGBA16_Float ||
            texture->GetTextureDesc().size.x != width || texture->GetTextureDesc().size.y != height) return false;
        device_ = &device; width_ = width; height_ = height; output_ = output;
        TextureDesc td{}; td.type = TextureType::Texture2D; td.size = {width, height, 1};
        td.format = DataFormat::RGBA16_Float;
        td.usage = TextureUsage::ShaderResource | TextureUsage::UnorderedAccess | TextureUsage::CopySource;
        indirect_ = device.CreateTexture(td);
        td.size = {1, 1, 1}; td.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest | TextureUsage::CopySource;
        black_ = device.CreateTexture(td); volume_ = device.CreateTexture(td);
        if (indirect_ == handles::INVALID_RESOURCE || black_ == handles::INVALID_RESOURCE || volume_ == handles::INVALID_RESOURCE) return false;
        const u16 black[4]{}, volume[4]{0, 0, 0, 0x3c00};
        device.UpdateTextureData(black_, black, 0, 0, 0, 1, 1, 1, 8);
        device.UpdateTextureData(volume_, volume, 0, 0, 0, 1, 1, 1, 8);
        SamplerDesc sd{}; sd.comparisonFunc = ComparisonFunc::Never;
        sd.minFilter = sd.magFilter = FilterMode::Linear;
        sd.addressU = sd.addressV = sd.addressW = TextureAddressMode::Clamp;
        sampler_ = device.CreateSampler(sd);
        if (sampler_ == handles::INVALID_SAMPLER) return false;
        const char* paths[] = {"Engine/Graphics/Dawn/shaders/MeasuredFusionIndirect.wgsl", "Engine/Graphics/Dawn/shaders/MeasuredFusionComposite.wgsl"};
        const char* entries[] = {"measured_fusion_indirect", "measured_fusion_composite"};
        for (u32 stage = 0; stage < 2; ++stage) {
            const u32 count = stage ? 13 : 8, sample = stage ? 7 : 5, uniform = stage ? 10 : 6;
            DescriptorSetLayoutBinding bindings[13]{};
            for (u32 i = 0; i < count; ++i) {
                const auto type = i == count - 1 ? DescriptorType::StorageImage : i == sample ? DescriptorType::Sampler :
                                  i == uniform ? DescriptorType::UniformBuffer : DescriptorType::SampledImage;
                bindings[i] = {i, type, 1, ShaderStage::Compute, nullptr};
                if (i == count - 1) bindings[i].format = DataFormat::RGBA16_Float;
            }
            layouts_[stage] = device.CreateDescriptorSetLayout({count, bindings});
            if (layouts_[stage] == handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;
            PipelineLayoutDesc pl{}; pl.setLayouts = &layouts_[stage]; pl.setLayoutCount = 1;
            pipelineLayouts_[stage] = device.CreatePipelineLayout(pl);
            std::ifstream file(paths[stage]); const std::string source{std::istreambuf_iterator<char>(file), {}};
            if (source.empty() || pipelineLayouts_[stage] == handles::INVALID_PIPELINE_LAYOUT) return false;
            shaders_[stage] = device.CreateShader(source.data(), source.size(), ShaderStage::Compute, entries[stage]);
            if (shaders_[stage] == handles::INVALID_SHADER) return false;
            ComputePipelineDesc cp{}; cp.layout = pipelineLayouts_[stage]; cp.computeShader = shaders_[stage];
            pipelines_[stage] = device.CreateComputePipeline(cp);
            if (pipelines_[stage] == handles::INVALID_PIPELINE) return false;
            for (auto& slot : slots_) {
                BufferDesc cb{}; cb.size = 16; cb.type = BufferType::Constant; cb.memoryUsage = GPUMemoryUsage::Static;
                slot.params[stage] = device.CreateBuffer(cb);
                slot.sets[stage] = device.CreateDescriptorSet({layouts_[stage]});
                if (slot.params[stage] == handles::INVALID_RESOURCE || slot.sets[stage] == handles::INVALID_DESCRIPTOR_SET) return false;
            }
        }
        ready_ = true; return true;
    }

    bool Record(DawnCommandBuffer& cmd, u32 frame, Inputs inputs, const Params& params) {
        if (!ready_ || frame != nextFrame_) return false;
        for (float v : params.gainMode) if (!std::isfinite(v)) return false;
        for (float v : params.environment) if (!std::isfinite(v)) return false;
        for (u32 i : {u32(SSGI), u32(AO), u32(SSR), u32(Offscreen)})
            if (inputs[i] == handles::INVALID_RESOURCE) inputs[i] = black_;
        if (inputs[Volume] == handles::INVALID_RESOURCE) inputs[Volume] = volume_;
        for (u32 i = 0; i < InputCount; ++i) {
            const auto* texture = device_->GetTexture(inputs[i]);
            if (!texture || inputs[i] == indirect_ || inputs[i] == output_) return false;
            const auto& td = texture->GetTextureDesc();
            if (td.type != TextureType::Texture2D || !td.size.x || !td.size.y ||
                !CapturePixelBytes(td.format) || td.format == DataFormat::D32_Float) return false;
            if ((i == Albedo || i == Direct || i == ORM) && (td.size.x != width_ || td.size.y != height_)) return false;
        }
        auto& slot = slots_[frame % slots_.size()];
        for (u32 stage = 0; stage < 2; ++stage) {
            if (!device_->UpdateBufferData(slot.params[stage], stage ? params.environment : params.gainMode, 16)) return false;
            const u32 count = stage ? 13 : 8, sample = stage ? 7 : 5, uniform = stage ? 10 : 6;
            const ResourceHandle indirectTextures[]{inputs[SSGI], inputs[DDGI], inputs[SPGI], inputs[Albedo], inputs[AO],
                                                    handles::INVALID_RESOURCE, handles::INVALID_RESOURCE, indirect_};
            const ResourceHandle compositeTextures[]{inputs[Direct], indirect_, inputs[Volume], inputs[SSR], inputs[ORM], inputs[Albedo],
                inputs[Specular], handles::INVALID_RESOURCE, inputs[Offscreen], inputs[Environment], handles::INVALID_RESOURCE,
                inputs[Visibility], output_};
            const auto* textures = stage ? compositeTextures : indirectTextures;
            DescriptorImageInfo images[13]{}; DescriptorBufferInfo cb{slot.params[stage], 0, 16}; WriteDescriptorSet writes[13]{};
            for (u32 i = 0; i < count; ++i) {
                auto& w = writes[i]; w.dstSet = slot.sets[stage]; w.dstBinding = i; w.descriptorCount = 1;
                w.descriptorType = i == count - 1 ? DescriptorType::StorageImage : i == sample ? DescriptorType::Sampler :
                                   i == uniform ? DescriptorType::UniformBuffer : DescriptorType::SampledImage;
                if (i == uniform) w.bufferInfo = &cb;
                else {
                    images[i] = {i == sample ? sampler_ : handles::INVALID_SAMPLER, textures[i],
                                 i == count - 1 ? ResourceState::UnorderedAccess : ResourceState::ShaderResource};
                    w.imageInfo = &images[i];
                }
            }
            device_->UpdateDescriptorSets(count, writes);
            cmd.MemoryBarrier(PipelineStage::AllCommands, PipelineStage::ComputeShader, AccessFlag::MemoryWrite, AccessFlag::ShaderRead);
            cmd.BindComputePipeline(pipelines_[stage]);
            cmd.BindDescriptorSets(PipelineBindPoint::Compute, pipelineLayouts_[stage], 0, 1, &slot.sets[stage], 0, nullptr);
            cmd.Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
            cmd.MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::AllCommands, AccessFlag::ShaderWrite, AccessFlag::MemoryRead);
        }
        lastInputs_ = inputs; lastParams_ = params; ++nextFrame_; return true;
    }
    ResourceHandle Indirect() const { return ready_ ? indirect_ : handles::INVALID_RESOURCE; }

    bool Capture(DawnCommandBuffer& cmd) {
        if (!ready_ || !nextFrame_ || readback_ != handles::INVALID_RESOURCE) return false;
        u64 bytes = 0;
        for (u32 i = 0; i < CaptureCount; ++i) {
            const auto handle = i < InputCount ? lastInputs_[i] : i == InputCount ? indirect_ : output_;
            const auto& td = device_->GetTexture(handle)->GetTextureDesc();
            auto& image = capture_[i]; image.width = td.size.x; image.height = td.size.y; image.format = td.format;
            image.bpp = CapturePixelBytes(td.format); if (!image.bpp) return false;
            image.pitch = (image.width * image.bpp + 255) & ~255u; image.offset = bytes;
            bytes += u64(image.pitch) * image.height;
        }
        BufferDesc bd{}; bd.size = bytes; bd.memoryUsage = GPUMemoryUsage::Readback;
        readback_ = device_->CreateBuffer(bd); if (readback_ == handles::INVALID_RESOURCE) return false;
        for (u32 i = 0; i < CaptureCount; ++i) {
            const auto& image = capture_[i]; BufferTextureCopyRegion copy{};
            copy.bufferOffset = image.offset; copy.bufferRowLength = image.pitch / image.bpp;
            copy.bufferImageHeight = image.height; copy.imageExtent = {image.width, image.height, 1};
            cmd.CopyTextureToBuffer(i < InputCount ? lastInputs_[i] : i == InputCount ? indirect_ : output_, readback_, &copy, 1);
        }
        return true;
    }
    // Caller must have checked actual submission completion before mapping.
    bool Save(const std::string& prefix) {
        if (readback_ == handles::INVALID_RESOURCE) return false;
        const auto& last = capture_.back();
        const auto* data = static_cast<const char*>(device_->MapBuffer(readback_, 0, last.offset + u64(last.pitch) * last.height));
        if (!data) return false;
        std::ofstream file(prefix + ".bin", std::ios::binary), meta(prefix + ".json");
        file.write(reinterpret_cast<const char*>(&lastParams_), sizeof(lastParams_));
        meta << "{\"frame\":" << nextFrame_ - 1 << ",\"width\":" << width_ << ",\"height\":" << height_
             << ",\"params_bytes\":32,\"images\":[";
        static constexpr const char* names[]{"ssgi", "ddgi", "spgi", "albedo", "ao", "direct", "volume", "ssr", "orm",
                                             "specular", "offscreen", "environment", "visibility", "indirect", "composite"};
        u64 cursor = sizeof(lastParams_); u32 invalid = 0;
        for (u32 i = 0; i < CaptureCount; ++i) {
            const auto& image = capture_[i]; const u64 size = u64(image.width) * image.height * image.bpp;
            meta << (i ? "," : "") << "{\"name\":\"" << names[i] << "\",\"format\":\"" << CaptureFormatName(image.format)
                 << "\",\"width\":" << image.width << ",\"height\":" << image.height << ",\"offset\":" << cursor << ",\"bytes\":" << size << "}";
            cursor += size;
            for (u32 y = 0; y < image.height; ++y) {
                const auto* row = data + image.offset + u64(y) * image.pitch; file.write(row, image.width * image.bpp);
                if (i >= InputCount) {
                    const auto* values = reinterpret_cast<const u16*>(row);
                    for (u32 x = 0; x < image.width * 4; ++x) invalid += (values[x] & 0x7c00u) == 0x7c00u;
                }
            }
        }
        file.close(); meta << "],\"invalid\":" << invalid << ",\"completion_checked_by_caller\":true}\n"; meta.close();
        device_->UnmapBuffer(readback_); device_->DestroyBuffer(readback_); readback_ = handles::INVALID_RESOURCE;
        return file.good() && meta.good() && !invalid;
    }
    void Shutdown() {
        if (!device_) return;
        for (auto& slot : slots_) {
            for (u32 stage = 0; stage < 2; ++stage) {
                if (slot.sets[stage] != handles::INVALID_DESCRIPTOR_SET) device_->DestroyDescriptorSet(slot.sets[stage]);
                if (slot.params[stage] != handles::INVALID_RESOURCE) device_->DestroyBuffer(slot.params[stage]);
            }
            slot = Slot{};
        }
        for (u32 stage = 0; stage < 2; ++stage) {
            if (pipelines_[stage] != handles::INVALID_PIPELINE) device_->DestroyPipeline(pipelines_[stage]);
            if (shaders_[stage] != handles::INVALID_SHADER) device_->DestroyShader(shaders_[stage]);
            if (pipelineLayouts_[stage] != handles::INVALID_PIPELINE_LAYOUT) device_->DestroyPipelineLayout(pipelineLayouts_[stage]);
            if (layouts_[stage] != handles::INVALID_DESCRIPTOR_SET_LAYOUT) device_->DestroyDescriptorSetLayout(layouts_[stage]);
        }
        if (sampler_ != handles::INVALID_SAMPLER) device_->DestroySampler(sampler_);
        for (auto handle : {indirect_, black_, volume_}) if (handle != handles::INVALID_RESOURCE) device_->DestroyTexture(handle);
        if (readback_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(readback_);
        device_ = nullptr; ready_ = false; nextFrame_ = 0;
        output_ = indirect_ = black_ = volume_ = readback_ = handles::INVALID_RESOURCE;
        sampler_ = handles::INVALID_SAMPLER;
        layouts_.fill(handles::INVALID_DESCRIPTOR_SET_LAYOUT);
        pipelineLayouts_.fill(handles::INVALID_PIPELINE_LAYOUT);
        pipelines_.fill(handles::INVALID_PIPELINE); shaders_.fill(handles::INVALID_SHADER);
    }
private:
    struct Slot {
        std::array<ResourceHandle, 2> params{handles::INVALID_RESOURCE, handles::INVALID_RESOURCE};
        std::array<DescriptorSetHandle, 2> sets{handles::INVALID_DESCRIPTOR_SET, handles::INVALID_DESCRIPTOR_SET};
    };
    struct CapturedImage { u32 width{}, height{}, bpp{}, pitch{}; u64 offset{}; DataFormat format{}; };
    static constexpr u32 CaptureCount = InputCount + 2;
    DawnDevice* device_ = nullptr; bool ready_ = false; u32 width_ = 0, height_ = 0, nextFrame_ = 0;
    ResourceHandle output_ = handles::INVALID_RESOURCE, indirect_ = handles::INVALID_RESOURCE, black_ = handles::INVALID_RESOURCE,
                   volume_ = handles::INVALID_RESOURCE, readback_ = handles::INVALID_RESOURCE;
    SamplerHandle sampler_ = handles::INVALID_SAMPLER;
    std::array<Slot, 3> slots_{}; Inputs lastInputs_{}; Params lastParams_{};
    std::array<CapturedImage, CaptureCount> capture_{};
    std::array<DescriptorSetLayoutHandle, 2> layouts_{handles::INVALID_DESCRIPTOR_SET_LAYOUT, handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    std::array<PipelineLayoutHandle, 2> pipelineLayouts_{handles::INVALID_PIPELINE_LAYOUT, handles::INVALID_PIPELINE_LAYOUT};
    std::array<PipelineHandle, 2> pipelines_{handles::INVALID_PIPELINE, handles::INVALID_PIPELINE};
    std::array<ShaderHandle, 2> shaders_{handles::INVALID_SHADER, handles::INVALID_SHADER};
};
}
