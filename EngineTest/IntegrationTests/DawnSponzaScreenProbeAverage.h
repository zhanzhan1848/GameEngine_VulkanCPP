#pragma once
#include "DawnSponzaScreenProbeFinalize.h"
namespace primal::tests::dawn_sponza {
// Raw projection only. Its output is NOT the final temporal/spatial history.
class ScreenProbeAverage {
public:
    struct alignas(16) Params {
        u32 totalProbes=0,raysPerProbe=0;float frameSeed=0,isMode=1;
        u32 tier3Mode=0,tierRaysMode=0,sh2CheckMode=0,pad0=0;
        float skyConfFloor=.25f,pad1=0,pad2=0,pad3=0;
    };static_assert(sizeof(Params)==48);static_assert(offsetof(Params,skyConfFloor)==32);
    struct Output {ResourceHandle sh=handles::INVALID_RESOURCE,openSky=handles::INVALID_RESOURCE;u32 frame=~0u,probes=0;ScreenProbePlacement::Output source{};};
    ScreenProbeAverage()=default;ScreenProbeAverage(const ScreenProbeAverage&)=delete;ScreenProbeAverage& operator=(const ScreenProbeAverage&)=delete;~ScreenProbeAverage(){Shutdown();}
    bool Initialize(DawnDevice& device,u32 probes,u32 rays){
        if(device_||!probes||rays<8||rays>64)return false;
        WGPULimits limits=WGPU_LIMITS_INIT;if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success||probes>limits.maxComputeWorkgroupsPerDimension||u64(probes)*rays*16>limits.maxStorageBufferBindingSize)return false;
        device_=&device;probes_=probes;rays_=rays;
        DescriptorSetLayoutBinding bindings[6]{};const u32 numbers[]{0,1,2,3,4,7};
        for(u32 i=0;i<6;++i){auto& b=bindings[i];b.binding=numbers[i];b.descriptorCount=1;b.stageFlags=ShaderStage::Compute;b.descriptorType=i==2?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer;b.readonly=i==0||i==3||i==4;}
        layout_=device.CreateDescriptorSetLayout({6,bindings});if(layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
        PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&layout_;pipelineLayout_=device.CreatePipelineLayout(pl);if(pipelineLayout_==handles::INVALID_PIPELINE_LAYOUT)return false;
        std::ifstream f("Engine/Graphics/Dawn/shaders/Lumen/ScreenProbeMeasuredAverage.wgsl");const std::string source((std::istreambuf_iterator<char>(f)),{});if(source.empty())return false;
        shader_=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,"measured_screen_average");if(shader_==handles::INVALID_SHADER)return false;
        ComputePipelineDesc cp{};cp.layout=pipelineLayout_;cp.computeShader=shader_;pipeline_=device.CreateComputePipeline(cp);if(pipeline_==handles::INVALID_PIPELINE)return false;
        for(auto& s:slots_){BufferDesc b{};b.type=BufferType::Constant;b.size=sizeof(Params);b.memoryUsage=GPUMemoryUsage::Static;s.params=device.CreateBuffer(b);b.type=BufferType::Raw;b.size=u64(probes)*64;s.sh=device.CreateBuffer(b);b.size=u64(probes)*16;s.sky=device.CreateBuffer(b);s.set=device.CreateDescriptorSet({layout_});if(s.params==handles::INVALID_RESOURCE||s.sh==handles::INVALID_RESOURCE||s.sky==handles::INVALID_RESOURCE||s.set==handles::INVALID_DESCRIPTOR_SET)return false;}
        ready_=true;return true;
    }
    bool Record(DawnCommandBuffer& cmd,u32 frame,const ScreenProbeFinalize::Output& finalized){
        const auto& source=finalized.source;const auto& p=source.params;const auto& history=source.importanceSource;
        if(!ready_||frame!=nextFrame_||finalized.frame!=frame||source.frame!=frame||finalized.probes!=probes_||source.probes!=probes_||finalized.rays!=rays_||source.raysPerProbe!=rays_)return false;
        if(history.frame!=(frame?frame-1:~0u)||history.probes!=probes_||p.tier[0]!=0||p.tier[3]!=0||!std::isfinite(p.camera[3])||p.camera[3]>=16777216.f||!std::isfinite(p.cache[1]))return false;
        if(!Fits(finalized.radiance,u64(probes_)*rays_*16)||!Fits(source.normals,u64(probes_)*16)||!Fits(history.sh,u64(probes_)*64))return false;
        auto& s=slots_[frame%3];s.cpu={probes_,rays_,p.camera[3],p.cache[1]};if(!device_->UpdateBufferData(s.params,&s.cpu,sizeof(Params)))return false;
        const u32 numbers[]{0,1,2,3,4,7};const DescriptorBufferInfo bi[]{{finalized.radiance,0,u64(probes_)*rays_*16},{s.sh,0,u64(probes_)*64},{s.params,0,sizeof(Params)},{source.normals,0,u64(probes_)*16},{history.sh,0,u64(probes_)*64},{s.sky,0,u64(probes_)*16}};
        WriteDescriptorSet writes[6]{};for(u32 i=0;i<6;++i){auto& w=writes[i];w.dstSet=s.set;w.dstBinding=numbers[i];w.descriptorCount=1;w.descriptorType=i==2?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer;w.bufferInfo=&bi[i];}device_->UpdateDescriptorSets(6,writes);
        cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::MemoryWrite,AccessFlag::ShaderRead);cmd.BindComputePipeline(pipeline_);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayout_,0,1,&s.set,0,nullptr);cmd.Dispatch(probes_,1,1);cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::AllCommands,AccessFlag::ShaderWrite,AccessFlag::MemoryRead);s.frame=frame;s.source=source;++nextFrame_;return true;
    }
    Output GetOutput(u32 frame)const{if(!ready_||!nextFrame_||frame!=nextFrame_-1)return {};const auto& s=slots_[frame%3];return {s.sh,s.sky,frame,probes_,s.source};}
    bool Capture(DawnCommandBuffer& cmd,u32 frame){
        if(GetOutput(frame).sh==handles::INVALID_RESOURCE||readback_!=handles::INVALID_RESOURCE)return false;captured_=frame;capturedParams_=slots_[frame%3].cpu;BufferDesc b{};b.memoryUsage=GPUMemoryUsage::Readback;b.size=u64(probes_)*80;readback_=device_->CreateBuffer(b);if(readback_==handles::INVALID_RESOURCE)return false;
        cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);cmd.CopyBuffer(slots_[frame%3].sh,readback_,0,0,u64(probes_)*64);cmd.CopyBuffer(slots_[frame%3].sky,readback_,0,u64(probes_)*64,u64(probes_)*16);cmd.MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);return true;
    }
    // Caller must wait for the submission containing Capture before Save.
    bool Save(const std::string& prefix){
        if(readback_==handles::INVALID_RESOURCE)return false;const u64 bytes=u64(probes_)*80;const auto* data=static_cast<const float*>(device_->MapBuffer(readback_,0,bytes));if(!data)return false;u32 invalid=0,valid=0;for(u64 i=0;i<bytes/4;++i)if(!std::isfinite(data[i]))++invalid;for(u32 i=0;i<probes_;++i)valid+=data[i*16+3]>0;
        std::ofstream f(prefix+".bin",std::ios::binary);f.write(reinterpret_cast<const char*>(&capturedParams_),sizeof(Params));f.write(reinterpret_cast<const char*>(data),bytes);f.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        std::ofstream meta(prefix+".json");meta<<"{\"frame\":"<<captured_<<",\"probes\":"<<probes_<<",\"rays_per_probe\":"<<rays_<<",\"valid_projection\":"<<valid<<",\"invalid\":"<<invalid<<",\"layout\":\"Params48,SH4 vec4,OpenSky vec4\",\"final_history\":false,\"completion_checked_by_caller\":true}\n";meta.close();// Frozen Average normalizes inactive zero normals; L1 may be NaN.
        // Temporal owns inactive-position rejection. Preserve raw evidence here.
        return f.good()&&meta.good();
    }
    void Shutdown(){if(!device_)return;for(auto& s:slots_){if(s.set!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(s.set);for(auto h:{s.params,s.sh,s.sky})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);s=Slot{};}if(readback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(readback_);if(pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipeline_);if(shader_!=handles::INVALID_SHADER)device_->DestroyShader(shader_);if(pipelineLayout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayout_);if(layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layout_);device_=nullptr;ready_=false;nextFrame_=0;readback_=handles::INVALID_RESOURCE;pipeline_=handles::INVALID_PIPELINE;shader_=handles::INVALID_SHADER;pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;}
private:
    bool Fits(ResourceHandle handle,u64 bytes)const{const auto* b=device_->GetBuffer(handle);return b&&b->GetBufferDesc().size>=bytes;}
    struct Slot {ResourceHandle params=handles::INVALID_RESOURCE,sh=handles::INVALID_RESOURCE,sky=handles::INVALID_RESOURCE;DescriptorSetHandle set=handles::INVALID_DESCRIPTOR_SET;u32 frame=~0u;Params cpu{};ScreenProbePlacement::Output source{};};
    DawnDevice* device_=nullptr;bool ready_=false;u32 probes_=0,rays_=0,nextFrame_=0,captured_=~0u;Params capturedParams_{};std::array<Slot,3> slots_{};ResourceHandle readback_=handles::INVALID_RESOURCE;
    DescriptorSetLayoutHandle layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;PipelineLayoutHandle pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;ShaderHandle shader_=handles::INVALID_SHADER;PipelineHandle pipeline_=handles::INVALID_PIPELINE;
};
}
