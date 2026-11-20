#pragma once
#include "DawnSponzaTAA.h"
#include "DawnSponzaGBufferFormats.h"
namespace primal::tests::dawn_sponza {
// Borrows immutable GBuffer/shadow/output identities. The host retains them
// until Shutdown; only parameters, descriptors and capture resources are owned.
class DeferredLighting {
public:
    struct Params {
        primal::math::m4x4 inverseVP;
        float camera[4]{},lightDirection[4]{},lightColor[4]{};
    };
    static_assert(sizeof(Params)==112);
    DeferredLighting()=default;
    DeferredLighting(const DeferredLighting&)=delete;
    DeferredLighting& operator=(const DeferredLighting&)=delete;
    ~DeferredLighting(){Shutdown();}
    bool Initialize(DawnDevice& device,u32 width,u32 height,std::array<ResourceHandle,6> textures){
        if(device_||!width||!height)return false;
        const DataFormat formats[]{DataFormat::D32_Float,DataFormat::RGBA16_Float,DataFormat::RGBA16_Float,DataFormat::RGBA8_UNorm,DataFormat::RGBA8_UNorm,DataFormat::RGBA16_Float};
        for(u32 i=0;i<6;++i){
            auto* texture=device.GetTexture(textures[i]);if(!texture)return false;
            const auto& d=texture->GetTextureDesc();
            if(d.type!=TextureType::Texture2D||((i==1||i==2)?!IsGBufferAttributeFormat(d.format):d.format!=formats[i])||!d.size.x||!d.size.y)return false;
            if(i!=4&&(d.size.x!=width||d.size.y!=height))return false;
            for(u32 j=0;j<i;++j)if(textures[i]==textures[j])return false;
            widths_[i]=d.size.x;heights_[i]=d.size.y;bpp_[i]=CapturePixelBytes(d.format);formats_[i]=d.format;
        }
        device_=&device;textures_=textures;width_=width;height_=height;
        DescriptorSetLayoutBinding bindings[8]{};
        for(u32 i=0;i<8;++i)bindings[i]={i,i==0?DescriptorType::SampledDepthImage:i<5?DescriptorType::SampledImage:i==5?DescriptorType::Sampler:i==6?DescriptorType::StorageImage:DescriptorType::UniformBuffer,1,ShaderStage::Compute,nullptr};
        layout_=device.CreateDescriptorSetLayout({8,bindings});if(layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
        PipelineLayoutDesc pl{};pl.setLayouts=&layout_;pl.setLayoutCount=1;pipelineLayout_=device.CreatePipelineLayout(pl);if(pipelineLayout_==handles::INVALID_PIPELINE_LAYOUT)return false;
        std::ifstream file("Engine/Graphics/Dawn/shaders/MeasuredDeferredLighting.wgsl");std::string source((std::istreambuf_iterator<char>(file)),{});
        if(source.empty())return false;
        shader_=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,"measured_deferred_direct");if(shader_==handles::INVALID_SHADER)return false;
        ComputePipelineDesc cp{};cp.layout=pipelineLayout_;cp.computeShader=shader_;pipeline_=device.CreateComputePipeline(cp);if(pipeline_==handles::INVALID_PIPELINE)return false;
        SamplerDesc sd{};sd.comparisonFunc=ComparisonFunc::Never;sd.minFilter=sd.magFilter=sd.mipFilter=FilterMode::Linear;sd.addressU=sd.addressV=sd.addressW=TextureAddressMode::Clamp;
        sampler_=device.CreateSampler(sd);if(sampler_==handles::INVALID_SAMPLER)return false;
        for(u32 slot=0;slot<3;++slot){
            BufferDesc cb{};cb.size=sizeof(Params);cb.type=BufferType::Constant;cb.memoryUsage=GPUMemoryUsage::Static;
            params_[slot]=device.CreateBuffer(cb);sets_[slot]=device.CreateDescriptorSet({layout_});
            if(params_[slot]==handles::INVALID_RESOURCE||sets_[slot]==handles::INVALID_DESCRIPTOR_SET)return false;
            DescriptorImageInfo images[7]{};DescriptorBufferInfo bi{params_[slot],0,sizeof(Params)};WriteDescriptorSet writes[8]{};
            for(u32 i=0;i<8;++i){
                auto& w=writes[i];w.dstSet=sets_[slot];w.dstBinding=i;w.descriptorCount=1;w.descriptorType=bindings[i].descriptorType;
                if(i==7)w.bufferInfo=&bi;
                else {images[i]={i==5?sampler_:handles::INVALID_SAMPLER,i==5?handles::INVALID_RESOURCE:textures[i==6?5:i],i==6?ResourceState::UnorderedAccess:ResourceState::ShaderResource};w.imageInfo=&images[i];}
            }
            device.UpdateDescriptorSets(8,writes);
        }
        ready_=true;return true;
    }
    bool Record(DawnCommandBuffer& cmd,u32 frame,Params p,const TemporalAA::Raster& raster){
        if(!ready_||frame!=nextFrame_||raster.frame!=frame||!raster.uploaded||raster.width!=width_||raster.height!=height_||raster.depth!=textures_[0])return false;
        // Frozen deferred reconstruction uses the base inverse, unlike TAA's
        // explicit jitter compensation. Derive it from the uploaded raster.
        p.inverseVP=graphics::rhi::math::Inverse(raster.baseVP);
        for(u32 c=0;c<4;++c)for(u32 r=0;r<4;++r)if(!std::isfinite(primal::math::elem(p.inverseVP,c,r)))return false;
        for(const auto* row:{p.camera,p.lightDirection,p.lightColor})for(u32 i=0;i<4;++i)if(!std::isfinite(row[i]))return false;
        if(p.lightDirection[0]*p.lightDirection[0]+p.lightDirection[1]*p.lightDirection[1]+p.lightDirection[2]*p.lightDirection[2]<=1e-12f)return false;
        const u32 slot=frame%3;if(!device_->UpdateBufferData(params_[slot],&p,sizeof(p)))return false;
        cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::MemoryWrite,AccessFlag::ShaderRead);
        cmd.BindComputePipeline(pipeline_);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayout_,0,1,&sets_[slot],0,nullptr);cmd.Dispatch((width_+7)/8,(height_+7)/8,1);
        cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::AllCommands,AccessFlag::ShaderWrite,AccessFlag::MemoryRead);
        last_=p;++nextFrame_;return true;
    }
    bool Capture(DawnCommandBuffer& cmd){
        if(!nextFrame_||readback_!=handles::INVALID_RESOURCE)return false;
        u64 bytes=0;for(u32 i=0;i<6;++i){pitch_[i]=(widths_[i]*bpp_[i]+255)&~255u;offset_[i]=bytes;bytes+=u64(pitch_[i])*heights_[i];}
        BufferDesc bd{};bd.size=bytes;bd.memoryUsage=GPUMemoryUsage::Readback;readback_=device_->CreateBuffer(bd);if(readback_==handles::INVALID_RESOURCE)return false;
        for(u32 i=0;i<6;++i){BufferTextureCopyRegion r{};r.bufferOffset=offset_[i];r.bufferRowLength=pitch_[i]/bpp_[i];r.bufferImageHeight=heights_[i];r.imageExtent={widths_[i],heights_[i],1};cmd.CopyTextureToBuffer(textures_[i],readback_,&r,1);}
        return true;
    }
    bool Save(const std::string& prefix){
        if(readback_==handles::INVALID_RESOURCE)return false;
        const auto* data=static_cast<const char*>(device_->MapBuffer(readback_,0,offset_[5]+u64(pitch_[5])*heights_[5]));if(!data)return false;
        std::ofstream f(prefix+".bin",std::ios::binary);f.write(reinterpret_cast<const char*>(&last_),sizeof(last_));u32 invalid=0;
        for(u32 i=0;i<6;++i)for(u32 y=0;y<heights_[i];++y){const auto* row=data+offset_[i]+u64(y)*pitch_[i];f.write(row,widths_[i]*bpp_[i]);if(i==5){const auto* values=reinterpret_cast<const u16*>(row);for(u32 x=0;x<width_*4;++x)invalid+=(values[x]&0x7c00u)==0x7c00u;}}
        f.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        std::ofstream meta(prefix+".json");meta<<"{\"frame\":"<<nextFrame_-1<<",\"width\":"<<width_<<",\"height\":"<<height_<<",\"shadow_width\":"<<widths_[4]<<",\"shadow_height\":"<<heights_[4]<<",\"invalid\":"<<invalid<<",\"normal_format\":\""<<CaptureFormatName(formats_[1])<<"\",\"orm_format\":\""<<CaptureFormatName(formats_[2])<<"\",\"layout\":\"Params112; depthD32,normal/orm as formats; albedoRGBA8,shadowRGBA8,directRGBA16F tight rows\"}\n";meta.close();return f.good()&&meta.good()&&!invalid;
    }
    void Shutdown(){
        if(!device_)return;
        for(auto h:sets_)if(h!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(h);
        for(auto h:params_)if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);
        if(readback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(readback_);
        if(sampler_!=handles::INVALID_SAMPLER)device_->DestroySampler(sampler_);
        if(pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipeline_);
        if(pipelineLayout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayout_);
        if(layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layout_);
        if(shader_!=handles::INVALID_SHADER)device_->DestroyShader(shader_);
        device_=nullptr;ready_=false;nextFrame_=0;readback_=handles::INVALID_RESOURCE;sampler_=handles::INVALID_SAMPLER;pipeline_=handles::INVALID_PIPELINE;pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;shader_=handles::INVALID_SHADER;sets_.fill(handles::INVALID_DESCRIPTOR_SET);params_.fill(handles::INVALID_RESOURCE);
    }
private:
    DawnDevice* device_=nullptr;bool ready_=false;u32 width_=0,height_=0,nextFrame_=0;Params last_{};
    std::array<ResourceHandle,6> textures_{};DataFormat formats_[6]{};u32 widths_[6]{},heights_[6]{},bpp_[6]{},pitch_[6]{};u64 offset_[6]{};
    std::array<ResourceHandle,3> params_{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};
    std::array<DescriptorSetHandle,3> sets_{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};
    ResourceHandle readback_=handles::INVALID_RESOURCE;SamplerHandle sampler_=handles::INVALID_SAMPLER;ShaderHandle shader_=handles::INVALID_SHADER;PipelineHandle pipeline_=handles::INVALID_PIPELINE;PipelineLayoutHandle pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;DescriptorSetLayoutHandle layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
};
}
