#pragma once
#include "DawnSponzaScreenProbeSDF.h"
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnBuffer.h"
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnTexture.h"
#include <filesystem>
namespace primal::tests::dawn_sponza {
// Static-scene measured profile. Borrowed input resources outlive this owner;
// producer frame and origin generation are checked before either dispatch.
class ScreenProbeFinalize {
public:
    struct Inputs {
        ResourceHandle cards=handles::INVALID_RESOURCE,grid=handles::INVALID_RESOURCE,gridCards=handles::INVALID_RESOURCE;
        ResourceHandle depth=handles::INVALID_RESOURCE,albedo=handles::INVALID_RESOURCE;
        u32 probes=0,rays=0,cardsCount=0,worldProbes=0;u64 sceneDigest=0;
    };
    struct Frame {
        ResourceHandle color=handles::INVALID_RESOURCE,lighting=handles::INVALID_RESOURCE,origins=handles::INVALID_RESOURCE;
        u32 colorFrame=~0u,lightingFrame=~0u;u64 sceneDigest=0,originsGeneration=0;
        ProbeUpdate::History history{};
    };
    struct Output {ResourceHandle radiance=handles::INVALID_RESOURCE;u32 frame=~0u,probes=0,rays=0;ScreenProbePlacement::Output source{};};
    ScreenProbeFinalize()=default;ScreenProbeFinalize(const ScreenProbeFinalize&)=delete;ScreenProbeFinalize& operator=(const ScreenProbeFinalize&)=delete;~ScreenProbeFinalize(){Shutdown();}
    bool Initialize(DawnDevice& device,const Inputs& in){
        if(device_||!in.probes||in.rays<8||in.rays>64||!in.cardsCount||!in.worldProbes||!in.sceneDigest)return false;
        for(auto h:{in.cards,in.grid,in.gridCards,in.depth,in.albedo})if(h==handles::INVALID_RESOURCE)return false;
        WGPULimits limits=WGPU_LIMITS_INIT;if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success||limits.maxStorageBuffersPerShaderStage<8||u64(in.probes)*in.rays*32>limits.maxStorageBufferBindingSize||(u64(in.probes)*in.rays+63)/64>limits.maxComputeWorkgroupsPerDimension)return false;
        device_=&device;input_=in;
        if(!Fits(in.cards,u64(in.cardsCount)*64)||!Fits(in.grid,68)||!Fits(in.gridCards,4))return false;
        gridBytes_=device.GetBuffer(in.grid)->GetBufferDesc().size;indicesBytes_=device.GetBuffer(in.gridCards)->GetBufferDesc().size;
        auto read=[](const std::string& path){std::ifstream f(path);return std::string((std::istreambuf_iterator<char>(f)),{});};const auto common=read("Engine/Graphics/Dawn/shaders/Lumen/ScreenProbeMeasuredCommon.wgsl");if(common.empty())return false;
        for(u32 stage=0;stage<2;++stage){const u32 count=stage?9:13;std::vector<DescriptorSetLayoutBinding> bindings(count);
            for(u32 i=0;i<count;++i){auto& b=bindings[i];b.binding=i;b.descriptorCount=1;b.stageFlags=ShaderStage::Compute;b.descriptorType=i==0?DescriptorType::UniformBuffer:i<9?DescriptorType::StorageBuffer:DescriptorType::SampledImage;b.readonly=i>0&&i<9&&i!=8&&i!=(stage?4:7);b.unfilterableFloat=i>=9;}
            layouts_[stage]=device.CreateDescriptorSetLayout({count,bindings.data()});if(layouts_[stage]==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&layouts_[stage];pipelineLayouts_[stage]=device.CreatePipelineLayout(pl);if(pipelineLayouts_[stage]==handles::INVALID_PIPELINE_LAYOUT)return false;
            const auto kernel=read(std::string("Engine/Graphics/Dawn/shaders/Lumen/ScreenProbeMeasuredFinalize")+(stage?"Fallback":"Surface")+".wgsl");if(kernel.empty())return false;const auto source=common+kernel;shaders_[stage]=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,stage?"measured_finalize_fallback":"measured_finalize_surface");if(shaders_[stage]==handles::INVALID_SHADER)return false;ComputePipelineDesc cp{};cp.layout=pipelineLayouts_[stage];cp.computeShader=shaders_[stage];pipelines_[stage]=device.CreateComputePipeline(cp);if(pipelines_[stage]==handles::INVALID_PIPELINE)return false;
        }
        for(auto& s:slots_){BufferDesc b{};b.type=BufferType::Constant;b.memoryUsage=GPUMemoryUsage::Static;b.size=sizeof(ScreenProbePlacement::Params);s.params=device.CreateBuffer(b);b.type=BufferType::Raw;b.size=Bytes();s.radiance=device.CreateBuffer(b);s.audit=device.CreateBuffer(b);for(u32 i=0;i<2;++i)s.sets[i]=device.CreateDescriptorSet({layouts_[i]});if(s.params==handles::INVALID_RESOURCE||s.radiance==handles::INVALID_RESOURCE||s.audit==handles::INVALID_RESOURCE)return false;for(auto h:s.sets)if(h==handles::INVALID_DESCRIPTOR_SET)return false;}
        ready_=true;return true;
    }
    bool Record(DawnCommandBuffer& cmd,u32 frame,const ScreenProbePlacement::Output& rays,const ScreenProbeSDF::Output& hits,const Frame& sources){
        if(!ready_||frame!=nextFrame_||rays.frame!=frame||hits.frame!=frame||rays.probes!=input_.probes||hits.probes!=input_.probes||rays.raysPerProbe!=input_.rays||hits.rays!=input_.rays||sources.colorFrame!=frame||sources.lightingFrame!=frame||sources.sceneDigest!=input_.sceneDigest||!sources.originsGeneration)return false;
        if(hits.source.rays!=rays.rays||hits.source.positions!=rays.positions||hits.source.normals!=rays.normals)return false;
        const auto& history=sources.history.state;
        if(sources.history.reset!=(frame==0)||history.frame!=(frame?frame-1:~0u)||history.sceneDigest!=input_.sceneDigest||history.originsGeneration!=sources.originsGeneration||history.probes!=input_.worldProbes)return false;
        if(generation_&&generation_!=sources.originsGeneration)return false;
        for(auto h:{sources.color,sources.lighting,sources.origins,rays.positions,rays.normals,rays.rays,hits.hits,history.sh,history.depth})if(h==handles::INVALID_RESOURCE)return false;
        const auto& p=hits.params;
        if(p.tier[0]!=0||p.tier[3]!=0||p.surfaceCache[3]!=0||p.tierCounts[3]<=.5f||p.surfaceCache[1]!=float(input_.cardsCount)||p.surfaceCache[2]<=.5f||p.hint[3]!=2||p.ddgi[1][3]<=.5f)return false;
        u64 worldCount=1;for(u32 a=0;a<3;++a){float n=p.ddgi[1][a];if(!std::isfinite(n)||n<1||n>65535||n!=std::floor(n))return false;worldCount*=u32(n);}
        if(worldCount!=input_.worldProbes||!std::isfinite(p.ddgi[0][3])||p.ddgi[0][3]<=0||!std::isfinite(p.tierCounts[1])||p.tierCounts[1]<0||!std::isfinite(p.diagnostics[1]))return false;
        for(u32 a=0;a<3;++a)if(!std::isfinite(p.ddgi[0][a]))return false;
        const u64 n=input_.probes,w=input_.worldProbes;
        if(!Fits(history.sh,w*144)||!Fits(history.depth,w*512)||!Fits(sources.origins,w*16)||!Fits(rays.positions,n*16)||!Fits(rays.normals,n*16)||!Fits(rays.rays,n*input_.rays*32)||!Fits(hits.hits,Bytes()))return false;
        auto& s=slots_[frame%3];s.cpu=p;if(!device_->UpdateBufferData(s.params,&p,sizeof(p)))return false;
        const DescriptorBufferInfo bi[2][9]{
            {{s.params,0,sizeof(p)},{rays.positions,0,n*16},{rays.rays,0,n*input_.rays*32},{hits.hits,0,Bytes()},{input_.cards,0,u64(input_.cardsCount)*64},{input_.grid,0,gridBytes_},{input_.gridCards,0,indicesBytes_},{s.radiance,0,Bytes()},{s.audit,0,Bytes()}},
            {{s.params,0,sizeof(p)},{rays.positions,0,n*16},{rays.normals,0,n*16},{rays.rays,0,n*input_.rays*32},{s.radiance,0,Bytes()},{history.sh,0,w*144},{history.depth,0,w*512},{sources.origins,0,w*16},{s.audit,0,Bytes()}}};
        const DescriptorImageInfo images[]{{handles::INVALID_SAMPLER,sources.color,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,input_.depth,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,sources.lighting,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,input_.albedo,ResourceState::ShaderResource}};
        for(u32 stage=0;stage<2;++stage){const u32 count=stage?9:13;WriteDescriptorSet writes[13]{};for(u32 i=0;i<count;++i){auto& v=writes[i];v.dstSet=s.sets[stage];v.dstBinding=i;v.descriptorCount=1;v.descriptorType=i==0?DescriptorType::UniformBuffer:i<9?DescriptorType::StorageBuffer:DescriptorType::SampledImage;if(i<9)v.bufferInfo=&bi[stage][i];else v.imageInfo=&images[i-9];}device_->UpdateDescriptorSets(count,writes);
            cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::MemoryWrite,AccessFlag::ShaderRead);cmd.BindComputePipeline(pipelines_[stage]);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayouts_[stage],0,1,&s.sets[stage],0,nullptr);cmd.Dispatch((input_.probes*input_.rays+63)/64,1,1);
        }
        s.inputs={bi[0][1],bi[1][2],bi[0][2],bi[0][3],bi[0][4],bi[0][5],bi[0][6],bi[1][5],bi[1][6],bi[1][7]};
        for(u32 i=0;i<4;++i)s.images[i]=images[i].imageView;
        cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::AllCommands,AccessFlag::ShaderWrite,AccessFlag::MemoryRead);s.frame=frame;s.source=rays;generation_=sources.originsGeneration;++nextFrame_;return true;
    }
    Output GetOutput(u32 frame)const{if(!ready_||!nextFrame_||frame!=nextFrame_-1)return {};return {slots_[frame%3].radiance,frame,input_.probes,input_.rays,slots_[frame%3].source};}
    bool Capture(DawnCommandBuffer& cmd,u32 frame){
        if(GetOutput(frame).radiance==handles::INVALID_RESOURCE||readback_!=handles::INVALID_RESOURCE)return false;const auto& s=slots_[frame%3];captured_=frame;capturedParams_=s.cpu;BufferDesc b{};b.size=Bytes()*2;b.memoryUsage=GPUMemoryUsage::Readback;readback_=device_->CreateBuffer(b);if(readback_==handles::INVALID_RESOURCE)return false;cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);cmd.CopyBuffer(s.radiance,readback_,0,0,Bytes());cmd.CopyBuffer(s.audit,readback_,0,Bytes(),Bytes());cmd.MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);return true;
    }
    bool CaptureInputs(DawnCommandBuffer& cmd,u32 frame){
        if(GetOutput(frame).radiance==handles::INVALID_RESOURCE||!witnesses_.empty())return false;
        const auto& s=slots_[frame%3];const char* names[]{"positions","normals","rays","hits","cards","grid","grid-cards","sh","ddgi-depth","origins","previous","card-depth","card-light","card-albedo"};
        for(u32 i=0;i<14;++i){Witness w{};w.name=names[i];if(i<10)w.bytes=s.inputs[i].range;else{const u32 strides[]{8,4,8,4};w.stride=strides[i-10];const auto* texture=device_->GetTexture(s.images[i-10]);if(!texture)return false;w.width=texture->GetTextureDesc().size.x;w.height=texture->GetTextureDesc().size.y;if(!w.width||!w.height)return false;w.pitch=(w.width*w.stride+255)&~255u;w.bytes=u64(w.pitch)*w.height;}
            BufferDesc b{};b.size=w.bytes;b.memoryUsage=GPUMemoryUsage::Readback;w.buffer=device_->CreateBuffer(b);if(w.buffer==handles::INVALID_RESOURCE)return false;witnesses_.push_back(w);
            if(i<10)cmd.CopyBuffer(s.inputs[i].buffer,w.buffer,s.inputs[i].offset,0,w.bytes);else{BufferTextureCopyRegion region{};region.bufferRowLength=w.pitch/w.stride;region.bufferImageHeight=w.height;region.imageExtent={w.width,w.height,1};cmd.CopyTextureToBuffer(s.images[i-10],w.buffer,&region,1);}
        }
        cmd.MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);return true;
    }
    // Caller must check GPU completion first.
    bool Save(const std::string& prefix){
        if(readback_==handles::INVALID_RESOURCE)return false;const auto* raw=static_cast<const u8*>(device_->MapBuffer(readback_,0,Bytes()*2));if(!raw)return false;u32 counts[6]{},invalid=0;float peak=0;
        for(u64 i=0;i<u64(input_.probes)*input_.rays;++i){float v[4];u32 a[4];std::memcpy(v,raw+i*16,16);std::memcpy(a,raw+Bytes()+i*16,16);if(a[0]>=6||a[0]==3)++invalid;else ++counts[a[0]];for(float x:v)if(!std::isfinite(x))++invalid;for(u32 c=0;c<3;++c)peak=std::max(peak,std::abs(v[c]));if((a[0]==1||a[0]==2)&&v[3]<0)++invalid;if(a[0]==4&&v[3]>0)++invalid;}
        std::ofstream out(prefix+".bin",std::ios::binary);out.write(reinterpret_cast<const char*>(&capturedParams_),sizeof(capturedParams_));out.write(reinterpret_cast<const char*>(raw),Bytes()*2);out.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        std::ofstream meta(prefix+".json");meta<<"{\"frame\":"<<captured_<<",\"probes\":"<<input_.probes<<",\"rays_per_probe\":"<<input_.rays<<",\"invalid\":"<<invalid<<",\"peak\":"<<peak<<",\"origins_generation\":"<<generation_<<",\"sources\":[";for(u32 i=0;i<6;++i){if(i)meta<<",";meta<<counts[i];}meta<<"],\"completion_checked\":true,\"color_source\":\"current pre-GI direct\",\"sc_source\":\"current full atlas\",\"ddgi_source\":\"previous completed slot; reset initialized unknown at frame0\",\"layout\":\"Params544,radiance vec4,audit uvec4\"}\n";meta.close();PRIMAL_LOG_STREAM(Info,"DawnSPGIFinalize")<<"frame="<<captured_<<" hzb="<<counts[1]<<" sc="<<counts[2]<<" ddgi="<<counts[4]<<" unknown="<<counts[5]<<" invalid="<<invalid;return out.good()&&meta.good()&&!invalid&&SaveInputs(prefix+"-inputs");
    }
    void Shutdown(){if(!device_)return;for(auto& w:witnesses_)if(w.buffer!=handles::INVALID_RESOURCE)device_->DestroyBuffer(w.buffer);witnesses_.clear();for(auto& s:slots_){for(auto h:s.sets)if(h!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(h);for(auto h:{s.params,s.radiance,s.audit})if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);s=Slot{};}for(u32 i=0;i<2;++i){if(pipelines_[i]!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipelines_[i]);if(shaders_[i]!=handles::INVALID_SHADER)device_->DestroyShader(shaders_[i]);if(pipelineLayouts_[i]!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayouts_[i]);if(layouts_[i]!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layouts_[i]);}if(readback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(readback_);device_=nullptr;ready_=false;nextFrame_=0;generation_=0;readback_=handles::INVALID_RESOURCE;pipelines_.fill(handles::INVALID_PIPELINE);shaders_.fill(handles::INVALID_SHADER);pipelineLayouts_.fill(handles::INVALID_PIPELINE_LAYOUT);layouts_.fill(handles::INVALID_DESCRIPTOR_SET_LAYOUT);}
private:
    struct Witness {std::string name;ResourceHandle buffer=handles::INVALID_RESOURCE;u64 bytes=0;u32 width=0,height=0,stride=0,pitch=0;};
    bool SaveInputs(const std::string& directory){if(witnesses_.empty())return true;std::filesystem::create_directories(directory);std::ofstream params(directory+"/params.bin",std::ios::binary);params.write(reinterpret_cast<const char*>(&capturedParams_),sizeof(capturedParams_));params.close();if(!params.good())return false;std::ofstream metadata(directory+"/textures.json");metadata<<"{";bool comma=false;
        for(auto& w:witnesses_){const auto* p=static_cast<const char*>(device_->MapBuffer(w.buffer,0,w.bytes));if(!p)return false;std::ofstream f(directory+"/"+w.name+".bin",std::ios::binary);if(w.width){for(u32 y=0;y<w.height;++y)f.write(p+u64(y)*w.pitch,u64(w.width)*w.stride);if(comma)metadata<<",";metadata<<"\""<<w.name<<"\":["<<w.width<<","<<w.height<<","<<w.stride<<"]";comma=true;}else f.write(p,w.bytes);f.close();device_->UnmapBuffer(w.buffer);device_->DestroyBuffer(w.buffer);w.buffer=handles::INVALID_RESOURCE;if(!f.good())return false;}
        metadata<<"}\n";metadata.close();witnesses_.clear();return metadata.good();
    }
    bool Fits(ResourceHandle h,u64 bytes)const{const auto* b=device_->GetBuffer(h);return b&&b->GetBufferDesc().size>=bytes;}
    u64 Bytes()const{return u64(input_.probes)*input_.rays*16;}
    struct Slot {ResourceHandle params=handles::INVALID_RESOURCE,radiance=handles::INVALID_RESOURCE,audit=handles::INVALID_RESOURCE;std::array<DescriptorSetHandle,2> sets{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};u32 frame=~0u;ScreenProbePlacement::Params cpu{};ScreenProbePlacement::Output source{};std::array<DescriptorBufferInfo,10> inputs{};std::array<ResourceHandle,4> images{};};
    std::vector<Witness> witnesses_;
    DawnDevice* device_=nullptr;Inputs input_{};bool ready_=false;u32 nextFrame_=0,captured_=~0u;u64 generation_=0,gridBytes_=0,indicesBytes_=0;std::array<Slot,3> slots_{};ResourceHandle readback_=handles::INVALID_RESOURCE;ScreenProbePlacement::Params capturedParams_{};
    std::array<DescriptorSetLayoutHandle,2> layouts_{handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT};std::array<PipelineLayoutHandle,2> pipelineLayouts_{handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT};std::array<PipelineHandle,2> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};std::array<ShaderHandle,2> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER};
};
}
