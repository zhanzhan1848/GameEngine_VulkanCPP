#pragma once
#include "DawnSponzaPostProcess.h"
namespace primal::tests::dawn_sponza {
// Native FinalBlit equivalent: internal LDR -> output viewport. The sampled
// resource is fixed for this owner, so no descriptor is rewritten in flight.
class Present {
public:
    Present()=default;Present(const Present&)=delete;Present& operator=(const Present&)=delete;~Present(){Shutdown();}
    bool Initialize(DawnDevice& device,ResourceHandle source,u32 width,u32 height){
        if(device_||source==handles::INVALID_RESOURCE||!width||!height)return false;device_=&device;width_=width;height_=height;
        std::ifstream f("Engine/Graphics/Dawn/shaders/SponzaPresent.wgsl");std::string code((std::istreambuf_iterator<char>(f)),{});if(code.empty())return false;
        vs_=device.CreateShader(code.data(),code.size(),ShaderStage::Vertex,"present_vs");fs_=device.CreateShader(code.data(),code.size(),ShaderStage::Pixel,"present_fs");if(vs_==handles::INVALID_SHADER||fs_==handles::INVALID_SHADER)return false;
        DescriptorSetLayoutBinding bindings[]{{0,DescriptorType::SampledImage,1,ShaderStage::Pixel,nullptr},{1,DescriptorType::Sampler,1,ShaderStage::Pixel,nullptr}};layout_=device.CreateDescriptorSetLayout({2,bindings});if(layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
        PipelineLayoutDesc ld{};ld.setLayoutCount=1;ld.setLayouts=&layout_;pl_=device.CreatePipelineLayout(ld);if(pl_==handles::INVALID_PIPELINE_LAYOUT)return false;
        GraphicsPipelineDesc pd{};pd.vertexShader=vs_;pd.pixelShader=fs_;pd.layout=pl_;pd.renderTargetCount=1;pd.renderTargetFormats[0]=DataFormat::BGRA8_UNorm;pd.cullMode=CullMode::None;pd.enableDepthTest=false;pd.enableDepthWrite=false;pipeline_=device.CreateGraphicsPipeline(pd);if(pipeline_==handles::INVALID_PIPELINE)return false;
        SamplerDesc sd{};sd.minFilter=sd.magFilter=FilterMode::Linear;sd.addressU=sd.addressV=sd.addressW=TextureAddressMode::Clamp;sd.comparisonFunc=ComparisonFunc::Never;sampler_=device.CreateSampler(sd);set_=device.CreateDescriptorSet({layout_});if(sampler_==handles::INVALID_SAMPLER||set_==handles::INVALID_DESCRIPTOR_SET)return false;
        DescriptorImageInfo image{sampler_,source,ResourceState::ShaderResource};WriteDescriptorSet writes[2]{};for(u32 i=0;i<2;++i){writes[i].dstSet=set_;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=i?DescriptorType::Sampler:DescriptorType::SampledImage;writes[i].imageInfo=&image;}device.UpdateDescriptorSets(2,writes);return true;
    }
    bool Record(DawnCommandBuffer& cmd,ResourceHandle target){
        if(!device_||pipeline_==handles::INVALID_PIPELINE||set_==handles::INVALID_DESCRIPTOR_SET||target==handles::INVALID_RESOURCE)return false;
        RenderPassDesc p{};p.colorAttachments.resize(1);auto& a=p.colorAttachments[0];a.texture=target;a.format=DataFormat::BGRA8_UNorm;a.loadOp=LoadAction::Clear;a.storeOp=StoreAction::Store;a.clearValue.color={0,0,0,1};p.viewport={{0,0},{float(width_),float(height_)},0,1};p.scissor={{0,0},{width_,height_}};cmd.BeginRenderPass(p);cmd.BindGraphicsPipeline(pipeline_);cmd.BindDescriptorSets(PipelineBindPoint::Graphics,pl_,0,1,&set_,0,nullptr);cmd.Draw(3,0,1,0);cmd.EndRenderPass();return true;
    }
    void Shutdown(){if(!device_)return;if(set_!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(set_);if(pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipeline_);if(pl_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pl_);if(layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layout_);if(sampler_!=handles::INVALID_SAMPLER)device_->DestroySampler(sampler_);for(auto h:{vs_,fs_})if(h!=handles::INVALID_SHADER)device_->DestroyShader(h);device_=nullptr;set_=handles::INVALID_DESCRIPTOR_SET;pipeline_=handles::INVALID_PIPELINE;pl_=handles::INVALID_PIPELINE_LAYOUT;layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;sampler_=handles::INVALID_SAMPLER;vs_=fs_=handles::INVALID_SHADER;}
private:
    DawnDevice* device_=nullptr;u32 width_=0,height_=0;ShaderHandle vs_=handles::INVALID_SHADER,fs_=handles::INVALID_SHADER;PipelineHandle pipeline_=handles::INVALID_PIPELINE;PipelineLayoutHandle pl_=handles::INVALID_PIPELINE_LAYOUT;DescriptorSetLayoutHandle layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;DescriptorSetHandle set_=handles::INVALID_DESCRIPTOR_SET;SamplerHandle sampler_=handles::INVALID_SAMPLER;
};
}
