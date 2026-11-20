#pragma once
#include "DawnSponzaScreenProbeAverage.h"
namespace primal::tests::dawn_sponza {
// Final SPGI SH ownership: Average -> Temporal -> measured Spatial. Positions
// and normals are copied into the same slot, so reprojection never aliases the
// next placement write. Submit frames in order on the same WebGPU queue;
// queue writes are ordered after earlier submissions. Wait before readback.
class ScreenProbeHistory {
public:
    struct alignas(16) TemporalParams {
        primal::math::m4x4 previousVP;u32 probes=0;float alpha=.1f,positionThreshold=.5f,normalThreshold=.5f;
        float clampScale=2,maxGradient=2;u32 width=0,height=0;float mode=0,pad0=0,pad1=0,pad2=0;
    };static_assert(sizeof(TemporalParams)==112);static_assert(offsetof(TemporalParams,mode)==96);
    struct SpatialParams {u32 probes=0,width=0;float sigma=2;u32 flags=3;};static_assert(sizeof(SpatialParams)==16);
    struct Output {ResourceHandle sh=handles::INVALID_RESOURCE,positions=handles::INVALID_RESOURCE,normals=handles::INVALID_RESOURCE;u32 frame=~0u,probes=0;};
    static std::string Source(u32 stage){
        auto read=[](const std::string& name){std::ifstream f(name);return std::string((std::istreambuf_iterator<char>(f)),{});};
        auto kernel=read(std::string("Engine/Graphics/Dawn/shaders/Lumen/ScreenProbeMeasured")+(stage?"Spatial":"Temporal")+".wgsl");if(!stage||kernel.empty())return kernel;
        auto common=read("Engine/Graphics/Dawn/shaders/Lumen/GeometryFirstHit.wgsl");auto end=common.find("fn geometryClassifyMeasured(");if(end==std::string::npos)return {};common.resize(end);
        for(u32 i=4;i<=6;++i){auto at=common.find("@group(0) @binding("+std::to_string(i)+")");if(at==std::string::npos)return {};auto nl=common.find('\n',at);if(nl==std::string::npos)return {};common.erase(at,nl-at+1);}
        for(u32 i=0;i<4;++i){auto before="@group(0) @binding("+std::to_string(i)+")";auto at=common.find(before);if(at==std::string::npos)return {};common.replace(at,before.size(),"@group(0) @binding("+std::to_string(i+5)+")");}
        const std::string before="let counts=geometryParams.counts;";auto at=common.find(before);if(at==std::string::npos)return {};
        common.replace(at,before.size(),"let counts=vec4<u32>(arrayLength(&geometryTop),arrayLength(&geometryBottom),arrayLength(&geometryTriangles),arrayLength(&geometryInstances));");return common+kernel;
    }
    ScreenProbeHistory()=default;ScreenProbeHistory(const ScreenProbeHistory&)=delete;ScreenProbeHistory& operator=(const ScreenProbeHistory&)=delete;~ScreenProbeHistory(){Shutdown();}
    bool Initialize(DawnDevice& device,u32 width,u32 height,const MeasuredGeometry::SceneView& scene){
        if(device_||!width||!height||!scene.digest||u64(width)*height>UINT32_MAX)return false;
        WGPULimits limits=WGPU_LIMITS_INIT;const u64 n=u64(width)*height;if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success||limits.maxStorageBuffersPerShaderStage<8||n*64>limits.maxStorageBufferBindingSize||(n+63)/64>limits.maxComputeWorkgroupsPerDimension)return false;
        device_=&device;width_=width;height_=height;count_=u32(n);scene_=scene;const u32 strides[]{48,48,48,64};
        for(u32 i=0;i<4;++i)if(!scene.counts[i]||!Fits(scene.buffers[i],u64(scene.counts[i])*strides[i]))return false;
        auto buffer=[&](u64 bytes,BufferType type){BufferDesc b{};b.size=bytes;b.type=type;b.memoryUsage=GPUMemoryUsage::Static;return device.CreateBuffer(b);};
        zero_=buffer(n*64,BufferType::Raw);std::vector<u8> zero(n*64);if(zero_==handles::INVALID_RESOURCE||!device.UpdateBufferData(zero_,zero.data(),zero.size()))return false;
        for(u32 stage=0;stage<2;++stage){const u32 bindingsCount=stage?9:8;DescriptorSetLayoutBinding bindings[9]{};
            for(u32 i=0;i<bindingsCount;++i){auto& b=bindings[i];b.binding=i;b.descriptorCount=1;b.stageFlags=ShaderStage::Compute;b.descriptorType=i==(stage?3u:5u)?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer;b.readonly=b.descriptorType==DescriptorType::StorageBuffer&&i!=(stage?1u:4u);}
            layouts_[stage]=device.CreateDescriptorSetLayout({bindingsCount,bindings});if(layouts_[stage]==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&layouts_[stage];pipelineLayouts_[stage]=device.CreatePipelineLayout(pl);if(pipelineLayouts_[stage]==handles::INVALID_PIPELINE_LAYOUT)return false;
            const auto source=Source(stage);if(source.empty())return false;shaders_[stage]=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,stage?"measured_screen_spatial":"measured_screen_temporal");if(shaders_[stage]==handles::INVALID_SHADER)return false;ComputePipelineDesc cp{};cp.layout=pipelineLayouts_[stage];cp.computeShader=shaders_[stage];pipelines_[stage]=device.CreateComputePipeline(cp);if(pipelines_[stage]==handles::INVALID_PIPELINE)return false;
        }
        for(auto& s:slots_){s.temporalParams=buffer(112,BufferType::Constant);s.spatialParams=buffer(16,BufferType::Constant);s.temporal=buffer(n*64,BufferType::Raw);s.sh=buffer(n*64,BufferType::Raw);s.positions=buffer(n*16,BufferType::Raw);s.normals=buffer(n*16,BufferType::Raw);
            for(auto h:{s.temporalParams,s.spatialParams,s.temporal,s.sh,s.positions,s.normals})if(h==handles::INVALID_RESOURCE)return false;
            for(u32 stage=0;stage<2;++stage){s.sets[stage]=device.CreateDescriptorSet({layouts_[stage]});if(s.sets[stage]==handles::INVALID_DESCRIPTOR_SET)return false;}}
        ready_=true;return true;
    }
    ScreenProbePlacement::History ProposalForFrame(u32 frame)const{
        if(!ready_||frame!=nextFrame_||!frame)return {};const auto& s=slots_[(frame-1)%3];if(s.frame!=frame-1)return {};return {s.sh,frame-1,count_};
    }
    bool Record(DawnCommandBuffer& cmd,u32 frame,const ScreenProbeAverage::Output& average,const MeasuredGeometry::SceneView& scene){
        const auto& source=average.source;const auto& p=source.params;
        if(!ready_||frame!=nextFrame_||average.frame!=frame||source.frame!=frame||average.probes!=count_||source.probes!=count_||p.grid[0]!=float(width_)||p.grid[1]!=float(height_)||scene.digest!=scene_.digest||scene.buffers!=scene_.buffers||scene.counts!=scene_.counts)return false;
        if(frame){const auto proposal=ProposalForFrame(frame);if(source.importanceSource.sh!=proposal.sh||source.importanceSource.frame!=proposal.frame||source.importanceSource.probes!=proposal.probes)return false;}
        else if(source.importanceSource.frame!=~0u||source.importanceSource.probes!=count_)return false;
        if(!Fits(average.sh,u64(count_)*64)||!Fits(source.positions,u64(count_)*16)||!Fits(source.normals,u64(count_)*16))return false;
        auto& s=slots_[frame%3];const Slot* history=frame?&slots_[(frame-1)%3]:nullptr;if(history&&history->frame!=frame-1)return false;
        TemporalParams temporal{};temporal.previousVP=history?history->vp:p.viewProjection;temporal.probes=count_;temporal.alpha=frame?.1f:1.f;temporal.width=width_;temporal.height=height_;SpatialParams spatial{count_,width_,2,3};
        if(!device_->UpdateBufferData(s.temporalParams,&temporal,sizeof(temporal))||!device_->UpdateBufferData(s.spatialParams,&spatial,sizeof(spatial)))return false;
        const u64 shBytes=u64(count_)*64,pointBytes=u64(count_)*16;
        const DescriptorBufferInfo temporalInputs[]{{average.sh,0,shBytes},{history?history->sh:zero_,0,shBytes},{source.positions,0,pointBytes},{history?history->positions:zero_,0,pointBytes},{s.temporal,0,shBytes},{s.temporalParams,0,112},{source.normals,0,pointBytes},{history?history->normals:zero_,0,pointBytes}};
        const DescriptorBufferInfo spatialInputs[]{{s.temporal,0,shBytes},{s.sh,0,shBytes},{source.positions,0,pointBytes},{s.spatialParams,0,16},{source.normals,0,pointBytes},{scene_.buffers[0],0,u64(scene_.counts[0])*48},{scene_.buffers[1],0,u64(scene_.counts[1])*48},{scene_.buffers[2],0,u64(scene_.counts[2])*48},{scene_.buffers[3],0,u64(scene_.counts[3])*64}};
        for(u32 stage=0;stage<2;++stage){const u32 n=stage?9:8;WriteDescriptorSet writes[9]{};for(u32 i=0;i<n;++i){auto& w=writes[i];w.dstSet=s.sets[stage];w.dstBinding=i;w.descriptorCount=1;w.descriptorType=i==(stage?3u:5u)?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer;w.bufferInfo=stage?&spatialInputs[i]:&temporalInputs[i];}device_->UpdateDescriptorSets(n,writes);
            cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::MemoryWrite,AccessFlag::ShaderRead);cmd.BindComputePipeline(pipelines_[stage]);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayouts_[stage],0,1,&s.sets[stage],0,nullptr);cmd.Dispatch((count_+63)/64,1,1);}
        cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);cmd.CopyBuffer(source.positions,s.positions,0,0,pointBytes);cmd.CopyBuffer(source.normals,s.normals,0,0,pointBytes);cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::MemoryWrite,AccessFlag::ShaderRead);
        s.inputs={temporalInputs[0],temporalInputs[1],temporalInputs[3],temporalInputs[7]};s.frame=frame;s.vp=p.viewProjection;s.cpuTemporal=temporal;s.cpuSpatial=spatial;++nextFrame_;return true;
    }
    Output GetOutput(u32 frame)const{if(!ready_||!nextFrame_||frame!=nextFrame_-1)return {};const auto& s=slots_[frame%3];return {s.sh,s.positions,s.normals,frame,count_};}
    bool Capture(DawnCommandBuffer& cmd,u32 frame){
        if(GetOutput(frame).sh==handles::INVALID_RESOURCE||readback_!=handles::INVALID_RESOURCE)return false;auto& s=slots_[frame%3];captured_=frame;capturedTemporal_=s.cpuTemporal;capturedSpatial_=s.cpuSpatial;BufferDesc b{};b.size=u64(count_)*320;b.memoryUsage=GPUMemoryUsage::Readback;readback_=device_->CreateBuffer(b);if(readback_==handles::INVALID_RESOURCE)return false;
        cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::Transfer,AccessFlag::MemoryWrite,AccessFlag::TransferRead);const ResourceHandle h[]{s.temporal,s.sh,s.positions,s.normals};const u64 sizes[]{u64(count_)*64,u64(count_)*64,u64(count_)*16,u64(count_)*16};u64 offset=0;for(u32 i=0;i<4;++i){cmd.CopyBuffer(h[i],readback_,0,offset,sizes[i]);offset+=sizes[i];}for(const auto& input:s.inputs){cmd.CopyBuffer(input.buffer,readback_,input.offset,offset,input.range);offset+=input.range;}cmd.MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);return true;
    }
    bool Save(const std::string& prefix){
        if(readback_==handles::INVALID_RESOURCE)return false;const u64 bytes=u64(count_)*320;const auto* raw=static_cast<const float*>(device_->MapBuffer(readback_,0,bytes));if(!raw)return false;u32 invalid=0,inactive=0,uncleared=0;
        for(u64 i=0;i<u64(count_)*32;++i)if(!std::isfinite(raw[i]))++invalid;
        for(u32 p=0;p<count_;++p)if(raw[u64(count_)*32+p*4+3]<=0){++inactive;for(u32 b=0;b<16;++b)if(raw[p*16+b]!=0||raw[u64(count_)*16+p*16+b]!=0)++uncleared;}
        std::ofstream f(prefix+".bin",std::ios::binary);f.write(reinterpret_cast<const char*>(&capturedTemporal_),112);f.write(reinterpret_cast<const char*>(&capturedSpatial_),16);f.write(reinterpret_cast<const char*>(raw),bytes);f.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        std::ofstream meta(prefix+".json");meta<<"{\"frame\":"<<captured_<<",\"probes\":"<<count_<<",\"invalid_sh\":"<<invalid<<",\"inactive\":"<<inactive<<",\"uncleared\":"<<uncleared<<",\"final_history\":true,\"layout\":\"Temporal112,Spatial16,temporal SH4,final SH4,positions,normals,current SH4,previous final SH4,previous positions,previous normals\",\"completion_checked_by_caller\":true}\n";meta.close();return f.good()&&meta.good()&&!invalid&&!uncleared;
    }
    void Shutdown(){if(!device_)return;for(auto& s:slots_){for(auto set:s.sets)if(set!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(set);for(auto h:{s.temporalParams,s.spatialParams,s.temporal,s.sh,s.positions,s.normals})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);s=Slot{};}for(auto h:{zero_,readback_})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);for(u32 i=0;i<2;++i){if(pipelines_[i]!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipelines_[i]);if(shaders_[i]!=handles::INVALID_SHADER)device_->DestroyShader(shaders_[i]);if(pipelineLayouts_[i]!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayouts_[i]);if(layouts_[i]!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layouts_[i]);}device_=nullptr;ready_=false;nextFrame_=0;zero_=readback_=handles::INVALID_RESOURCE;pipelines_.fill(handles::INVALID_PIPELINE);shaders_.fill(handles::INVALID_SHADER);pipelineLayouts_.fill(handles::INVALID_PIPELINE_LAYOUT);layouts_.fill(handles::INVALID_DESCRIPTOR_SET_LAYOUT);}
private:
    bool Fits(ResourceHandle h,u64 size)const{const auto* b=device_->GetBuffer(h);return b&&b->GetBufferDesc().size>=size;}
    struct Slot {ResourceHandle temporalParams=handles::INVALID_RESOURCE,spatialParams=handles::INVALID_RESOURCE,temporal=handles::INVALID_RESOURCE,sh=handles::INVALID_RESOURCE,positions=handles::INVALID_RESOURCE,normals=handles::INVALID_RESOURCE;std::array<DescriptorSetHandle,2> sets{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};u32 frame=~0u;primal::math::m4x4 vp;TemporalParams cpuTemporal{};SpatialParams cpuSpatial{};std::array<DescriptorBufferInfo,4> inputs{};};
    DawnDevice* device_=nullptr;bool ready_=false;u32 width_=0,height_=0,count_=0,nextFrame_=0,captured_=~0u;MeasuredGeometry::SceneView scene_{};std::array<Slot,3> slots_{};ResourceHandle zero_=handles::INVALID_RESOURCE,readback_=handles::INVALID_RESOURCE;TemporalParams capturedTemporal_{};SpatialParams capturedSpatial_{};
    std::array<DescriptorSetLayoutHandle,2> layouts_{handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT};std::array<PipelineLayoutHandle,2> pipelineLayouts_{handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT};std::array<ShaderHandle,2> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER};std::array<PipelineHandle,2> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
};
}
