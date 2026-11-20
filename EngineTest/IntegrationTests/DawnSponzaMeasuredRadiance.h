#pragma once
#include "DawnSponzaGeometry.h"
#include "Engine/Graphics/Lumen/DDGISlotMap.h"
namespace primal::tests::dawn_sponza {
// Borrowed inputs must outlive this static-scene producer. Its compact ray
// order is the measured update-list order, not the full probe-array order.
class MeasuredRadiance {
public:
    struct alignas(16) Params {u32 counts[4]{};float sky[4]{2,2,2,0};};
    static_assert(sizeof(Params)==32);
    struct Inputs {ResourceHandle cards=handles::INVALID_RESOURCE,directory=handles::INVALID_RESOURCE,depth=handles::INVALID_RESOURCE;u32 rayCount=0,cardCount=0,ownerCount=0;u64 sceneDigest=0;float sky[4]{2,2,2,0};};
    MeasuredRadiance()=default;MeasuredRadiance(const MeasuredRadiance&)=delete;MeasuredRadiance& operator=(const MeasuredRadiance&)=delete;~MeasuredRadiance(){Shutdown();}
    bool Initialize(DawnDevice& device,const Inputs& in){
        if(device_||!in.rayCount||!in.cardCount||!in.ownerCount||!in.sceneDigest||in.cards==handles::INVALID_RESOURCE||in.directory==handles::INVALID_RESOURCE||in.depth==handles::INVALID_RESOURCE)return false;
        for(float x:in.sky)if(!std::isfinite(x)||x<0)return false;
        WGPULimits limits=WGPU_LIMITS_INIT;if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success||u64(in.rayCount)*96>limits.maxStorageBufferBindingSize||(in.rayCount+63)/64>limits.maxComputeWorkgroupsPerDimension)return false;
        device_=&device;input_=in;
        BufferDesc bd{};bd.memoryUsage=GPUMemoryUsage::Static;
        DescriptorSetLayoutBinding b[9]{};for(u32 i=0;i<9;++i){b[i].binding=i;b[i].descriptorCount=1;b[i].stageFlags=ShaderStage::Compute;b[i].descriptorType=i<6?DescriptorType::StorageBuffer:i==6?DescriptorType::UniformBuffer:DescriptorType::SampledImage;b[i].readonly=i<4;}
        b[7].unfilterableFloat=true;layout_=device.CreateDescriptorSetLayout({9,b});if(layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
        PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&layout_;pipelineLayout_=device.CreatePipelineLayout(pl);if(pipelineLayout_==handles::INVALID_PIPELINE_LAYOUT)return false;
        std::ifstream file("Engine/Graphics/Dawn/shaders/Lumen/DDGIMeasuredRadiance.wgsl");std::string source((std::istreambuf_iterator<char>(file)),{});if(source.empty())return false;
        shader_=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,"measured_radiance");if(shader_==handles::INVALID_SHADER)return false;
        ComputePipelineDesc cp{};cp.computeShader=shader_;cp.layout=pipelineLayout_;pipeline_=device.CreateComputePipeline(cp);if(pipeline_==handles::INVALID_PIPELINE)return false;
        for(auto& s:slots_){bd.size=sizeof(Params);bd.type=BufferType::Constant;s.uniform=device.CreateBuffer(bd);if(s.uniform==handles::INVALID_RESOURCE)return false;bd.size=u64(in.rayCount)*16;bd.type=BufferType::Raw;s.radiance=device.CreateBuffer(bd);s.audit=device.CreateBuffer(bd);s.set=device.CreateDescriptorSet({layout_});if(s.radiance==handles::INVALID_RESOURCE||s.audit==handles::INVALID_RESOURCE||s.set==handles::INVALID_DESCRIPTOR_SET)return false;}
        ready_=true;return true;
    }
    bool Record(DawnCommandBuffer& cmd,u32 frame,const MeasuredGeometry::RecordedOutput& measured,ResourceHandle lighting){
        if(!ready_||frame!=nextFrame_||measured.frame!=frame||measured.sceneDigest!=input_.sceneDigest||!measured.relocatedOriginsQualified||!measured.originsGeneration||measured.rayCount>input_.rayCount||measured.rays==handles::INVALID_RESOURCE||measured.hits==handles::INVALID_RESOURCE||!measured.raysPerProbe||measured.rayCount%measured.raysPerProbe||!measured.probeCount||measured.updateIndices==handles::INVALID_RESOURCE||measured.origins==handles::INVALID_RESOURCE||lighting==handles::INVALID_RESOURCE)return false;
        if(originGeneration_&&originGeneration_!=measured.originsGeneration)return false;
        auto& s=slots_[graphics::lumen::DDGISlotsForFrame(frame).writeIndex];const u64 n=std::max<u32>(1,measured.rayCount);
        Params params{{measured.rayCount,input_.cardCount,input_.ownerCount,1}};std::copy_n(input_.sky,4,params.sky);
        if(!device_->UpdateBufferData(s.uniform,&params,sizeof(params)))return false;
        DescriptorBufferInfo buffers[]{{measured.rays,0,n*32},{measured.hits,0,n*96},{input_.cards,0,u64(input_.cardCount)*64},{input_.directory,0,u64(input_.ownerCount)*48},{s.radiance,0,n*16},{s.audit,0,n*16},{s.uniform,0,sizeof(Params)}};
        DescriptorImageInfo images[]{{handles::INVALID_SAMPLER,input_.depth,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,lighting,ResourceState::ShaderResource}};WriteDescriptorSet writes[9]{};
        for(u32 i=0;i<9;++i){auto& w=writes[i];w.dstSet=s.set;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=i<6?DescriptorType::StorageBuffer:i==6?DescriptorType::UniformBuffer:DescriptorType::SampledImage;if(i<7)w.bufferInfo=&buffers[i];else w.imageInfo=&images[i-7];}
        device_->UpdateDescriptorSets(9,writes);cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead);
        cmd.BindComputePipeline(pipeline_);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayout_,0,1,&s.set,0,nullptr);if(measured.rayCount)cmd.Dispatch((measured.rayCount+63)/64,1,1);
        cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead);
        s.frame=frame;s.rayCount=measured.rayCount;s.updates=measured.updateIndices;s.raysPerProbe=measured.raysPerProbe;originGeneration_=measured.originsGeneration;++nextFrame_;return true;
    }
    struct Output {ResourceHandle radiance=handles::INVALID_RESOURCE,updates=handles::INVALID_RESOURCE;u32 rayCount=0,raysPerProbe=0,frame=~0u;u64 sceneDigest=0,originsGeneration=0;};
    Output GetRecordedOutput(u32 frame)const{const auto& s=slots_[graphics::lumen::DDGISlotsForFrame(frame).writeIndex];if(!ready_||s.frame!=frame)return {};return {s.radiance,s.updates,s.rayCount,s.raysPerProbe,frame,input_.sceneDigest,originGeneration_};}
    bool Capture(DawnCommandBuffer& cmd,u32 frame){
        if(GetRecordedOutput(frame).radiance==handles::INVALID_RESOURCE||readback_!=handles::INVALID_RESOURCE)return false;
        capturedRays_=GetRecordedOutput(frame).rayCount;
        BufferDesc b{};b.size=std::max<u64>(4,u64(capturedRays_)*32);b.memoryUsage=GPUMemoryUsage::Readback;readback_=device_->CreateBuffer(b);if(readback_==handles::INVALID_RESOURCE)return false;captured_=frame;
        const auto& s=slots_[graphics::lumen::DDGISlotsForFrame(frame).writeIndex];if(capturedRays_)cmd.CopyBuffer(s.radiance,readback_,0,0,u64(capturedRays_)*16);if(capturedRays_)cmd.CopyBuffer(s.audit,readback_,0,u64(capturedRays_)*16,u64(capturedRays_)*16);cmd.MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);return true;
    }
    // Caller must first check End/Submit/WaitForCompletion.
    bool VerifyAndSave(const std::string& prefix){
        if(readback_==handles::INVALID_RESOURCE)return false;const u64 bytes=u64(capturedRays_)*32;const auto* data=static_cast<const u8*>(device_->MapBuffer(readback_,0,std::max<u64>(4,bytes)));if(!data)return false;
        u32 counts[5]{},black=0,invalid=0;float maximum=0;
        for(u32 i=0;i<capturedRays_;++i){float v[4];u32 a[4];std::memcpy(v,data+i*16,16);std::memcpy(a,data+u64(capturedRays_)*16+i*16,16);
            if(a[0]>4){++invalid;continue;}++counts[a[0]];if(a[0]==1&&v[0]==0&&v[1]==0&&v[2]==0)++black;
            for(float x:v)if(!std::isfinite(x))++invalid;for(u32 c=0;c<3;++c){maximum=std::max(maximum,v[c]);if(v[c]<0)++invalid;}
            if((a[0]==1&&a[1]>=input_.cardCount)||(a[0]==2&&v[3]!=-2)||(a[0]==4&&v[3]!=-3)||a[3]>16)++invalid;
            if((a[0]==0||a[0]==3||a[0]==4)&&(v[0]!=0||v[1]!=0||v[2]!=0))++invalid;}
        std::ofstream file(prefix+".bin",std::ios::binary);file.write(reinterpret_cast<const char*>(data),bytes);file.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        std::ofstream meta(prefix+".json");meta << "{\"native_frame\":" << captured_ << ",\"rays\":" << capturedRays_ << ",\"scene_digest\":\"" << input_.sceneDigest << "\",\"origins_generation\":" << originGeneration_ << ",\"unknown\":" << counts[0] << ",\"card\":" << counts[1] << ",\"sky\":" << counts[2] << ",\"unqualified_hit\":" << counts[3] << ",\"bounded_miss\":" << counts[4] << ",\"valid_black\":" << black << ",\"invalid\":" << invalid << ",\"max_radiance\":" << maximum << ",\"completion_checked\":true}\n";meta.close();
        PRIMAL_LOG_STREAM(Info,"DawnMeasuredRadiance") << "frame=" << captured_ << " card=" << counts[1] << " sky=" << counts[2] << " unqualified=" << counts[3] << " black=" << black << " invalid=" << invalid;
        return file.good()&&meta.good()&&!invalid;
    }
    void Shutdown(){if(!device_)return;for(auto& s:slots_){if(s.set!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(s.set);for(auto h:{s.radiance,s.audit,s.uniform})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);s=Slot{};}
        if(pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipeline_);if(pipelineLayout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayout_);if(layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layout_);if(shader_!=handles::INVALID_SHADER)device_->DestroyShader(shader_);
        if(readback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(readback_);device_=nullptr;ready_=false;nextFrame_=0;originGeneration_=0;readback_=handles::INVALID_RESOURCE;pipeline_=handles::INVALID_PIPELINE;pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;shader_=handles::INVALID_SHADER;
    }
private:
    struct Slot {ResourceHandle uniform=handles::INVALID_RESOURCE,radiance=handles::INVALID_RESOURCE,audit=handles::INVALID_RESOURCE,updates=handles::INVALID_RESOURCE;DescriptorSetHandle set=handles::INVALID_DESCRIPTOR_SET;u32 frame=~0u,raysPerProbe=0,rayCount=0;};
    DawnDevice* device_=nullptr;bool ready_=false;u32 nextFrame_=0,captured_=~0u,capturedRays_=0;u64 originGeneration_=0;Inputs input_{};std::array<Slot,3> slots_{};
    ResourceHandle readback_=handles::INVALID_RESOURCE;ShaderHandle shader_=handles::INVALID_SHADER;PipelineHandle pipeline_=handles::INVALID_PIPELINE;PipelineLayoutHandle pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;DescriptorSetLayoutHandle layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
};
}
