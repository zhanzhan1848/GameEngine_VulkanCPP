#pragma once
#include "DawnSponzaHalf.h"
#include "DawnSponzaScreenProbeHistory.h"
namespace primal::tests::dawn_sponza {
// Full-resolution reconstruction of canonical radiance SH. Both outputs are
// owned; borrowed depth/normal and final SH must belong to the current frame.
class ScreenProbeGather {
public:
    struct DenoiseParams {float depth=8,normal=8,spatial=1.2f,pad=0;u32 radius=1,width=0,height=0,pad2=0;};static_assert(sizeof(DenoiseParams)==32);
    ScreenProbeGather()=default;ScreenProbeGather(const ScreenProbeGather&)=delete;ScreenProbeGather& operator=(const ScreenProbeGather&)=delete;~ScreenProbeGather(){Shutdown();}
    static std::string Source(u32 stage){std::ifstream f(std::string("Engine/Graphics/Dawn/shaders/Lumen/ScreenProbeMeasured")+(stage?"Denoise":"Gather")+".wgsl");return {(std::istreambuf_iterator<char>(f)),{}};}
    bool Initialize(DawnDevice& device,u32 width,u32 height,ResourceHandle depth,ResourceHandle normal){
        if(device_||!width||!height||depth==handles::INVALID_RESOURCE||normal==handles::INVALID_RESOURCE)return false;
        device_=&device;width_=width;height_=height;depth_=depth;normal_=normal;
        for(u32 stage=0;stage<2;++stage){
            const u32 count=stage?5:7;DescriptorSetLayoutBinding b[7]{};
            for(u32 i=0;i<count;++i){b[i].binding=i;b[i].descriptorCount=1;b[i].stageFlags=ShaderStage::Compute;b[i].descriptorType=stage?(i==2?DescriptorType::SampledDepthImage:i==3?DescriptorType::StorageImage:i==4?DescriptorType::UniformBuffer:DescriptorType::SampledImage):(i==0?DescriptorType::SampledDepthImage:i==1?DescriptorType::SampledImage:i==2?DescriptorType::StorageImage:i==5?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer);b[i].readonly=b[i].descriptorType==DescriptorType::StorageBuffer;}
            layouts_[stage]=device.CreateDescriptorSetLayout({count,b});if(layouts_[stage]==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
            PipelineLayoutDesc pl{};pl.setLayouts=&layouts_[stage];pl.setLayoutCount=1;pipelineLayouts_[stage]=device.CreatePipelineLayout(pl);if(pipelineLayouts_[stage]==handles::INVALID_PIPELINE_LAYOUT)return false;
            auto source=Source(stage);if(source.empty())return false;shaders_[stage]=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,stage?"measured_screen_denoise":"measured_screen_gather");if(shaders_[stage]==handles::INVALID_SHADER)return false;
            ComputePipelineDesc cp{};cp.computeShader=shaders_[stage];cp.layout=pipelineLayouts_[stage];pipelines_[stage]=device.CreateComputePipeline(cp);if(pipelines_[stage]==handles::INVALID_PIPELINE)return false;
            TextureDesc t{};t.type=TextureType::Texture2D;t.size={width,height,1};t.format=DataFormat::RGBA16_Float;t.usage=TextureUsage::ShaderResource|TextureUsage::UnorderedAccess|TextureUsage::CopySource;outputs_[stage]=device.CreateTexture(t);if(outputs_[stage]==handles::INVALID_RESOURCE)return false;
            for(auto& s:slots_){s.sets[stage]=device.CreateDescriptorSet({layouts_[stage]});BufferDesc cb{};cb.type=BufferType::Constant;cb.memoryUsage=GPUMemoryUsage::Static;cb.size=stage?sizeof(DenoiseParams):sizeof(ScreenProbePlacement::Params);s.params[stage]=device.CreateBuffer(cb);if(s.sets[stage]==handles::INVALID_DESCRIPTOR_SET||s.params[stage]==handles::INVALID_RESOURCE)return false;}
        }
        ready_=true;return true;
    }
    bool Record(DawnCommandBuffer& cmd,u32 frame,const ScreenProbePlacement::Params& params,const ScreenProbeHistory::Output& source){
        if(!ready_||frame!=nextFrame_||source.frame!=frame||params.trace[2]!=float(width_)||params.trace[3]!=float(height_)||params.tier[0]!=0||!(params.grid[2]>0)||!std::isfinite(params.grid[2]))return false;
        const u32 gridX=u32(std::ceil(float(width_)/params.grid[2])),gridY=u32(std::ceil(float(height_)/params.grid[2]));
        if(params.grid[0]!=float(gridX)||params.grid[1]!=float(gridY)||u64(gridX)*gridY!=source.probes)return false;
        const ResourceHandle buffers[]{source.positions,source.sh,source.normals};const u64 sizes[]{u64(source.probes)*16,u64(source.probes)*64,u64(source.probes)*16};
        for(u32 i=0;i<3;++i){auto* b=device_->GetBuffer(buffers[i]);if(!b||b->GetBufferDesc().size<sizes[i])return false;}
        DenoiseParams denoise{};denoise.width=width_;denoise.height=height_;auto& s=slots_[frame%3];
        if(!device_->UpdateBufferData(s.params[0],&params,sizeof(params))||!device_->UpdateBufferData(s.params[1],&denoise,sizeof(denoise)))return false;
        DescriptorBufferInfo bi[]{{source.positions,0,sizes[0]},{source.sh,0,sizes[1]},{s.params[0],0,sizeof(params)},{source.normals,0,sizes[2]},{s.params[1],0,sizeof(denoise)}};
        for(u32 stage=0;stage<2;++stage){
            DescriptorImageInfo images[4]{};WriteDescriptorSet writes[7]{};const u32 count=stage?5:7;
            for(u32 i=0;i<count;++i){auto& w=writes[i];w.dstSet=s.sets[stage];w.dstBinding=i;w.descriptorCount=1;
                if((!stage&&i>=3)||(stage&&i==4)){w.descriptorType=(!stage&&i==5)||(stage&&i==4)?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer;w.bufferInfo=&bi[stage?4:i-3];}
                else{const auto tex=stage?(i==0?outputs_[0]:i==1?normal_:i==2?depth_:outputs_[1]):(i==0?depth_:i==1?normal_:outputs_[0]);const bool out=i==(stage?3u:2u);images[i]={handles::INVALID_SAMPLER,tex,out?ResourceState::UnorderedAccess:ResourceState::ShaderResource};w.descriptorType=out?DescriptorType::StorageImage:i==(stage?2u:0u)?DescriptorType::SampledDepthImage:DescriptorType::SampledImage;w.imageInfo=&images[i];}
            }
            device_->UpdateDescriptorSets(count,writes);cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::MemoryWrite,AccessFlag::ShaderRead);cmd.BindComputePipeline(pipelines_[stage]);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayouts_[stage],0,1,&s.sets[stage],0,nullptr);cmd.Dispatch((width_+7)/8,(height_+7)/8,1);
        }
        cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::AllCommands,AccessFlag::ShaderWrite,AccessFlag::MemoryRead);lastParams_=params;lastDenoise_=denoise;lastSource_=source;++nextFrame_;return true;
    }
    ResourceHandle Filtered()const{return ready_?outputs_[1]:handles::INVALID_RESOURCE;}
    bool Capture(DawnCommandBuffer& cmd){
        if(!ready_||!nextFrame_||readback_!=handles::INVALID_RESOURCE)return false;pitch_=(width_*8+255)&~255u;BufferDesc b{};b.memoryUsage=GPUMemoryUsage::Readback;b.size=u64(pitch_)*height_*2;readback_=device_->CreateBuffer(b);if(readback_==handles::INVALID_RESOURCE)return false;
        for(u32 i=0;i<2;++i){BufferTextureCopyRegion r{};r.bufferOffset=u64(pitch_)*height_*i;r.bufferRowLength=pitch_/8;r.bufferImageHeight=height_;r.imageExtent={width_,height_,1};cmd.CopyTextureToBuffer(outputs_[i],readback_,&r,1);}return true;
    }
    bool Save(const std::string& prefix){
        if(readback_==handles::INVALID_RESOURCE)return false;const auto* raw=static_cast<const char*>(device_->MapBuffer(readback_,0,u64(pitch_)*height_*2));if(!raw)return false;
        std::ofstream out(prefix+".bin",std::ios::binary);u32 invalid=0,valid[2]{};float peak=0;
        for(u32 image=0;image<2;++image)for(u32 y=0;y<height_;++y){const auto* row=raw+u64(pitch_)*(height_*image+y);out.write(row,width_*8);const auto* values=reinterpret_cast<const HalfStorage*>(row);for(u32 x=0;x<width_;++x){valid[image]+=float(values[x*4+3])>0;for(u32 c=0;c<4;++c){const float v=values[x*4+c];invalid+=!std::isfinite(v);peak=std::max(peak,std::abs(v));}}}
        out.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        std::ofstream params(prefix+"-params.bin",std::ios::binary);params.write(reinterpret_cast<const char*>(&lastParams_),sizeof(lastParams_));params.write(reinterpret_cast<const char*>(&lastDenoise_),sizeof(lastDenoise_));params.close();
        std::ofstream meta(prefix+".json");meta<<"{\"frame\":"<<lastSource_.frame<<",\"width\":"<<width_<<",\"height\":"<<height_<<",\"probes\":"<<lastSource_.probes<<",\"valid_gather\":"<<valid[0]<<",\"valid_denoise\":"<<valid[1]<<",\"invalid\":"<<invalid<<",\"peak\":"<<peak<<",\"layout\":\"gather RGBA16F; denoise RGBA16F; tight rows\",\"completion_checked_by_caller\":true}\n";meta.close();return out.good()&&params.good()&&meta.good()&&!invalid;
    }
    void Shutdown(){if(!device_)return;for(auto& s:slots_){for(auto h:s.sets)if(h!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(h);for(auto h:s.params)if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);s=Slot{};}for(u32 i=0;i<2;++i){if(outputs_[i]!=handles::INVALID_RESOURCE)device_->DestroyTexture(outputs_[i]);if(pipelines_[i]!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipelines_[i]);if(shaders_[i]!=handles::INVALID_SHADER)device_->DestroyShader(shaders_[i]);if(pipelineLayouts_[i]!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayouts_[i]);if(layouts_[i]!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layouts_[i]);}if(readback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(readback_);device_=nullptr;ready_=false;nextFrame_=0;readback_=handles::INVALID_RESOURCE;outputs_.fill(handles::INVALID_RESOURCE);pipelines_.fill(handles::INVALID_PIPELINE);shaders_.fill(handles::INVALID_SHADER);pipelineLayouts_.fill(handles::INVALID_PIPELINE_LAYOUT);layouts_.fill(handles::INVALID_DESCRIPTOR_SET_LAYOUT);}
private:
    struct Slot{std::array<ResourceHandle,2> params{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};std::array<DescriptorSetHandle,2> sets{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};};
    DawnDevice* device_=nullptr;bool ready_=false;u32 width_=0,height_=0,nextFrame_=0,pitch_=0;ScreenProbePlacement::Params lastParams_{};DenoiseParams lastDenoise_{};ScreenProbeHistory::Output lastSource_{};std::array<Slot,3> slots_{};
    ResourceHandle depth_=handles::INVALID_RESOURCE,normal_=handles::INVALID_RESOURCE,readback_=handles::INVALID_RESOURCE;std::array<ResourceHandle,2> outputs_{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};
    std::array<DescriptorSetLayoutHandle,2> layouts_{handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT};std::array<PipelineLayoutHandle,2> pipelineLayouts_{handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT};std::array<ShaderHandle,2> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER};std::array<PipelineHandle,2> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
};
}
