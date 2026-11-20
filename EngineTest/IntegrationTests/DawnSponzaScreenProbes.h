#pragma once
#include "DawnSponzaGBufferFormats.h"
#include "DawnSponzaGather.h"
namespace primal::tests::dawn_sponza {
class ScreenProbePlacement {
public:
    struct Params {
        primal::math::m4x4 viewProjection,inverseVP;
        float camera[4]{},grid[4]{},trace[4]{};
        float sdfOrigins[3][4]{},sdfVoxels[3][4]{},sdfExtents[3][4]{},sdfResolutions[4]{};
        float surfaceCache[4]{},ddgi[2][4]{},hzb[4]{},cache[4]{},diagnostics[4]{},hint[4]{},sky[4]{},wrc[2][4]{};
        float representative[4]{1,1,.5f,1},tier[4]{0,.85f,0,0},tierCounts[4]{};
    };static_assert(sizeof(Params)==544);static_assert(offsetof(Params,representative)==496);
    struct History {ResourceHandle sh=handles::INVALID_RESOURCE;u32 frame=~0u,probes=0;};
    struct Output {
        ResourceHandle positions=handles::INVALID_RESOURCE,normals=handles::INVALID_RESOURCE,rays=handles::INVALID_RESOURCE;
        u32 frame=~0u,probes=0,raysPerProbe=0;Params params{};History importanceSource{};
    };
    Output GetRays(u32 frame)const {
        if(!ready_||!nextFrame_||frame!=nextFrame_-1)return {};
        const auto& s=slots_[frame%3];if(s.rayFrame!=frame)return {};
        return {s.positions,s.normals,s.rays,frame,count_,rays_,s.cpu,s.importanceSource};
    }
    ScreenProbePlacement()=default;ScreenProbePlacement(const ScreenProbePlacement&)=delete;ScreenProbePlacement& operator=(const ScreenProbePlacement&)=delete;~ScreenProbePlacement(){Shutdown();}
    bool Initialize(DawnDevice& device,u32 width,u32 height,u32 spacing,u32 rays,ResourceHandle depth,ResourceHandle normal){
        if(device_||!width||!height||!spacing||spacing>64||rays<8||rays>64||depth==handles::INVALID_RESOURCE||normal==handles::INVALID_RESOURCE)return false;
        width_=width;height_=height;spacing_=spacing;rays_=rays;gridX_=(width+spacing-1)/spacing;gridY_=(height+spacing-1)/spacing;count_=gridX_*gridY_;
        WGPULimits limits=WGPU_LIMITS_INIT;if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success||u64(count_)*rays*32>limits.maxStorageBufferBindingSize||(u64(count_)*rays+63)/64>limits.maxComputeWorkgroupsPerDimension)return false;
        device_=&device;depth_=depth;normal_=normal;
        auto buffer=[&](u64 size,const void* data,BufferType type){BufferDesc b{};b.type=type;b.size=size;b.memoryUsage=GPUMemoryUsage::Static;auto h=device.CreateBuffer(b);if(h!=handles::INVALID_RESOURCE&&data&&!device.UpdateBufferData(h,data,size)){device.DestroyBuffer(h);return handles::INVALID_RESOURCE;}return h;};
        std::vector<u8> zero(u64(count_)*64);zeroHistory_=buffer(zero.size(),zero.data(),BufferType::Raw);if(zeroHistory_==handles::INVALID_RESOURCE)return false;
        auto read=[](const char* path){std::ifstream f(path);return std::string((std::istreambuf_iterator<char>(f)),{});};auto common=read("Engine/Graphics/Dawn/shaders/Lumen/ScreenProbeMeasuredCommon.wgsl");if(common.empty())return false;
        for(u32 stage=0;stage<2;++stage){DescriptorSetLayoutBinding bindings[5]{};for(u32 i=0;i<5;++i){auto& b=bindings[i];b.binding=i;b.descriptorCount=1;b.stageFlags=ShaderStage::Compute;b.descriptorType=i==0?DescriptorType::UniformBuffer:stage==0&&i==1?DescriptorType::SampledDepthImage:stage==0&&i==2?DescriptorType::SampledImage:DescriptorType::StorageBuffer;b.readonly=stage==1&&i>0&&i<4;}
            layouts_[stage]=device.CreateDescriptorSetLayout({5,bindings});if(layouts_[stage]==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&layouts_[stage];pipelineLayouts_[stage]=device.CreatePipelineLayout(pl);if(pipelineLayouts_[stage]==handles::INVALID_PIPELINE_LAYOUT)return false;
            auto kernel=read(stage?"Engine/Graphics/Dawn/shaders/Lumen/ScreenProbeMeasuredRays.wgsl":"Engine/Graphics/Dawn/shaders/Lumen/ScreenProbeMeasuredPlace.wgsl");if(kernel.empty())return false;auto source=common+kernel;shaders_[stage]=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,stage?"measured_screen_rays":"measured_place");if(shaders_[stage]==handles::INVALID_SHADER)return false;ComputePipelineDesc cp{};cp.computeShader=shaders_[stage];cp.layout=pipelineLayouts_[stage];pipelines_[stage]=device.CreateComputePipeline(cp);if(pipelines_[stage]==handles::INVALID_PIPELINE)return false;
        }
        for(auto& s:slots_){s.params=buffer(sizeof(Params),nullptr,BufferType::Constant);s.positions=buffer(u64(count_)*16,nullptr,BufferType::Raw);s.normals=buffer(u64(count_)*16,nullptr,BufferType::Raw);s.rays=buffer(u64(count_)*rays_*32,nullptr,BufferType::Raw);for(auto h:{s.params,s.positions,s.normals,s.rays})if(h==handles::INVALID_RESOURCE)return false;for(u32 i=0;i<2;++i){s.sets[i]=device.CreateDescriptorSet({layouts_[i]});if(s.sets[i]==handles::INVALID_DESCRIPTOR_SET)return false;}}
        ready_=true;return true;
    }
    bool RecordPlace(DawnCommandBuffer& cmd,u32 frame,Params params){
        if(!ready_||frame!=nextFrame_||params.tier[0]!=0||params.tier[2]!=0||params.tier[3]!=0||!std::isfinite(params.camera[3])||params.camera[3]>=16777216.f)return false;
        params.grid[0]=float(gridX_);params.grid[1]=float(gridY_);params.grid[2]=float(spacing_);params.grid[3]=float(rays_);params.trace[2]=float(width_);params.trace[3]=float(height_);
        auto& s=slots_[frame%3];if(!device_->UpdateBufferData(s.params,&params,sizeof(params)))return false;
        DescriptorBufferInfo bi[]{{s.params,0,sizeof(params)},{s.positions,0,u64(count_)*16},{s.normals,0,u64(count_)*16}};DescriptorImageInfo ii[]{{handles::INVALID_SAMPLER,depth_,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,normal_,ResourceState::ShaderResource}};WriteDescriptorSet writes[5]{};
        for(u32 i=0;i<5;++i){auto& w=writes[i];w.dstSet=s.sets[0];w.dstBinding=i;w.descriptorCount=1;w.descriptorType=i==0?DescriptorType::UniformBuffer:i==1?DescriptorType::SampledDepthImage:i==2?DescriptorType::SampledImage:DescriptorType::StorageBuffer;if(i==1||i==2)w.imageInfo=&ii[i-1];else w.bufferInfo=&bi[i==0?0:i-2];}
        device_->UpdateDescriptorSets(5,writes);cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::MemoryWrite,AccessFlag::ShaderRead);Dispatch(cmd,s,0,(count_+63)/64);s.frame=frame;s.rayFrame=~0u;s.cpu=params;++nextFrame_;return true;
    }
    // A reset may use zero history only on frame 0. Later importance sampling
    // must receive the actual previous temporal/spatial SH; absence is failure.
    bool RecordRays(DawnCommandBuffer& cmd,u32 frame,History history){
        if(!ready_||!nextFrame_||frame!=nextFrame_-1)return false;auto& s=slots_[frame%3];if(s.frame!=frame||s.rayFrame==frame)return false;
        ResourceHandle sh=history.sh;
        if(sh==handles::INVALID_RESOURCE){if(frame!=0)return false;sh=zeroHistory_;}
        else if(frame==0||history.frame!=frame-1||history.probes!=count_)return false;
        if(!(s.cpu.sdfVoxels[0][0]>0)||!std::isfinite(s.cpu.sdfVoxels[0][0]))return false;
        DescriptorBufferInfo bi[]{{s.params,0,sizeof(Params)},{s.positions,0,u64(count_)*16},{s.normals,0,u64(count_)*16},{sh,0,u64(count_)*64},{s.rays,0,u64(count_)*rays_*32}};WriteDescriptorSet writes[5]{};for(u32 i=0;i<5;++i){auto& w=writes[i];w.dstSet=s.sets[1];w.dstBinding=i;w.descriptorCount=1;w.descriptorType=i?DescriptorType::StorageBuffer:DescriptorType::UniformBuffer;w.bufferInfo=&bi[i];}device_->UpdateDescriptorSets(5,writes);Dispatch(cmd,s,1,(count_*rays_+63)/64);s.rayFrame=frame;s.importanceSource={sh,frame?frame-1:~0u,count_};return true;
    }
    bool Capture(DawnCommandBuffer& cmd,u32 frame){
        if(!ready_||!nextFrame_||frame!=nextFrame_-1||readback_!=handles::INVALID_RESOURCE)return false;auto& s=slots_[frame%3];captured_=frame;hasRays_=s.rayFrame==frame;capturedParams_=s.cpu;
        BufferDesc b{};b.memoryUsage=GPUMemoryUsage::Readback;b.size=StateBytes();readback_=device_->CreateBuffer(b);if(readback_==handles::INVALID_RESOURCE)return false;
        cmd.CopyBuffer(s.positions,readback_,0,0,u64(count_)*16);cmd.CopyBuffer(s.normals,readback_,0,u64(count_)*16,u64(count_)*16);if(hasRays_)cmd.CopyBuffer(s.rays,readback_,0,u64(count_)*32,u64(count_)*rays_*32);
        for(u32 i=0;i<2;++i){imageFormats_[i]=device_->GetTexture(i?normal_:depth_)->GetTextureDesc().format;imageBpp_[i]=CapturePixelBytes(imageFormats_[i]);if(!imageBpp_[i])return false;pitch_[i]=(width_*imageBpp_[i]+255)&~255u;b.size=u64(pitch_[i])*height_;imageReadback_[i]=device_->CreateBuffer(b);if(imageReadback_[i]==handles::INVALID_RESOURCE)return false;BufferTextureCopyRegion copy{};copy.bufferRowLength=pitch_[i]/imageBpp_[i];copy.bufferImageHeight=height_;copy.imageExtent={width_,height_,1};cmd.CopyTextureToBuffer(i?normal_:depth_,imageReadback_[i],&copy,1);}return true;
    }
    bool Save(const std::string& prefix){
        if(readback_==handles::INVALID_RESOURCE)return false;auto data=static_cast<const float*>(device_->MapBuffer(readback_,0,StateBytes()));if(!data)return false;u32 invalid=0,active=0;for(u64 i=0;i<StateBytes()/4;++i)if(!std::isfinite(data[i]))++invalid;for(u32 i=0;i<count_;++i)active+=data[i*4+3]>0;
        std::ofstream f(prefix+".bin",std::ios::binary);f.write(reinterpret_cast<const char*>(&capturedParams_),sizeof(Params));f.write(reinterpret_cast<const char*>(data),StateBytes());f.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;bool ok=f.good();
        for(u32 i=0;i<2;++i){auto raw=static_cast<const char*>(device_->MapBuffer(imageReadback_[i],0,u64(pitch_[i])*height_));if(!raw)return false;std::ofstream image(prefix+(i?"-normal.bin":"-depth.bin"),std::ios::binary);for(u32 y=0;y<height_;++y)image.write(raw+u64(y)*pitch_[i],width_*imageBpp_[i]);image.close();ok&=image.good();device_->UnmapBuffer(imageReadback_[i]);device_->DestroyBuffer(imageReadback_[i]);imageReadback_[i]=handles::INVALID_RESOURCE;}
        std::ofstream meta(prefix+".json");meta<<"{\"frame\":"<<captured_<<",\"width\":"<<width_<<",\"height\":"<<height_<<",\"grid_width\":"<<gridX_<<",\"grid_height\":"<<gridY_<<",\"probes\":"<<count_<<",\"rays_per_probe\":"<<rays_<<",\"active\":"<<active<<",\"has_rays\":"<<(hasRays_?"true":"false")<<",\"invalid\":"<<invalid<<",\"normal_format\":\""<<CaptureFormatName(imageFormats_[1])<<"\",\"layout\":\"Params544; positions vec4; normals vec4; optional originWeight,directionCosine pairs\",\"completion_checked_by_caller\":true}\n";meta.close();return ok&&meta.good()&&!invalid;
    }
    u32 ProbeCount()const{return count_;}
    void Shutdown(){if(!device_)return;for(auto& s:slots_){for(auto h:s.sets)if(h!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(h);for(auto h:{s.params,s.positions,s.normals,s.rays})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);s=Slot{};}for(auto h:{zeroHistory_,readback_,imageReadback_[0],imageReadback_[1]})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);for(u32 i=0;i<2;++i){if(pipelines_[i]!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipelines_[i]);if(shaders_[i]!=handles::INVALID_SHADER)device_->DestroyShader(shaders_[i]);if(pipelineLayouts_[i]!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayouts_[i]);if(layouts_[i]!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layouts_[i]);}device_=nullptr;ready_=false;nextFrame_=0;zeroHistory_=readback_=handles::INVALID_RESOURCE;imageReadback_.fill(handles::INVALID_RESOURCE);pipelines_.fill(handles::INVALID_PIPELINE);shaders_.fill(handles::INVALID_SHADER);pipelineLayouts_.fill(handles::INVALID_PIPELINE_LAYOUT);layouts_.fill(handles::INVALID_DESCRIPTOR_SET_LAYOUT);}
private:
    struct Slot{ResourceHandle params=handles::INVALID_RESOURCE,positions=handles::INVALID_RESOURCE,normals=handles::INVALID_RESOURCE,rays=handles::INVALID_RESOURCE;std::array<DescriptorSetHandle,2> sets{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};u32 frame=~0u,rayFrame=~0u;Params cpu{};History importanceSource{};};
    void Dispatch(DawnCommandBuffer& cmd,Slot& s,u32 stage,u32 groups){cmd.BindComputePipeline(pipelines_[stage]);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayouts_[stage],0,1,&s.sets[stage],0,nullptr);cmd.Dispatch(groups,1,1);cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::AllCommands,AccessFlag::ShaderWrite,AccessFlag::MemoryRead);}
    u64 StateBytes()const{return u64(count_)*32+(hasRays_?u64(count_)*rays_*32:0);}
    DawnDevice* device_=nullptr;bool ready_=false,hasRays_=false;u32 width_=0,height_=0,spacing_=0,rays_=0,gridX_=0,gridY_=0,count_=0,nextFrame_=0,captured_=~0u,pitch_[2]{};Params capturedParams_{};
    DataFormat imageFormats_[2]{};u32 imageBpp_[2]{};
    ResourceHandle depth_=handles::INVALID_RESOURCE,normal_=handles::INVALID_RESOURCE,zeroHistory_=handles::INVALID_RESOURCE,readback_=handles::INVALID_RESOURCE;std::array<ResourceHandle,2> imageReadback_{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};std::array<Slot,3> slots_{};
    std::array<DescriptorSetLayoutHandle,2> layouts_{handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT};std::array<PipelineLayoutHandle,2> pipelineLayouts_{handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT};std::array<ShaderHandle,2> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER};std::array<PipelineHandle,2> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
};
}
