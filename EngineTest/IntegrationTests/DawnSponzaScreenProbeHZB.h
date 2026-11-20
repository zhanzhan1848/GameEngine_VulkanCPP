#pragma once
#include "DawnSponzaScreenProbes.h"
namespace primal::tests::dawn_sponza {
// Native R32F min pyramid and frozen-profile near-field trace. Owns only its
// outputs; placement/rays and the depth image remain borrowed.
class ScreenProbeHZB {
public:
    struct Output {ResourceHandle hits=handles::INVALID_RESOURCE;u32 frame=~0u,probes=0,rays=0,mips=0;};
    Output GetOutput(u32 frame)const {auto h=Hits(frame);return h==handles::INVALID_RESOURCE?Output{}:Output{h,frame,count_,rays_,levels_};}
    ScreenProbeHZB()=default;ScreenProbeHZB(const ScreenProbeHZB&)=delete;ScreenProbeHZB& operator=(const ScreenProbeHZB&)=delete;
    ~ScreenProbeHZB(){Shutdown();}
    bool Initialize(DawnDevice& device,u32 width,u32 height,ResourceHandle depth,u32 probes,u32 rays){
        if(device_||!width||!height||!probes||rays<8||rays>64||depth==handles::INVALID_RESOURCE)return false;
        WGPULimits limits=WGPU_LIMITS_INIT;if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success||width>limits.maxTextureDimension2D||height>limits.maxTextureDimension2D||u64(probes)*rays*32>limits.maxStorageBufferBindingSize)return false;
        device_=&device;width_=width;height_=height;count_=probes;rays_=rays;
        // Same stop rule as frozen HZBSystem(min_mip_size=8); small fixtures
        // retain two legal levels because the frozen marcher starts at mip1.
        for(u32 size=std::max(width,height);size>8;size/=2)++levels_;
        levels_=std::max(levels_,2u);if(std::max(width,height)<2)return false;
        TextureDesc td{};td.type=TextureType::Texture2D;td.size={width,height,1};td.format=DataFormat::R32_Float;td.mipLevels=levels_;td.usage=TextureUsage::ShaderResource|TextureUsage::UnorderedAccess|TextureUsage::CopySource;
        texture_=device.CreateTexture(td);if(texture_==handles::INVALID_RESOURCE)return false;
        for(u32 mip=0;mip<levels_;++mip){TextureViewDesc v{};v.texture=texture_;v.viewType=TextureType::Texture2D;v.format=DataFormat::R32_Float;v.mostDetailedMip=mip;v.mipCount=1;v.arraySize=1;views_.push_back(device.CreateTextureView(v));if(views_.back()==handles::INVALID_RESOURCE)return false;}
        auto read=[](const char* path){std::ifstream f(path);return std::string((std::istreambuf_iterator<char>(f)),{});};
        const char* paths[]={"Engine/Graphics/Dawn/shaders/Nanite/HZBCopy.wgsl","Engine/Graphics/Dawn/shaders/Nanite/HZBMip.wgsl","Engine/Graphics/Dawn/shaders/Lumen/ScreenProbeMeasuredHZB.wgsl"};
        const char* entries[]={"copy_depth_to_hzb_mip0","generate_hzb_mip_level","measured_hzb_trace"};
        for(u32 stage=0;stage<3;++stage){std::vector<DescriptorSetLayoutBinding> bindings(stage==2?6:2);
            for(u32 i=0;i<bindings.size();++i){auto& b=bindings[i];b.binding=i;b.descriptorCount=1;b.stageFlags=ShaderStage::Compute;
                b.descriptorType=stage==2?(i==0?DescriptorType::UniformBuffer:i==1?DescriptorType::SampledImage:DescriptorType::StorageBuffer):(i==1?DescriptorType::StorageImage:stage==0?DescriptorType::SampledDepthImage:DescriptorType::SampledImage);b.readonly=stage==2&&i>=2&&i<5;b.format=DataFormat::R32_Float;b.unfilterableFloat=b.descriptorType==DescriptorType::SampledImage;}
            layouts_[stage]=device.CreateDescriptorSetLayout({u32(bindings.size()),bindings.data()});if(layouts_[stage]==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
            PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&layouts_[stage];pipelineLayouts_[stage]=device.CreatePipelineLayout(pl);if(pipelineLayouts_[stage]==handles::INVALID_PIPELINE_LAYOUT)return false;
            std::string source=read(paths[stage]);if(source.empty())return false;if(stage==2){auto common=read("Engine/Graphics/Dawn/shaders/Lumen/ScreenProbeMeasuredCommon.wgsl");if(common.empty())return false;source=common+source;}
            shaders_[stage]=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,entries[stage]);if(shaders_[stage]==handles::INVALID_SHADER)return false;
            ComputePipelineDesc cp{};cp.layout=pipelineLayouts_[stage];cp.computeShader=shaders_[stage];pipelines_[stage]=device.CreateComputePipeline(cp);if(pipelines_[stage]==handles::INVALID_PIPELINE)return false;
        }
        for(u32 mip=0;mip<levels_;++mip){auto set=device.CreateDescriptorSet({layouts_[mip?1:0]});if(set==handles::INVALID_DESCRIPTOR_SET)return false;buildSets_.push_back(set);
            DescriptorImageInfo ii[]{{handles::INVALID_SAMPLER,mip?views_[mip-1]:depth,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,views_[mip],ResourceState::UnorderedAccess}};WriteDescriptorSet w[2]{};
            for(u32 i=0;i<2;++i){w[i].dstSet=set;w[i].dstBinding=i;w[i].descriptorCount=1;w[i].descriptorType=i?DescriptorType::StorageImage:mip?DescriptorType::SampledImage:DescriptorType::SampledDepthImage;w[i].imageInfo=&ii[i];}device.UpdateDescriptorSets(2,w);
        }
        for(auto& s:slots_){BufferDesc b{};b.type=BufferType::Constant;b.memoryUsage=GPUMemoryUsage::Static;b.size=sizeof(ScreenProbePlacement::Params);s.params=device.CreateBuffer(b);b.type=BufferType::Raw;b.size=u64(count_)*rays_*16;s.hits=device.CreateBuffer(b);s.set=device.CreateDescriptorSet({layouts_[2]});if(s.params==handles::INVALID_RESOURCE||s.hits==handles::INVALID_RESOURCE||s.set==handles::INVALID_DESCRIPTOR_SET)return false;}
        ready_=true;return true;
    }
    bool Build(DawnCommandBuffer& cmd,u32 frame){
        if(!ready_||frame!=nextFrame_)return false;
        for(u32 mip=0;mip<levels_;++mip){u32 stage=mip?1:0;cmd.BindComputePipeline(pipelines_[stage]);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayouts_[stage],0,1,&buildSets_[mip],0,nullptr);cmd.Dispatch((std::max(1u,width_>>mip)+15)/16,(std::max(1u,height_>>mip)+15)/16,1);cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead);}
        ++nextFrame_;return true;
    }
    bool Trace(DawnCommandBuffer& cmd,u32 frame,const ScreenProbePlacement::Output& input,float nearPlane=.1f,float farPlane=1000.f,float distance=25.f){
        if(!ready_||!nextFrame_||frame!=nextFrame_-1||input.frame!=frame||input.probes!=count_||input.raysPerProbe!=rays_||slots_[frame%3].frame==frame)return false;
        if(!(nearPlane>0&&farPlane>nearPlane)||!std::isfinite(farPlane)||!std::isfinite(distance)||distance<0)return false;
        for(auto h:{input.positions,input.normals,input.rays})if(h==handles::INVALID_RESOURCE)return false;
        auto& s=slots_[frame%3];s.cpu=input.params;s.cpu.hzb[0]=nearPlane;s.cpu.hzb[1]=farPlane;s.cpu.hzb[2]=float(levels_);s.cpu.hzb[3]=distance;
        if(!device_->UpdateBufferData(s.params,&s.cpu,sizeof(s.cpu)))return false;
        DescriptorBufferInfo bi[]{{s.params,0,sizeof(s.cpu)},{input.positions,0,u64(count_)*16},{input.normals,0,u64(count_)*16},{input.rays,0,u64(count_)*rays_*32},{s.hits,0,u64(count_)*rays_*16}};
        DescriptorImageInfo image{handles::INVALID_SAMPLER,texture_,ResourceState::ShaderResource};WriteDescriptorSet writes[6]{};
        for(u32 i=0;i<6;++i){auto& w=writes[i];w.dstSet=s.set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=i==0?DescriptorType::UniformBuffer:i==1?DescriptorType::SampledImage:DescriptorType::StorageBuffer;if(i==1)w.imageInfo=&image;else w.bufferInfo=&bi[i==0?0:i-1];}
        device_->UpdateDescriptorSets(6,writes);cmd.BindComputePipeline(pipelines_[2]);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayouts_[2],0,1,&s.set,0,nullptr);cmd.Dispatch((count_*rays_+63)/64,1,1);cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::AllCommands,AccessFlag::ShaderWrite,AccessFlag::MemoryRead);s.frame=frame;return true;
    }
    bool Capture(DawnCommandBuffer& cmd,u32 frame){
        if(!ready_||!nextFrame_||frame!=nextFrame_-1||slots_[frame%3].frame!=frame||readback_!=handles::INVALID_RESOURCE)return false;
        captured_=frame;capturedParams_=slots_[frame%3].cpu;BufferDesc b{};b.memoryUsage=GPUMemoryUsage::Readback;b.size=u64(count_)*rays_*16;readback_=device_->CreateBuffer(b);if(readback_==handles::INVALID_RESOURCE)return false;cmd.CopyBuffer(slots_[frame%3].hits,readback_,0,0,b.size);
        for(u32 mip=0;mip<levels_;++mip){const u32 w=std::max(1u,width_>>mip),h=std::max(1u,height_>>mip),pitch=(w*4+255)&~255u;b.size=u64(pitch)*h;auto rb=device_->CreateBuffer(b);if(rb==handles::INVALID_RESOURCE)return false;mipReadbacks_.push_back(rb);BufferTextureCopyRegion region{};region.bufferRowLength=pitch/4;region.bufferImageHeight=h;region.imageSubresource.mipLevel=mip;region.imageExtent={w,h,1};cmd.CopyTextureToBuffer(texture_,rb,&region,1);}return true;
    }
    bool Save(const std::string& prefix){
        if(readback_==handles::INVALID_RESOURCE||mipReadbacks_.size()!=levels_)return false;const u64 bytes=u64(count_)*rays_*16;auto raw=static_cast<const float*>(device_->MapBuffer(readback_,0,bytes));if(!raw)return false;u32 hits=0,invalid=0;for(u64 i=0;i<bytes/4;++i)invalid+=!std::isfinite(raw[i]);for(u64 i=0;i<u64(count_)*rays_;++i)hits+=raw[i*4+3]==1;
        std::ofstream out(prefix+".bin",std::ios::binary);out.write(reinterpret_cast<const char*>(&capturedParams_),sizeof(capturedParams_));out.write(reinterpret_cast<const char*>(raw),bytes);device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        for(u32 mip=0;mip<levels_;++mip){const u32 w=std::max(1u,width_>>mip),h=std::max(1u,height_>>mip),pitch=(w*4+255)&~255u;auto p=static_cast<const char*>(device_->MapBuffer(mipReadbacks_[mip],0,u64(pitch)*h));if(!p)return false;for(u32 y=0;y<h;++y)out.write(p+u64(y)*pitch,w*4);device_->UnmapBuffer(mipReadbacks_[mip]);device_->DestroyBuffer(mipReadbacks_[mip]);}mipReadbacks_.clear();out.close();
        std::ofstream meta(prefix+".json");meta<<"{\"frame\":"<<captured_<<",\"width\":"<<width_<<",\"height\":"<<height_<<",\"probes\":"<<count_<<",\"rays\":"<<rays_<<",\"mips\":"<<levels_<<",\"hits\":"<<hits<<",\"invalid\":"<<invalid<<",\"layout\":\"Params544; hits vec4; tight R32F mip chain\"}\n";meta.close();return out.good()&&meta.good()&&!invalid;
    }
    ResourceHandle Hits(u32 frame)const{return ready_&&nextFrame_&&frame==nextFrame_-1&&slots_[frame%3].frame==frame?slots_[frame%3].hits:handles::INVALID_RESOURCE;}
    void Shutdown(){if(!device_)return;for(auto& s:slots_){if(s.set!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(s.set);for(auto h:{s.params,s.hits})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);s=Slot{};}for(auto h:buildSets_)device_->DestroyDescriptorSet(h);buildSets_.clear();for(auto h:views_)device_->DestroyTexture(h);views_.clear();if(texture_!=handles::INVALID_RESOURCE)device_->DestroyTexture(texture_);texture_=handles::INVALID_RESOURCE;if(readback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;for(auto h:mipReadbacks_)device_->DestroyBuffer(h);mipReadbacks_.clear();for(u32 i=0;i<3;++i){if(pipelines_[i]!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipelines_[i]);if(shaders_[i]!=handles::INVALID_SHADER)device_->DestroyShader(shaders_[i]);if(pipelineLayouts_[i]!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayouts_[i]);if(layouts_[i]!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layouts_[i]);}pipelines_.fill(handles::INVALID_PIPELINE);shaders_.fill(handles::INVALID_SHADER);pipelineLayouts_.fill(handles::INVALID_PIPELINE_LAYOUT);layouts_.fill(handles::INVALID_DESCRIPTOR_SET_LAYOUT);device_=nullptr;ready_=false;levels_=nextFrame_=0;}
private:
    struct Slot{ResourceHandle params=handles::INVALID_RESOURCE,hits=handles::INVALID_RESOURCE;DescriptorSetHandle set=handles::INVALID_DESCRIPTOR_SET;u32 frame=~0u;ScreenProbePlacement::Params cpu{};};
    DawnDevice* device_=nullptr;bool ready_=false;u32 width_=0,height_=0,levels_=0,count_=0,rays_=0,nextFrame_=0,captured_=~0u;
    ResourceHandle texture_=handles::INVALID_RESOURCE,readback_=handles::INVALID_RESOURCE;ScreenProbePlacement::Params capturedParams_{};
    std::vector<ResourceHandle> views_,mipReadbacks_;std::vector<DescriptorSetHandle> buildSets_;std::array<Slot,3> slots_{};
    std::array<DescriptorSetLayoutHandle,3> layouts_{handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT};std::array<PipelineLayoutHandle,3> pipelineLayouts_{handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT};std::array<ShaderHandle,3> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER,handles::INVALID_SHADER};std::array<PipelineHandle,3> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
};
}
