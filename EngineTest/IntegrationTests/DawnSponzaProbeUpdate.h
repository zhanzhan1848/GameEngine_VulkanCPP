#pragma once
#include "DawnSponzaMeasuredRadiance.h"
#include <iomanip>
namespace primal::tests::dawn_sponza {
class ProbeUpdate {
public:
    struct Params {u32 counts[4];float policy[4];};static_assert(sizeof(Params)==32);
    struct Config {u32 probes=0,raysPerProbe=128,budget=0;u64 sceneDigest=0;float maxDistance=50,voxel=.1f,shAlpha=.05f,depthAlpha=.2f;};
    ProbeUpdate()=default;ProbeUpdate(const ProbeUpdate&)=delete;ProbeUpdate& operator=(const ProbeUpdate&)=delete;~ProbeUpdate(){Shutdown();}
    bool Initialize(DawnDevice& device,Config config){
        if(device_||!config.probes||!config.sceneDigest||!config.budget||config.budget>config.probes||config.raysPerProbe<64||config.raysPerProbe>256||config.raysPerProbe%64)return false;
        for(float x:{config.maxDistance,config.voxel,config.shAlpha,config.depthAlpha})if(!std::isfinite(x))return false;
        if(config.maxDistance<=0||config.voxel<=0||config.shAlpha<0||config.shAlpha>1||config.depthAlpha<0||config.depthAlpha>1)return false;
        WGPULimits limits=WGPU_LIMITS_INIT;if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success||config.budget>limits.maxComputeWorkgroupsPerDimension||u64(config.probes)*512>limits.maxStorageBufferBindingSize||u64(config.budget)*config.raysPerProbe*32>limits.maxStorageBufferBindingSize||limits.maxStorageBuffersPerShaderStage<8)return false;
        device_=&device;config_=config;
        auto buffer=[&](u64 bytes,const void* data,BufferType type){BufferDesc b{};b.size=bytes;b.type=type;b.memoryUsage=GPUMemoryUsage::Static;auto h=device.CreateBuffer(b);if(h!=handles::INVALID_RESOURCE&&data&&!device.UpdateBufferData(h,data,bytes)){device.DestroyBuffer(h);return handles::INVALID_RESOURCE;}return h;};
        std::vector<float> sh(u64(config.probes)*36,0),depth(u64(config.probes)*128,0),stats(u64(config.probes)*8,0);for(u32 p=0;p<config.probes;++p)std::fill_n(depth.data()+p*128,64,-1.f);
        stats_=buffer(stats.size()*4,stats.data(),BufferType::Raw);if(stats_==handles::INVALID_RESOURCE)return false;
        DescriptorSetLayoutBinding b[9]{};for(u32 i=0;i<9;++i){b[i].binding=i;b[i].descriptorCount=1;b[i].stageFlags=ShaderStage::Compute;b[i].descriptorType=i?DescriptorType::StorageBuffer:DescriptorType::UniformBuffer;b[i].readonly=i>=1&&i<=4||i==6;}
        layout_=device.CreateDescriptorSetLayout({9,b});if(layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&layout_;pipelineLayout_=device.CreatePipelineLayout(pl);if(pipelineLayout_==handles::INVALID_PIPELINE_LAYOUT)return false;
        std::ifstream f("Engine/Graphics/Dawn/shaders/Lumen/DDGIMeasuredProbeUpdate.wgsl");std::string source((std::istreambuf_iterator<char>(f)),{});if(source.empty())return false;
        const char* entries[]{"measured_update_sh","measured_update_depth"};for(u32 i=0;i<2;++i){shaders_[i]=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,entries[i]);if(shaders_[i]==handles::INVALID_SHADER)return false;ComputePipelineDesc cp{};cp.computeShader=shaders_[i];cp.layout=pipelineLayout_;pipelines_[i]=device.CreateComputePipeline(cp);if(pipelines_[i]==handles::INVALID_PIPELINE)return false;}
        for(auto& s:slots_){s.sh=buffer(sh.size()*4,sh.data(),BufferType::Raw);s.depth=buffer(depth.size()*4,depth.data(),BufferType::Raw);s.params=buffer(sizeof(Params),nullptr,BufferType::Constant);s.set=device.CreateDescriptorSet({layout_});if(s.sh==handles::INVALID_RESOURCE||s.depth==handles::INVALID_RESOURCE||s.params==handles::INVALID_RESOURCE||s.set==handles::INVALID_DESCRIPTOR_SET)return false;}
        ready_=true;return true;
    }
    bool Record(DawnCommandBuffer& cmd,u32 frame,const MeasuredGeometry::RecordedOutput& geometry,const MeasuredRadiance::Output& measured,bool capture=false){
        const u32 rayCount=geometry.rayCount;
        if(!config_.raysPerProbe||rayCount%config_.raysPerProbe||rayCount/config_.raysPerProbe>config_.budget)return false;
        const u32 updates=rayCount/config_.raysPerProbe;
        if(!ready_||frame!=nextFrame_||geometry.frame!=frame||measured.frame!=frame||geometry.sceneDigest!=config_.sceneDigest||measured.sceneDigest!=config_.sceneDigest||!geometry.relocatedOriginsQualified||!geometry.originsGeneration||geometry.originsGeneration!=measured.originsGeneration||geometry.rayCount!=rayCount||measured.rayCount!=rayCount||geometry.probeCount!=config_.probes||geometry.raysPerProbe!=config_.raysPerProbe||measured.raysPerProbe!=config_.raysPerProbe||geometry.updateIndices!=measured.updates||geometry.updateIndices==handles::INVALID_RESOURCE||geometry.rays==handles::INVALID_RESOURCE||measured.radiance==handles::INVALID_RESOURCE)return false;
        if(generation_&&generation_!=geometry.originsGeneration)return false;if(capture&&readback_!=handles::INVALID_RESOURCE)return false;
        const auto indices=graphics::lumen::DDGISlotsForFrame(frame);auto& out=slots_[indices.writeIndex];auto& history=slots_[indices.previousIndex];
        Params params{{config_.probes,updates,config_.raysPerProbe,frame},{config_.maxDistance,config_.voxel,config_.shAlpha,config_.depthAlpha}};
        if(!device_->UpdateBufferData(out.params,&params,sizeof(params)))return false;
        // Copy ALL probes before sparse writes, so an unselected probe survives
        // slot reuse. Stats are one persistent buffer, like the frozen producer.
        cmd.CopyBuffer(history.sh,out.sh,0,0,SHBytes());cmd.CopyBuffer(history.depth,out.depth,0,0,DepthBytes());
        if(capture){capturedUpdates_=updates;BufferDesc b{};b.size=CaptureBytes();b.memoryUsage=GPUMemoryUsage::Readback;readback_=device_->CreateBuffer(b);if(readback_==handles::INVALID_RESOURCE)return false;
            cmd.CopyBuffer(history.sh,readback_,0,0,SHBytes());cmd.CopyBuffer(history.depth,readback_,0,SHBytes(),DepthBytes());cmd.CopyBuffer(stats_,readback_,0,SHBytes()+DepthBytes(),StatsBytes());
            const u64 offset=2*StateBytes();if(updates)cmd.CopyBuffer(geometry.updateIndices,readback_,0,offset,u64(updates)*4);if(updates)cmd.CopyBuffer(geometry.rays,readback_,0,offset+u64(updates)*4,u64(rayCount)*32);if(updates)cmd.CopyBuffer(measured.radiance,readback_,0,offset+u64(updates)*4+u64(rayCount)*32,u64(rayCount)*16);}
        DescriptorBufferInfo bi[]{{out.params,0,sizeof(params)},{measured.radiance,0,u64(std::max(1u,rayCount))*16},{geometry.rays,0,u64(std::max(1u,rayCount))*32},{geometry.updateIndices,0,u64(std::max(1u,updates))*4},{history.sh,0,SHBytes()},{out.sh,0,SHBytes()},{history.depth,0,DepthBytes()},{out.depth,0,DepthBytes()},{stats_,0,StatsBytes()}};
        WriteDescriptorSet writes[9]{};for(u32 i=0;i<9;++i){auto& w=writes[i];w.dstSet=out.set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=i?DescriptorType::StorageBuffer:DescriptorType::UniformBuffer;w.bufferInfo=&bi[i];}device_->UpdateDescriptorSets(9,writes);
        if(updates)for(auto p:pipelines_){cmd.BindComputePipeline(p);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayout_,0,1,&out.set,0,nullptr);cmd.Dispatch(updates,1,1);cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead);}
        if(capture){cmd.CopyBuffer(out.sh,readback_,0,StateBytes(),SHBytes());cmd.CopyBuffer(out.depth,readback_,0,StateBytes()+SHBytes(),DepthBytes());cmd.CopyBuffer(stats_,readback_,0,StateBytes()+SHBytes()+DepthBytes(),StatsBytes());cmd.MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);captured_=frame;}
        out.frame=frame;generation_=geometry.originsGeneration;++nextFrame_;return true;
    }
    struct Output {ResourceHandle sh=handles::INVALID_RESOURCE,depth=handles::INVALID_RESOURCE,stats=handles::INVALID_RESOURCE;u32 probes=0,frame=~0u;u64 sceneDigest=0,originsGeneration=0;};
    Output GetRecordedOutput(u32 frame)const{if(!ready_||!nextFrame_||frame!=nextFrame_-1)return {};const auto& s=slots_[graphics::lumen::DDGISlotsForFrame(frame).writeIndex];return {s.sh,s.depth,stats_,config_.probes,frame,config_.sceneDigest,generation_};}
    struct History {Output state{};bool reset=false;};
    // Only while the current frame has been recorded: the preceding slot is
    // still immutable. At frame zero it contains the initialized zero SH and
    // negative (unknown) depth, not a fabricated completed measurement.
    History GetHistoryForFrame(u32 frame)const{
        if(!ready_||!nextFrame_||frame!=nextFrame_-1)return {};
        const auto& s=slots_[graphics::lumen::DDGISlotsForFrame(frame).previousIndex];
        if(s.frame!=(frame?frame-1:~0u))return {};
        return {{s.sh,s.depth,handles::INVALID_RESOURCE,config_.probes,s.frame,config_.sceneDigest,generation_},frame==0};
    }
    // Caller has checked End/Submit/Wait. Layout: before state, after state,
    // updateIndices, GeometryRay[], radiance[]. Each state: SH, depth, stats.
    bool VerifyAndSave(const std::string& prefix){
        if(readback_==handles::INVALID_RESOURCE)return false;const auto* data=static_cast<const u8*>(device_->MapBuffer(readback_,0,CaptureBytes()));if(!data)return false;
        u32 invalid=0,updated=0;float maxSH=0,maxVariance=0;const auto* sh=reinterpret_cast<const float*>(data+StateBytes());const auto* depth=reinterpret_cast<const float*>(data+StateBytes()+SHBytes());const auto* stats=reinterpret_cast<const float*>(data+StateBytes()+SHBytes()+DepthBytes());
        for(u32 p=0;p<config_.probes;++p){updated+=stats[p*8+3]>0;for(u32 j=0;j<36;++j){float x=sh[p*36+j];maxSH=std::max(maxSH,std::abs(x));if(!std::isfinite(x))++invalid;}
            for(u32 j=0;j<64;++j){float mean=depth[p*128+j],variance=depth[p*128+64+j];maxVariance=std::max(maxVariance,variance);if(!std::isfinite(mean)||!std::isfinite(variance)||(mean<0&&mean!=-1)||variance<0)++invalid;}
            for(u32 j=0;j<8;++j)if(!std::isfinite(stats[p*8+j])||stats[p*8+j]<0||stats[p*8+j]>1)++invalid;}
        std::ofstream f(prefix+".bin",std::ios::binary);f.write(reinterpret_cast<const char*>(data),CaptureBytes());f.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        std::ofstream m(prefix+".json");m << std::setprecision(9) << "{\"frame\":" << captured_ << ",\"probes\":" << config_.probes << ",\"budget\":" << capturedUpdates_ << ",\"rays_per_probe\":" << config_.raysPerProbe << ",\"max_distance\":" << config_.maxDistance << ",\"voxel\":" << config_.voxel << ",\"sh_alpha\":" << config_.shAlpha << ",\"depth_alpha\":" << config_.depthAlpha << ",\"updated_probes\":" << updated << ",\"invalid\":" << invalid << ",\"max_sh_component\":" << maxSH << ",\"max_variance\":" << maxVariance << ",\"completion_checked\":true}\n";m.close();
        PRIMAL_LOG_STREAM(Info,"DawnProbeUpdate") << "frame=" << captured_ << " updated=" << updated << " invalid=" << invalid << " maxSH=" << maxSH << " maxVariance=" << maxVariance;return f.good()&&m.good()&&!invalid;
    }
    u64 SHBytes()const{return u64(config_.probes)*144;}u64 DepthBytes()const{return u64(config_.probes)*512;}u64 StatsBytes()const{return u64(config_.probes)*32;}u64 StateBytes()const{return SHBytes()+DepthBytes()+StatsBytes();}u64 CaptureBytes()const{return StateBytes()*2+u64(capturedUpdates_)*4+u64(capturedUpdates_)*config_.raysPerProbe*48;}
    void Shutdown(){if(!device_)return;for(auto& s:slots_){if(s.set!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(s.set);for(auto h:{s.sh,s.depth,s.params})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);s=Slot{};}
        for(auto h:pipelines_)if(h!=handles::INVALID_PIPELINE)device_->DestroyPipeline(h);for(auto h:shaders_)if(h!=handles::INVALID_SHADER)device_->DestroyShader(h);if(pipelineLayout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayout_);if(layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layout_);for(auto h:{stats_,readback_})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);
        device_=nullptr;ready_=false;nextFrame_=0;generation_=0;stats_=readback_=handles::INVALID_RESOURCE;pipelines_.fill(handles::INVALID_PIPELINE);shaders_.fill(handles::INVALID_SHADER);pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
private:
    struct Slot {ResourceHandle sh=handles::INVALID_RESOURCE,depth=handles::INVALID_RESOURCE,params=handles::INVALID_RESOURCE;DescriptorSetHandle set=handles::INVALID_DESCRIPTOR_SET;u32 frame=~0u;};
    Config config_{};DawnDevice* device_=nullptr;bool ready_=false;u32 nextFrame_=0,captured_=~0u,capturedUpdates_=0;u64 generation_=0;std::array<Slot,3> slots_{};
    ResourceHandle stats_=handles::INVALID_RESOURCE,readback_=handles::INVALID_RESOURCE;std::array<ShaderHandle,2> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER};std::array<PipelineHandle,2> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};PipelineLayoutHandle pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;DescriptorSetLayoutHandle layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
};
}
