#pragma once
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "DawnSponzaShadowFit.h"
#include <array>
#include <fstream>
#include <iterator>
#include <vector>
#include <cmath>

namespace primal::tests::dawn_sponza {

class Shadows {
public:
    Shadows()=default;
    Shadows(const Shadows&)=delete; Shadows& operator=(const Shadows&)=delete;
    ~Shadows(){Shutdown();}
    bool Initialize(DawnDevice& device,u32 width,u32 height,u32 mapSize=2048){
        if(device_||!width||!height||!mapSize)return false;
        device_=&device;width_=(width+1)/2;height_=(height+1)/2;mapSize_=mapSize;
        auto load=[&](const char* path,ShaderStage stage,const char* entry){
            std::ifstream input(path,std::ios::binary);std::string code((std::istreambuf_iterator<char>(input)),{});
            return code.empty()?handles::INVALID_SHADER:device.CreateShader(code.data(),code.size(),stage,entry);
        };
        shaders_[0]=load("EngineTest/shaders/DawnWGSL/SponzaShadowDepth.wgsl",ShaderStage::Vertex,"shadow_depth");
        shaders_[1]=load("Engine/Graphics/Dawn/shaders/SponzaShadowFilter.wgsl",ShaderStage::Compute,"shadow_filter");
        for(auto h:shaders_)if(h==handles::INVALID_SHADER)return false;
        DescriptorSetLayoutBinding db{0,DescriptorType::UniformBuffer,1,ShaderStage::Vertex,nullptr};
        layouts_[0]=device.CreateDescriptorSetLayout({1,&db});
        DescriptorSetLayoutBinding bindings[9]{};
        for(u32 i=0;i<9;++i){
            const auto type=i==0?DescriptorType::UniformBuffer:i==3?DescriptorType::StorageImage:i==4?DescriptorType::SampledImage:
                i==6?DescriptorType::Sampler:DescriptorType::SampledDepthImage;
            bindings[i]={i,type,1,ShaderStage::Compute,nullptr};
        }
        bindings[3].format=DataFormat::RGBA8_UNorm;
        layouts_[1]=device.CreateDescriptorSetLayout({9,bindings});
        for(u32 i=0;i<2;++i){
            if(layouts_[i]==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
            PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&layouts_[i];
            pipelineLayouts_[i]=device.CreatePipelineLayout(pl);
            if(pipelineLayouts_[i]==handles::INVALID_PIPELINE_LAYOUT)return false;
        }
        GraphicsPipelineDesc gp{};gp.vertexShader=shaders_[0];gp.layout=pipelineLayouts_[0];gp.renderTargetCount=0;
        gp.depthStencilFormat=DataFormat::D32_Float;gp.enableDepthTest=gp.enableDepthWrite=true;
        gp.depthFunc=ComparisonFunc::Less;gp.cullMode=CullMode::None;
        gp.vertexAttributes.push_back({0,0,DataFormat::RGB32_Float,0});gp.vertexBindings.push_back({0,32,true});
        pipelines_[0]=device.CreateGraphicsPipeline(gp);
        ComputePipelineDesc cp{};cp.computeShader=shaders_[1];cp.layout=pipelineLayouts_[1];cp.threadGroupSize={8,8,1};
        pipelines_[1]=device.CreateComputePipeline(cp);
        for(auto h:pipelines_)if(h==handles::INVALID_PIPELINE)return false;
        SamplerDesc sd{};sd.minFilter=sd.magFilter=FilterMode::Linear;
        sd.addressU=sd.addressV=sd.addressW=TextureAddressMode::Clamp;sd.comparisonFunc=ComparisonFunc::Never;
        sampler_=device.CreateSampler(sd);if(sampler_==handles::INVALID_SAMPLER)return false;
        TextureDesc td{};td.type=TextureType::Texture2D;td.size={mapSize,mapSize,1};td.format=DataFormat::D32_Float;
        td.usage=TextureUsage::DepthStencil|TextureUsage::ShaderResource|TextureUsage::CopySource;
        for(auto& h:maps_){h=device.CreateTexture(td);if(h==handles::INVALID_RESOURCE)return false;}
        td.size={width_,height_,1};td.format=DataFormat::RGBA8_UNorm;
        td.usage=TextureUsage::UnorderedAccess|TextureUsage::ShaderResource|TextureUsage::CopySource;
        visibility_=device.CreateTexture(td);if(visibility_==handles::INVALID_RESOURCE)return false;
        for(u32 f=0;f<3;++f)for(u32 p=0;p<5;++p){
            BufferDesc bd{};bd.size=p==4?sizeof(ShadowParameters):sizeof(smath::m4x4);bd.type=BufferType::Constant;bd.memoryUsage=GPUMemoryUsage::Dynamic;
            buffers_[f][p]=device.CreateBuffer(bd);
            DescriptorSetDesc ds{};ds.layout=layouts_[p==4?1:0];sets_[f][p]=device.CreateDescriptorSet(ds);
            if(buffers_[f][p]==handles::INVALID_RESOURCE||sets_[f][p]==handles::INVALID_DESCRIPTOR_SET)return false;
        }
        return true;
    }
    template<class Draw>
    bool RecordMaps(DawnCommandBuffer& cmd,const ShadowParameters& p,u32 frame,Draw draw){
        const u32 slot=frame%3;
        for(u32 c=0;c<4;++c){
            if(!device_->UpdateBufferData(buffers_[slot][c],&p.lightVP[c],64))return false;
            DescriptorBufferInfo bi{buffers_[slot][c],0,64};WriteDescriptorSet w{};
            w.dstSet=sets_[slot][c];w.dstBinding=0;w.descriptorCount=1;w.descriptorType=DescriptorType::UniformBuffer;w.bufferInfo=&bi;
            device_->UpdateDescriptorSets(1,&w);
            RenderPassDesc pass{};pass.depthAttachment.texture=maps_[c];pass.depthAttachment.format=DataFormat::D32_Float;
            pass.depthAttachment.loadOp=LoadAction::Clear;pass.depthAttachment.storeOp=StoreAction::Store;
            pass.depthAttachment.clearValue.depth=1;pass.viewport={{0,0},{float(mapSize_),float(mapSize_)},0,1};pass.scissor={{0,0},{mapSize_,mapSize_}};
            cmd.BeginRenderPass(pass);cmd.BindGraphicsPipeline(pipelines_[0]);
            cmd.BindDescriptorSets(PipelineBindPoint::Graphics,pipelineLayouts_[0],0,1,&sets_[slot][c],0,nullptr);
            draw(cmd);cmd.EndRenderPass();
        }
        return true;
    }
    bool RecordFilter(DawnCommandBuffer& cmd,ResourceHandle depth,ResourceHandle normal,const ShadowParameters& p,u32 frame){
        const u32 slot=frame%3;
        if(!device_->UpdateBufferData(buffers_[slot][4],&p,sizeof(p)))return false;
        DescriptorBufferInfo bi{buffers_[slot][4],0,sizeof(p)};
        ResourceHandle textures[9]{handles::INVALID_RESOURCE,maps_[0],maps_[1],visibility_,normal,depth,normal,maps_[2],maps_[3]};
        DescriptorImageInfo images[9]{};WriteDescriptorSet writes[9]{};
        for(u32 i=0;i<9;++i){
            images[i]={sampler_,textures[i],i==3?ResourceState::UnorderedAccess:ResourceState::ShaderResource};
            auto& w=writes[i];w.dstSet=sets_[slot][4];w.dstBinding=i;w.descriptorCount=1;
            w.descriptorType=i==0?DescriptorType::UniformBuffer:i==3?DescriptorType::StorageImage:i==4?DescriptorType::SampledImage:
                i==6?DescriptorType::Sampler:DescriptorType::SampledDepthImage;
            if(i==0)w.bufferInfo=&bi;else w.imageInfo=&images[i];
        }
        device_->UpdateDescriptorSets(9,writes);cmd.BindComputePipeline(pipelines_[1]);
        cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayouts_[1],0,1,&sets_[slot][4],0,nullptr);
        cmd.Dispatch((width_+7)/8,(height_+7)/8,1);return true;
    }
    ResourceHandle Visibility()const{return visibility_;}
    ResourceHandle Map(u32 c)const{return maps_[c];}
    SamplerHandle Sampler()const{return sampler_;}
    void Shutdown(){
        if(!device_)return;
        for(auto& row:sets_)for(auto& h:row){if(h!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(h);h=handles::INVALID_DESCRIPTOR_SET;}
        for(auto& row:buffers_)for(auto& h:row){if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);h=handles::INVALID_RESOURCE;}
        for(auto& h:maps_){if(h!=handles::INVALID_RESOURCE)device_->DestroyTexture(h);h=handles::INVALID_RESOURCE;}
        if(visibility_!=handles::INVALID_RESOURCE)device_->DestroyTexture(visibility_);visibility_=handles::INVALID_RESOURCE;
        if(sampler_!=handles::INVALID_SAMPLER)device_->DestroySampler(sampler_);sampler_=handles::INVALID_SAMPLER;
        for(auto& h:pipelines_){if(h!=handles::INVALID_PIPELINE)device_->DestroyPipeline(h);h=handles::INVALID_PIPELINE;}
        for(auto& h:pipelineLayouts_){if(h!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(h);h=handles::INVALID_PIPELINE_LAYOUT;}
        for(auto& h:layouts_){if(h!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(h);h=handles::INVALID_DESCRIPTOR_SET_LAYOUT;}
        for(auto& h:shaders_){if(h!=handles::INVALID_SHADER)device_->DestroyShader(h);h=handles::INVALID_SHADER;}
        device_=nullptr;
    }
private:
    DawnDevice* device_=nullptr;u32 width_=0,height_=0,mapSize_=0;
    std::array<ShaderHandle,2> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER};
    std::array<PipelineHandle,2> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
    std::array<PipelineLayoutHandle,2> pipelineLayouts_{handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT};
    std::array<DescriptorSetLayoutHandle,2> layouts_{handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    std::array<ResourceHandle,4> maps_{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};
    ResourceHandle visibility_=handles::INVALID_RESOURCE;SamplerHandle sampler_=handles::INVALID_SAMPLER;
    std::array<std::array<ResourceHandle,5>,3> buffers_=[] {std::array<std::array<ResourceHandle,5>,3> a;for(auto& row:a)row.fill(handles::INVALID_RESOURCE);return a;}();
    std::array<std::array<DescriptorSetHandle,5>,3> sets_=[] {std::array<std::array<DescriptorSetHandle,5>,3> a;for(auto& row:a)row.fill(handles::INVALID_DESCRIPTOR_SET);return a;}();
};
}
