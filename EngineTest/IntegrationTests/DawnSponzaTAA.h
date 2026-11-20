#pragma once
#include "DawnSponzaHalf.h"
#include "DawnSponzaPostProcess.h"
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnTexture.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"
#include "Engine/Graphics/Utils/HaltonSequence.h"
#include <filesystem>
namespace primal::tests::dawn_sponza {
// Color, depth and actual uploaded raster matrices share a slot/generation.
// Submit frames in order on the same queue; wait for completion before Save.
class TemporalAA {
public:
    struct alignas(16) Params {float screen[4]{};float reset=1;u32 depthValid=0,pad0=0,pad1=0;primal::math::m4x4 currentInverse,previousVP;};static_assert(sizeof(Params)==160);
    struct Raster {u32 frame=~0u,width=0,height=0;ResourceHandle depth=handles::INVALID_RESOURCE;primal::math::m4x4 baseVP;primal::math::v2 jitter{};bool uploaded=false;};
    TemporalAA()=default;TemporalAA(const TemporalAA&)=delete;TemporalAA& operator=(const TemporalAA&)=delete;~TemporalAA(){Shutdown();}
    static primal::math::m4x4 ClipJitter(primal::math::v2 j){auto m=graphics::rhi::math::MatrixIdentity();primal::math::set_elem(m,3,0,j.x);primal::math::set_elem(m,3,1,j.y);return m;}
    static std::string Source(u32 stage){std::ifstream f(stage?"Engine/Graphics/Dawn/shaders/TAAHistoryDepth.wgsl":"Engine/Graphics/Dawn/shaders/MeasuredTAA.wgsl");return {(std::istreambuf_iterator<char>(f)),{}};}
    bool Initialize(DawnDevice& device,u32 width,u32 height,ResourceHandle depth){
        if(device_||!width||!height||depth==handles::INVALID_RESOURCE)return false;const auto* dt=device.GetTexture(depth);if(!dt||dt->GetTextureDesc().size.x!=width||dt->GetTextureDesc().size.y!=height||dt->GetTextureDesc().format!=DataFormat::D32_Float)return false;
        device_=&device;width_=width;height_=height;depth_=depth;
        for(u32 stage=0;stage<2;++stage){DescriptorSetLayoutBinding b[7]{};const u32 count=stage?2:7;
            for(u32 i=0;i<count;++i){b[i].binding=i;b[i].descriptorCount=1;b[i].stageFlags=ShaderStage::Compute;b[i].descriptorType=stage?(i?DescriptorType::StorageImage:DescriptorType::SampledDepthImage):(i==3?DescriptorType::StorageImage:i==4?DescriptorType::UniformBuffer:i==5?DescriptorType::SampledDepthImage:DescriptorType::SampledImage);if(stage&&i==1)b[i].format=DataFormat::R32_Float;if(!stage&&i==6)b[i].unfilterableFloat=true;}
            layouts_[stage]=device.CreateDescriptorSetLayout({count,b});if(layouts_[stage]==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;PipelineLayoutDesc pl{};pl.setLayouts=&layouts_[stage];pl.setLayoutCount=1;pipelineLayouts_[stage]=device.CreatePipelineLayout(pl);if(pipelineLayouts_[stage]==handles::INVALID_PIPELINE_LAYOUT)return false;auto source=Source(stage);shaders_[stage]=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,stage?"copy_taa_depth":"measured_taa");if(shaders_[stage]==handles::INVALID_SHADER)return false;ComputePipelineDesc cp{};cp.computeShader=shaders_[stage];cp.layout=pipelineLayouts_[stage];pipelines_[stage]=device.CreateComputePipeline(cp);if(pipelines_[stage]==handles::INVALID_PIPELINE)return false;
        }
        TextureDesc t{};t.type=TextureType::Texture2D;t.size={width,height,1};t.usage=TextureUsage::ShaderResource|TextureUsage::UnorderedAccess|TextureUsage::CopySource;
        for(auto& s:slots_){t.format=DataFormat::RGBA16_Float;s.color=device.CreateTexture(t);t.format=DataFormat::R32_Float;s.depth=device.CreateTexture(t);BufferDesc cb{};cb.size=sizeof(Params);cb.type=BufferType::Constant;cb.memoryUsage=GPUMemoryUsage::Static;s.params=device.CreateBuffer(cb);for(u32 i=0;i<2;++i)s.sets[i]=device.CreateDescriptorSet({layouts_[i]});if(s.color==handles::INVALID_RESOURCE||s.depth==handles::INVALID_RESOURCE||s.params==handles::INVALID_RESOURCE||s.sets[0]==handles::INVALID_DESCRIPTOR_SET||s.sets[1]==handles::INVALID_DESCRIPTOR_SET)return false;}
        // Static scene path has no independent object motion. Matrix reprojection
        // supersedes this zero fallback whenever history is valid; reset ignores it.
        t.size={1,1,1};t.format=DataFormat::RGBA16_Float;t.usage=TextureUsage::ShaderResource|TextureUsage::CopyDest;zeroVelocity_=device.CreateTexture(t);const u16 zero[4]{};if(zeroVelocity_==handles::INVALID_RESOURCE)return false;device.UpdateTextureData(zeroVelocity_,zero,0,0,0,1,1,1,8);ready_=true;return true;
    }
    void Reset(){++generation_;}
    bool Record(DawnCommandBuffer& cmd,u32 frame,ResourceHandle current,const Raster& raster){
        if(!ready_||frame!=nextFrame_||raster.frame!=frame||!raster.uploaded||raster.depth!=depth_||raster.width!=width_||raster.height!=height_||!std::isfinite(raster.jitter.x)||!std::isfinite(raster.jitter.y))return false;
        for(u32 c=0;c<4;++c)for(u32 r=0;r<4;++r)if(!std::isfinite(primal::math::elem(raster.baseVP,c,r)))return false;
        const auto* ct=device_->GetTexture(current);if(!ct||ct->GetTextureDesc().size.x!=width_||ct->GetTextureDesc().size.y!=height_||ct->GetTextureDesc().format!=DataFormat::RGBA16_Float)return false;
        auto& s=slots_[frame%3];const auto& previous=slots_[(frame+2)%3];if(current==s.color||current==previous.color)return false;
        const bool valid=frame>0&&previous.frame==frame-1&&previous.generation==generation_&&previous.raster.depth==depth_&&previous.raster.uploaded;
        Params p{};p.screen[0]=float(width_);p.screen[1]=float(height_);p.screen[2]=1.f/width_;p.screen[3]=1.f/height_;p.reset=valid?0.f:1.f;p.depthValid=valid?1:0;
        p.currentInverse=graphics::rhi::math::Inverse(raster.baseVP)*ClipJitter({-raster.jitter.x,-raster.jitter.y});p.previousVP=valid?ClipJitter(previous.raster.jitter)*previous.raster.baseVP:raster.baseVP;
        for(u32 c=0;c<4;++c)for(u32 r=0;r<4;++r)if(!std::isfinite(primal::math::elem(p.currentInverse,c,r))||!std::isfinite(primal::math::elem(p.previousVP,c,r)))return false;
        if(!device_->UpdateBufferData(s.params,&p,sizeof(p)))return false;
        const ResourceHandle textures[]{current,valid?previous.color:current,zeroVelocity_,s.color,handles::INVALID_RESOURCE,depth_,previous.depth};DescriptorImageInfo images[7]{};DescriptorBufferInfo bi{s.params,0,sizeof(p)};WriteDescriptorSet writes[7]{};
        for(u32 i=0;i<7;++i){auto& v=writes[i];v.dstSet=s.sets[0];v.dstBinding=i;v.descriptorCount=1;v.descriptorType=i==3?DescriptorType::StorageImage:i==4?DescriptorType::UniformBuffer:i==5?DescriptorType::SampledDepthImage:DescriptorType::SampledImage;if(i==4)v.bufferInfo=&bi;else{images[i]={handles::INVALID_SAMPLER,textures[i],i==3?ResourceState::UnorderedAccess:ResourceState::ShaderResource};v.imageInfo=&images[i];}}
        device_->UpdateDescriptorSets(7,writes);Dispatch(cmd,s,0);
        DescriptorImageInfo di[]{{handles::INVALID_SAMPLER,depth_,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,s.depth,ResourceState::UnorderedAccess}};WriteDescriptorSet dw[2]{};for(u32 i=0;i<2;++i){dw[i].dstSet=s.sets[1];dw[i].dstBinding=i;dw[i].descriptorCount=1;dw[i].descriptorType=i?DescriptorType::StorageImage:DescriptorType::SampledDepthImage;dw[i].imageInfo=&di[i];}device_->UpdateDescriptorSets(2,dw);Dispatch(cmd,s,1);
        s.frame=frame;s.generation=generation_;s.raster=raster;s.cpu=p;s.inputs={current,textures[1],s.color,depth_,previous.depth};++nextFrame_;return true;
    }
    ResourceHandle Output(u32 frame)const{return ready_&&nextFrame_&&frame==nextFrame_-1?slots_[frame%3].color:handles::INVALID_RESOURCE;}
    bool Capture(DawnCommandBuffer& cmd,u32 frame){
        if(Output(frame)==handles::INVALID_RESOURCE||readback_!=handles::INVALID_RESOURCE)return false;const auto& s=slots_[frame%3];captured_=frame;capturedParams_=s.cpu;capturedRaster_=s.raster;
        u64 bytes=0;for(u32 i=0;i<5;++i){pitch_[i]=(width_*(i<3?8:4)+255)&~255u;offset_[i]=bytes;bytes+=u64(pitch_[i])*height_;}BufferDesc b{};b.memoryUsage=GPUMemoryUsage::Readback;b.size=bytes;readback_=device_->CreateBuffer(b);if(readback_==handles::INVALID_RESOURCE)return false;
        for(u32 i=0;i<5;++i){BufferTextureCopyRegion r{};r.bufferOffset=offset_[i];r.bufferRowLength=pitch_[i]/(i<3?8:4);r.bufferImageHeight=height_;r.imageExtent={width_,height_,1};cmd.CopyTextureToBuffer(s.inputs[i],readback_,&r,1);}return true;
    }
    bool Save(const std::string& prefix){
        if(readback_==handles::INVALID_RESOURCE)return false;const u64 bytes=offset_[4]+u64(pitch_[4])*height_;auto* raw=static_cast<const char*>(device_->MapBuffer(readback_,0,bytes));if(!raw)return false;
        std::ofstream f(prefix+".bin",std::ios::binary);f.write(reinterpret_cast<const char*>(&capturedParams_),sizeof(Params));u32 invalid=0;
        for(u32 i=0;i<5;++i)for(u32 y=0;y<height_;++y){const auto* row=raw+offset_[i]+u64(y)*pitch_[i];f.write(row,width_*(i<3?8:4));if(i==2){const auto* values=reinterpret_cast<const HalfStorage*>(row);for(u32 x=0;x<width_*4;++x)invalid+=!std::isfinite(float(values[x]));}}
        f.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        std::ofstream meta(prefix+".json");meta<<"{\"frame\":"<<captured_<<",\"width\":"<<width_<<",\"height\":"<<height_<<",\"reset\":"<<capturedParams_.reset<<",\"depth_valid\":"<<capturedParams_.depthValid<<",\"jitter\":["<<capturedRaster_.jitter.x<<","<<capturedRaster_.jitter.y<<"],\"invalid\":"<<invalid<<",\"layout\":\"Params160; current,history,output RGBA16F; current,previous depth float32; tight rows\",\"completion_checked_by_caller\":true}\n";meta.close();return f.good()&&meta.good()&&!invalid;
    }
    void Shutdown(){if(!device_)return;for(auto& s:slots_){for(auto h:s.sets)if(h!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(h);for(auto h:{s.color,s.depth})if(h!=handles::INVALID_RESOURCE)device_->DestroyTexture(h);if(s.params!=handles::INVALID_RESOURCE)device_->DestroyBuffer(s.params);s=Slot{};}if(zeroVelocity_!=handles::INVALID_RESOURCE)device_->DestroyTexture(zeroVelocity_);if(readback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(readback_);for(u32 i=0;i<2;++i){if(pipelines_[i]!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipelines_[i]);if(shaders_[i]!=handles::INVALID_SHADER)device_->DestroyShader(shaders_[i]);if(pipelineLayouts_[i]!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayouts_[i]);if(layouts_[i]!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layouts_[i]);}device_=nullptr;ready_=false;nextFrame_=0;generation_=0;zeroVelocity_=readback_=handles::INVALID_RESOURCE;pipelines_.fill(handles::INVALID_PIPELINE);shaders_.fill(handles::INVALID_SHADER);pipelineLayouts_.fill(handles::INVALID_PIPELINE_LAYOUT);layouts_.fill(handles::INVALID_DESCRIPTOR_SET_LAYOUT);}
private:
    struct Slot{ResourceHandle color=handles::INVALID_RESOURCE,depth=handles::INVALID_RESOURCE,params=handles::INVALID_RESOURCE;std::array<DescriptorSetHandle,2> sets{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};u32 frame=~0u,generation=~0u;Raster raster{};Params cpu{};std::array<ResourceHandle,5> inputs{};};
    void Dispatch(DawnCommandBuffer& cmd,const Slot& s,u32 stage){cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::MemoryWrite,AccessFlag::ShaderRead);cmd.BindComputePipeline(pipelines_[stage]);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayouts_[stage],0,1,&s.sets[stage],0,nullptr);cmd.Dispatch((width_+7)/8,(height_+7)/8,1);cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::AllCommands,AccessFlag::ShaderWrite,AccessFlag::MemoryRead);}
    DawnDevice* device_=nullptr;bool ready_=false;u32 width_=0,height_=0,nextFrame_=0,generation_=0,captured_=~0u;std::array<Slot,3> slots_{};ResourceHandle depth_=handles::INVALID_RESOURCE,zeroVelocity_=handles::INVALID_RESOURCE,readback_=handles::INVALID_RESOURCE;Params capturedParams_{};Raster capturedRaster_{};u32 pitch_[5]{};u64 offset_[5]{};
    std::array<DescriptorSetLayoutHandle,2> layouts_{handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT};std::array<PipelineLayoutHandle,2> pipelineLayouts_{handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT};std::array<ShaderHandle,2> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER};std::array<PipelineHandle,2> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
};
}
