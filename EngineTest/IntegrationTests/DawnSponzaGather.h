#pragma once
#include "DawnSponzaProbeUpdate.h"
namespace primal::tests::dawn_sponza {
// Native mode-7 consumer. All input buffers are borrowed and frame-qualified;
// this owner only owns its pipeline, per-frame descriptors/UBOs and two outputs.
class ScreenGather {
public:
    struct Params {
        primal::math::m4x4 inverseVP;
        float camera[4]{},originSpacing[4]{};
        u32 counts[4]{};
        float jitterPolicy[4]{0,0,1,1};
    };static_assert(sizeof(Params)==128);
    struct Inputs {
        MeasuredGeometry::SceneView scene;
        ResourceHandle depth=handles::INVALID_RESOURCE,normal=handles::INVALID_RESOURCE,orm=handles::INVALID_RESOURCE;
        u32 width=0,height=0;
        // Independent gather lattice; zero pair preserves the half-source default.
        u32 targetWidth=0,targetHeight=0;
    };
    ScreenGather()=default;ScreenGather(const ScreenGather&)=delete;ScreenGather& operator=(const ScreenGather&)=delete;
    ~ScreenGather(){Shutdown();}
    static std::string Source(){
        auto read=[](const char* p){std::ifstream f(p);return std::string((std::istreambuf_iterator<char>(f)),{});};
        auto common=read("Engine/Graphics/Dawn/shaders/Lumen/GeometryFirstHit.wgsl");
        const auto end=common.find("fn geometryClassifyMeasured(");if(end==std::string::npos)return {};
        common.resize(end);
        for(const char* name:{"@group(0) @binding(4)","@group(0) @binding(5)"}){
            auto at=common.find(name);if(at==std::string::npos)return {};auto nl=common.find('\n',at);if(nl==std::string::npos)return {};common.erase(at,nl-at+1);
        }
        auto connection=read("Engine/Graphics/Dawn/shaders/Lumen/GeometryConnectionQuery.wgsl");
        auto kernel=read("Engine/Graphics/Dawn/shaders/Lumen/DDGIMeasuredGather.wgsl");
        return connection.empty()||kernel.empty()?std::string{}:common+connection+kernel;
    }
    bool Initialize(DawnDevice& device,const Inputs& in){
        if(device_||!in.scene.digest||!in.width||!in.height||in.width%2||in.height%2||bool(in.targetWidth)!=bool(in.targetHeight))return false;
        for(u32 i=0;i<4;++i)if(!in.scene.counts[i]||in.scene.buffers[i]==handles::INVALID_RESOURCE)return false;
        for(auto h:{in.depth,in.normal,in.orm})if(h==handles::INVALID_RESOURCE)return false;
        WGPULimits limits=WGPU_LIMITS_INIT;if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success||limits.maxStorageBuffersPerShaderStage<8||in.width>limits.maxTextureDimension2D||in.height>limits.maxTextureDimension2D||in.targetWidth>limits.maxTextureDimension2D||in.targetHeight>limits.maxTextureDimension2D)return false;
        device_=&device;inputs_=in;
        inputs_.targetWidth=in.targetWidth?in.targetWidth:in.width/2;
        inputs_.targetHeight=in.targetHeight?in.targetHeight:in.height/2;
        std::vector<DescriptorSetLayoutBinding> bindings;
        for(u32 i:{0u,1u,2u,3u,6u,7u,8u,9u,10u,11u,12u,13u,14u,15u,16u}){
            DescriptorSetLayoutBinding b{};b.binding=i;b.descriptorCount=1;b.stageFlags=ShaderStage::Compute;
            b.descriptorType=i==6||i==7?DescriptorType::UniformBuffer:i<12?DescriptorType::StorageBuffer:i==12?DescriptorType::SampledDepthImage:i<15?DescriptorType::SampledImage:DescriptorType::StorageImage;b.readonly=i<12&&i!=6&&i!=7;bindings.push_back(b);
        }
        layout_=device.CreateDescriptorSetLayout({u32(bindings.size()),bindings.data()});if(layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
        PipelineLayoutDesc pl{};pl.setLayouts=&layout_;pl.setLayoutCount=1;pipelineLayout_=device.CreatePipelineLayout(pl);if(pipelineLayout_==handles::INVALID_PIPELINE_LAYOUT)return false;
        auto source=Source();if(source.empty())return false;shader_=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,"measured_gather");if(shader_==handles::INVALID_SHADER)return false;
        ComputePipelineDesc cp{};cp.computeShader=shader_;cp.layout=pipelineLayout_;pipeline_=device.CreateComputePipeline(cp);if(pipeline_==handles::INVALID_PIPELINE)return false;
        for(auto& h:outputs_){TextureDesc t{};t.type=TextureType::Texture2D;t.size={inputs_.targetWidth,inputs_.targetHeight,1};t.format=DataFormat::RGBA16_Float;t.usage=TextureUsage::ShaderResource|TextureUsage::UnorderedAccess|TextureUsage::CopySource;h=device.CreateTexture(t);if(h==handles::INVALID_RESOURCE)return false;}
        for(auto& s:slots_){s.set=device.CreateDescriptorSet({layout_});BufferDesc b{};b.type=BufferType::Constant;b.memoryUsage=GPUMemoryUsage::Static;b.size=sizeof(Params);s.params=device.CreateBuffer(b);b.size=32;s.geometry=device.CreateBuffer(b);if(s.set==handles::INVALID_DESCRIPTOR_SET||s.params==handles::INVALID_RESOURCE||s.geometry==handles::INVALID_RESOURCE)return false;}
        ready_=true;return true;
    }
    bool Record(DawnCommandBuffer& cmd,u32 frame,const Params& params,const ProbeUpdate::Output& probes,ResourceHandle origins,u64 generation){
        const u64 count=u64(params.counts[0])*params.counts[1]*params.counts[2];
        if(!ready_||frame!=nextFrame_||!count||count!=probes.probes||probes.frame!=frame||probes.sceneDigest!=inputs_.scene.digest||!generation||probes.originsGeneration!=generation||!(params.originSpacing[3]>0)||!std::isfinite(params.originSpacing[3]))return false;
        for(float v:params.camera)if(!std::isfinite(v))return false;
        for(float v:params.originSpacing)if(!std::isfinite(v))return false;
        for(float v:params.jitterPolicy)if(!std::isfinite(v))return false;
        for(u32 i=0;i<3;++i)if(!params.counts[i]||params.counts[i]>65535)return false;
        for(auto h:{probes.sh,probes.depth,probes.stats,origins})if(h==handles::INVALID_RESOURCE)return false;
        auto& s=slots_[frame%3];u32 geometry[8]{};std::copy(inputs_.scene.counts.begin(),inputs_.scene.counts.end(),geometry);geometry[5]=1;
        if(!device_->UpdateBufferData(s.params,&params,sizeof(params))||!device_->UpdateBufferData(s.geometry,geometry,sizeof(geometry)))return false;
        ResourceHandle handles[]{inputs_.scene.buffers[0],inputs_.scene.buffers[1],inputs_.scene.buffers[2],inputs_.scene.buffers[3],s.geometry,s.params,probes.sh,probes.depth,origins,probes.stats};
        const u32 indices[]{0,1,2,3,6,7,8,9,10,11};const u64 bytes[]{u64(geometry[0])*48,u64(geometry[1])*48,u64(geometry[2])*48,u64(geometry[3])*64,32,128,count*144,count*512,count*16,count*32};
        DescriptorBufferInfo bi[10]{};WriteDescriptorSet writes[15]{};
        for(u32 i=0;i<10;++i){bi[i]={handles[i],0,bytes[i]};auto& w=writes[i];w.dstSet=s.set;w.dstBinding=indices[i];w.descriptorCount=1;w.descriptorType=i==4||i==5?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer;w.bufferInfo=&bi[i];}
        DescriptorImageInfo images[5]{};const ResourceHandle tex[]{inputs_.depth,inputs_.normal,inputs_.orm,outputs_[0],outputs_[1]};
        for(u32 i=0;i<5;++i){images[i]={handles::INVALID_SAMPLER,tex[i],i>=3?ResourceState::UnorderedAccess:ResourceState::ShaderResource};auto& w=writes[10+i];w.dstSet=s.set;w.dstBinding=12+i;w.descriptorCount=1;w.descriptorType=i==0?DescriptorType::SampledDepthImage:i<3?DescriptorType::SampledImage:DescriptorType::StorageImage;w.imageInfo=&images[i];}
        device_->UpdateDescriptorSets(15,writes);cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::MemoryWrite,AccessFlag::ShaderRead);
        cmd.BindComputePipeline(pipeline_);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayout_,0,1,&s.set,0,nullptr);cmd.Dispatch((inputs_.targetWidth+7)/8,(inputs_.targetHeight+7)/8,1);
        cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::AllCommands,AccessFlag::ShaderWrite,AccessFlag::MemoryRead);lastParams_=params;++nextFrame_;return true;
    }
    ResourceHandle Diffuse()const{return ready_?outputs_[0]:handles::INVALID_RESOURCE;}
    ResourceHandle Specular()const{return ready_?outputs_[1]:handles::INVALID_RESOURCE;}
    bool Capture(DawnCommandBuffer& cmd){
        if(!ready_||!nextFrame_||readback_!=handles::INVALID_RESOURCE)return false;
        const u32 w=inputs_.targetWidth,h=inputs_.targetHeight;pitch_=(w*8+255)&~255u;BufferDesc b{};b.size=u64(pitch_)*h*2;b.memoryUsage=GPUMemoryUsage::Readback;readback_=device_->CreateBuffer(b);if(readback_==handles::INVALID_RESOURCE)return false;
        for(u32 i=0;i<2;++i){BufferTextureCopyRegion r{};r.bufferOffset=u64(pitch_)*h*i;r.bufferRowLength=pitch_/8;r.bufferImageHeight=h;r.imageExtent={w,h,1};cmd.CopyTextureToBuffer(outputs_[i],readback_,&r,1);}return true;
    }
    bool Save(const std::string& prefix){
        if(readback_==handles::INVALID_RESOURCE)return false;const u32 w=inputs_.targetWidth,h=inputs_.targetHeight;const auto* raw=static_cast<const u8*>(device_->MapBuffer(readback_,0,u64(pitch_)*h*2));if(!raw)return false;
        std::ofstream out(prefix+".bin",std::ios::binary);u32 invalid=0,valid=0;float peak=0;
        for(u32 image=0;image<2;++image)for(u32 y=0;y<h;++y){const auto* row=raw+u64(pitch_)*(h*image+y);out.write(reinterpret_cast<const char*>(row),w*8);const auto* values=reinterpret_cast<const _Float16*>(row);for(u32 x=0;x<w;++x){valid+=image==0&&float(values[x*4+3])>0;for(u32 c=0;c<4;++c){float value=values[x*4+c];if(!std::isfinite(value))++invalid;peak=std::max(peak,std::abs(value));}}}
        out.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        // Preserve the exact host record uploaded for this captured dispatch.
        std::ofstream params(prefix+".params.bin",std::ios::binary);params.write(reinterpret_cast<const char*>(&lastParams_),sizeof(lastParams_));params.close();
        std::ofstream meta(prefix+".json");meta<<"{\"frame\":"<<nextFrame_-1<<",\"width\":"<<w<<",\"height\":"<<h<<",\"valid\":"<<valid<<",\"invalid\":"<<invalid<<",\"peak\":"<<peak<<",\"completion_checked_by_caller\":true,\"layout\":\"diffuse RGBA16F, specular RGBA16F; tight rows\"}\n";meta.close();return out.good()&&params.good()&&meta.good()&&!invalid;
    }
    void Shutdown(){if(!device_)return;for(auto& s:slots_){if(s.set!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(s.set);for(auto h:{s.params,s.geometry})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);s=Slot{};}for(auto h:outputs_)if(h!=handles::INVALID_RESOURCE)device_->DestroyTexture(h);if(readback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(readback_);if(pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipeline_);if(shader_!=handles::INVALID_SHADER)device_->DestroyShader(shader_);if(pipelineLayout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayout_);if(layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layout_);device_=nullptr;ready_=false;nextFrame_=0;outputs_.fill(handles::INVALID_RESOURCE);readback_=handles::INVALID_RESOURCE;pipeline_=handles::INVALID_PIPELINE;shader_=handles::INVALID_SHADER;pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;}
private:
    struct Slot {ResourceHandle params=handles::INVALID_RESOURCE,geometry=handles::INVALID_RESOURCE;DescriptorSetHandle set=handles::INVALID_DESCRIPTOR_SET;};
    DawnDevice* device_=nullptr;Inputs inputs_{};Params lastParams_{};bool ready_=false;u32 nextFrame_=0,pitch_=0;
    std::array<Slot,3> slots_{};std::array<ResourceHandle,2> outputs_{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};ResourceHandle readback_=handles::INVALID_RESOURCE;
    DescriptorSetLayoutHandle layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;PipelineLayoutHandle pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;ShaderHandle shader_=handles::INVALID_SHADER;PipelineHandle pipeline_=handles::INVALID_PIPELINE;
};
}
