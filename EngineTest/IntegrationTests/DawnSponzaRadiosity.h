#pragma once
#include "DawnSponzaHalf.h"
#include "DawnSponzaSurfaceLighting.h"
#include "Engine/Graphics/Lumen/SurfaceCache/CardTileOwners.h"

namespace primal::tests::dawn_sponza {
// Frozen default static/offline profile: tile=8, rays=32, stride=2,
// N-2 lighting, N-1 tile EMA. LightEval consumes the write slot BEFORE this
// frame overwrites it (N-3 tile output), matching SurfaceCachePass ordering.
class TileRadiosity {
public:
    struct alignas(16) Params {u32 counts[4]{2048,8,32,0},schedule[4]{0,2,0,0};float policy[4]{.25f,50,.9f,.5f},origin[4]{},extent[4]{0,0,0,2};};
    static_assert(sizeof(Params)==80);
    struct Inputs {ResourceHandle cards=handles::INVALID_RESOURCE,lookups=handles::INVALID_RESOURCE,sdf=handles::INVALID_RESOURCE;std::array<ResourceHandle,3> material{};std::span<const graphics::lumen::SurfaceCacheCard> cardData;u32 lookupCount=0,warmup=90;Params params{};};
    TileRadiosity()=default;TileRadiosity(const TileRadiosity&)=delete;TileRadiosity& operator=(const TileRadiosity&)=delete;~TileRadiosity(){Shutdown();}
    bool Initialize(DawnDevice& device,const Inputs& input){
        const auto& p=input.params;const u32 size=p.counts[0],tile=p.counts[1];
        if(device_||!size||!tile||size%tile||size%32||p.counts[2]!=32||!p.schedule[1]||p.schedule[1]>64||input.cardData.empty()||input.cardData.size()>2048||!input.lookupCount||input.cards==handles::INVALID_RESOURCE||input.lookups==handles::INVALID_RESOURCE||input.sdf==handles::INVALID_RESOURCE)return false;
        for(auto h:input.material)if(h==handles::INVALID_RESOURCE)return false;
        for(u32 a=0;a<3;++a)if(!std::isfinite(p.origin[a])||!std::isfinite(p.extent[a])||p.extent[a]<=0)return false;
        if(!std::isfinite(p.origin[3])||p.origin[3]<1||!std::isfinite(p.extent[3])||p.extent[3]<0)return false;
        std::vector<u32> owners;if(!graphics::lumen::BuildCardTileOwners(input.cardData,size,tile,owners))return false;
        for(const auto& c:input.cardData)if(c._pad0!=0||(c.axis_direction&255u)>2u)return false;
        WGPULimits limits=WGPU_LIMITS_INIT;const u64 hitBytes=u64(owners.size())*32*32;
        if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success||size>limits.maxTextureDimension2D||hitBytes>limits.maxStorageBufferBindingSize)return false;
        device_=&device;params_=p;params_.counts[3]=u32(input.cardData.size());params_.schedule[2]=input.lookupCount;warmup_=input.warmup;width_=size/tile;tileCount_=width_*width_;
        auto buffer=[&](u64 bytes,const void* data,BufferType type){BufferDesc d{};d.size=bytes;d.type=type;d.memoryUsage=GPUMemoryUsage::Static;auto h=device.CreateBuffer(d);buffers_.push_back(h);if(h!=handles::INVALID_RESOURCE&&data&&!device.UpdateBufferData(h,data,bytes))return handles::INVALID_RESOURCE;return h;};
        owners_=buffer(owners.size()*4,owners.data(),BufferType::Raw);hits_=buffer(hitBytes,nullptr,BufferType::Raw);std::vector<u32> zero((tileCount_+31)/32);valid_=buffer(zero.size()*4,zero.data(),BufferType::Raw);
        auto texture=[&](u32 n){TextureDesc d{};d.type=TextureType::Texture2D;d.size={n,n,1};d.format=DataFormat::RGBA16_Float;d.usage=TextureUsage::ShaderResource|TextureUsage::UnorderedAccess|TextureUsage::CopySource;auto h=device.CreateTexture(d);textures_.push_back(h);return h;};
        for(u32 slot=0;slot<3;++slot){grid_[slot]=texture(width_);output_[slot]=texture(size);uniform_[slot]=buffer(sizeof(params_),&params_,BufferType::Constant);}aux_=texture(width_);
        for(auto h:buffers_)if(h==handles::INVALID_RESOURCE)return false;for(auto h:textures_)if(h==handles::INVALID_RESOURCE)return false;
        const std::string base="Engine/Graphics/Dawn/shaders/Lumen/SurfaceCacheRadiosity";
        auto read=[](const std::string& path){std::ifstream f(path);return std::string((std::istreambuf_iterator<char>(f)),{});};const auto common=read(base+"Common.wgsl");if(common.empty())return false;
        const char* names[]{"Trace","Resolve","Reconstruct","Clear"};const char* entries[]{"tile_trace","tile_resolve","tile_reconstruct","tile_clear"};
        for(u32 stage=0;stage<4;++stage){
            const u32 count=stage==1?11:stage==3?3:7;std::vector<DescriptorSetLayoutBinding> bindings(count);
            for(u32 b=0;b<count;++b){auto& x=bindings[b];x.binding=b;x.descriptorCount=1;x.stageFlags=ShaderStage::Compute;x.descriptorType=DescriptorType::SampledImage;}
            if(stage!=3)bindings[0].descriptorType=DescriptorType::UniformBuffer;
            if(stage==0){for(u32 b=1;b<=3;++b){bindings[b].descriptorType=DescriptorType::StorageBuffer;bindings[b].readonly=b!=3;}bindings[4].unfilterableFloat=true;bindings[6].is3D=true;}
            if(stage==1){for(u32 b=1;b<=4;++b){bindings[b].descriptorType=DescriptorType::StorageBuffer;bindings[b].readonly=b!=4;}bindings[5].unfilterableFloat=true;}
            if(stage==2)bindings[2].unfilterableFloat=true;
            for(u32 b=0;b<count;++b)if((stage==1&&b>=9)||(stage==2&&b==6)||stage==3){bindings[b].descriptorType=DescriptorType::StorageImage;bindings[b].format=DataFormat::RGBA16_Float;}
            layouts_[stage]=device.CreateDescriptorSetLayout({count,bindings.data()});if(layouts_[stage]==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&layouts_[stage];pipelineLayouts_[stage]=device.CreatePipelineLayout(pl);if(pipelineLayouts_[stage]==handles::INVALID_PIPELINE_LAYOUT)return false;
            auto code=read(base+names[stage]+".wgsl");if(code.empty())return false;code=common+code;shaders_[stage]=device.CreateShader(code.data(),code.size(),ShaderStage::Compute,entries[stage]);if(shaders_[stage]==handles::INVALID_SHADER)return false;
            ComputePipelineDesc cp{};cp.computeShader=shaders_[stage];cp.layout=pipelineLayouts_[stage];pipelines_[stage]=device.CreateComputePipeline(cp);if(pipelines_[stage]==handles::INVALID_PIPELINE)return false;
            for(u32 slot=0;slot<3;++slot){auto& set=sets_[stage][slot];set=device.CreateDescriptorSet({layouts_[stage]});if(set==handles::INVALID_DESCRIPTOR_SET)return false;
                std::vector<DescriptorBufferInfo> bi(count);std::vector<DescriptorImageInfo> ii(count);std::vector<WriteDescriptorSet> w(count);
                bi[0]={uniform_[slot],0,sizeof(params_)};const auto previous=graphics::lumen::DDGISlotsForFrame(slot).previousIndex;
                auto im=[&](u32 b,ResourceHandle h){ii[b]={handles::INVALID_SAMPLER,h,ResourceState::ShaderResource};};
                if(stage==0){bi[1]={input.cards,0,input.cardData.size()*64};bi[2]={owners_,0,owners.size()*4};bi[3]={hits_,0,hitBytes};im(4,input.material[2]);im(5,input.material[1]);im(6,input.sdf);}
                if(stage==1){bi[1]={input.lookups,0,u64(input.lookupCount)*48};bi[2]={input.cards,0,input.cardData.size()*64};bi[3]={hits_,0,hitBytes};bi[4]={valid_,0,zero.size()*4};im(5,input.material[2]);im(6,input.material[1]);im(7,output_[previous]);im(8,grid_[previous]);im(9,grid_[slot]);im(10,aux_);}
                if(stage==2){im(1,input.material[0]);im(2,input.material[2]);im(3,input.material[1]);im(4,grid_[slot]);im(5,aux_);im(6,output_[slot]);}
                if(stage==3){im(0,grid_[slot]);im(1,aux_);im(2,output_[slot]);}
                for(u32 b=0;b<count;++b){auto& x=w[b];x.dstSet=set;x.dstBinding=b;x.descriptorCount=1;x.descriptorType=bindings[b].descriptorType;if(x.descriptorType==DescriptorType::UniformBuffer||x.descriptorType==DescriptorType::StorageBuffer)x.bufferInfo=&bi[b];else x.imageInfo=&ii[b];}device.UpdateDescriptorSets(count,w.data());
            }
        }
        ready_=true;PRIMAL_LOG_STREAM(Info,"DawnRadiosity") << "tiles=" << tileCount_ << " rays_per_tile=32 stride=" << params_.schedule[1] << " warmup=" << warmup_ << " offline_sdf=1";return true;
    }
    bool Prepare(DawnCommandBuffer& cmd,u32 frame){
        if(!ready_||prepared_||frame!=next_)return false;
        if(frame==0){for(u32 s=0;s<3;++s)Dispatch(cmd,3,s,(params_.counts[0]+7)/8);Barrier(cmd);}
        params_.schedule[0]=frame;const u32 slot=graphics::lumen::DDGISlotsForFrame(frame).writeIndex;
        if(!device_->UpdateBufferData(uniform_[slot],&params_,sizeof(params_)))return false;prepared_=true;return true;
    }
    ResourceHandle LightInput(u32 frame)const{return ready_&&prepared_&&frame==next_?output_[graphics::lumen::DDGISlotsForFrame(frame).writeIndex]:handles::INVALID_RESOURCE;}
    bool Active(u32 frame)const{return frame>=warmup_;}
    bool CaptureLightInput(DawnCommandBuffer& cmd,u32 frame){
        if(LightInput(frame)==handles::INVALID_RESOURCE||lightReadback_!=handles::INVALID_RESOURCE)return false;
        const u32 size=params_.counts[0];BufferDesc d{};d.size=u64(size)*size*8;d.memoryUsage=GPUMemoryUsage::Readback;lightReadback_=device_->CreateBuffer(d);if(lightReadback_==handles::INVALID_RESOURCE)return false;
        BufferTextureCopyRegion r{};r.bufferRowLength=size;r.bufferImageHeight=size;r.imageExtent={size,size,1};r.imageSubresource={0,0,1};cmd.CopyTextureToBuffer(LightInput(frame),lightReadback_,&r,1);lightCaptureFrame_=frame;return true;
    }

    bool Record(DawnCommandBuffer& cmd,u32 frame,ResourceHandle feedback){
        if(!ready_||!prepared_||frame!=next_||feedback==handles::INVALID_RESOURCE)return false;
        const u32 slot=graphics::lumen::DDGISlotsForFrame(frame).writeIndex;
        if(Active(frame)){DescriptorImageInfo ii{handles::INVALID_SAMPLER,feedback,ResourceState::ShaderResource};WriteDescriptorSet w{};w.dstSet=sets_[1][slot];w.dstBinding=7;w.descriptorCount=1;w.descriptorType=DescriptorType::SampledImage;w.imageInfo=&ii;device_->UpdateDescriptorSets(1,&w);
            Barrier(cmd);Dispatch(cmd,0,slot,(width_+7)/8);Barrier(cmd);Dispatch(cmd,1,slot,(width_+7)/8);Barrier(cmd);Dispatch(cmd,2,slot,(params_.counts[0]+7)/8);Barrier(cmd);}
        feedback_=feedback;prepared_=false;recorded_=frame;++next_;return true;
    }
    // Bounded diagnostics: full tile evidence/reconstruction, and up to 256
    // spatially stratified tiles' actual ray hits. No duplicate SDF/material dump.
    bool Capture(DawnCommandBuffer& cmd,u32 frame){
        if(!ready_||prepared_||next_!=frame+1||recorded_!=frame||readback_!=handles::INVALID_RESOURCE||width_%32)return false;
        const u64 tileBytes=u64(tileCount_)*8,atlasBytes=u64(params_.counts[0])*params_.counts[0]*8;sampleTiles_=std::min(tileCount_,256u);bytes_=tileBytes*3+atlasBytes*2+u64(sampleTiles_)*32*32;
        BufferDesc b{};b.size=bytes_;b.memoryUsage=GPUMemoryUsage::Readback;readback_=device_->CreateBuffer(b);if(readback_==handles::INVALID_RESOURCE)return false;const auto slots=graphics::lumen::DDGISlotsForFrame(frame);
        cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::Transfer,AccessFlag::MemoryWrite,AccessFlag::TransferRead);
        auto copy=[&](ResourceHandle image,u32 size,u64 offset){BufferTextureCopyRegion r{};r.bufferOffset=offset;r.bufferRowLength=(size*8+255)/256*32;r.bufferImageHeight=size;r.imageExtent={size,size,1};r.imageSubresource={0,0,1};cmd.CopyTextureToBuffer(image,readback_,&r,1);};
        // Capture requires tile row bytes aligned to WebGPU's 256-byte copy pitch.
        copy(grid_[slots.previousIndex],width_,0);copy(grid_[slots.writeIndex],width_,tileBytes);copy(aux_,width_,tileBytes*2);copy(output_[slots.writeIndex],params_.counts[0],tileBytes*3);copy(feedback_,params_.counts[0],tileBytes*3+atlasBytes);
        for(u32 i=0;i<sampleTiles_;++i){const u32 begin=u64(i)*tileCount_/sampleTiles_,end=u64(i+1)*tileCount_/sampleTiles_;const u32 tile=begin+(i*73u)%(end-begin);cmd.CopyBuffer(hits_,readback_,u64(tile)*1024,tileBytes*3+atlasBytes*2+u64(i)*1024,1024);}
        cmd.MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);return true;
    }
    bool Save(const char* prefix){
        if(readback_==handles::INVALID_RESOURCE)return false;const auto* data=static_cast<const u8*>(device_->MapBuffer(readback_,0,bytes_));if(!data)return false;
        const u64 textureBytes=u64(tileCount_)*24+u64(params_.counts[0])*params_.counts[0]*16;u64 invalid=0,validTiles=0;float maximum=0;
        for(u64 at=0;at<textureBytes;at+=2){HalfStorage v;std::memcpy(&v,data+at,2);if(!std::isfinite(float(v)))++invalid;}
        for(u32 t=0;t<tileCount_;++t){HalfStorage v[4];std::memcpy(v,data+u64(tileCount_)*8+t*8,8);if(v[3]>0)++validTiles;for(u32 c=0;c<3;++c)maximum=std::max(maximum,float(v[c]));}
        std::ofstream raw(std::string(prefix)+".bin",std::ios::binary);raw.write(reinterpret_cast<const char*>(data),bytes_);raw.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        if(lightReadback_!=handles::INVALID_RESOURCE){
            if(lightCaptureFrame_!=recorded_)return false;const u64 bytes=u64(params_.counts[0])*params_.counts[0]*8;
            const auto* old=device_->MapBuffer(lightReadback_,0,bytes);if(!old)return false;std::ofstream file(std::string(prefix)+"-light-input.bin",std::ios::binary);file.write(static_cast<const char*>(old),bytes);file.close();device_->UnmapBuffer(lightReadback_);device_->DestroyBuffer(lightReadback_);lightReadback_=handles::INVALID_RESOURCE;if(!file.good())return false;
        }
        std::ofstream meta(std::string(prefix)+".json");meta<<"{\"format_version\":2,\"frame\":"<<recorded_<<",\"atlas_size\":"<<params_.counts[0]<<",\"tile_size\":"<<params_.counts[1]<<",\"tile_count\":"<<tileCount_<<",\"sampled_tiles\":"<<sampleTiles_<<",\"refresh_stride\":"<<params_.schedule[1]<<",\"sample_selection\":3,\"rays_per_tile\":32,\"valid_tiles\":"<<validTiles<<",\"max_incident\":"<<maximum<<",\"nonfinite\":"<<invalid<<",\"completion_checked\":true}\n";meta.close();PRIMAL_LOG_STREAM(Info,"DawnRadiosity")<<"frame="<<recorded_<<" valid_tiles="<<validTiles<<" max="<<maximum<<" invalid="<<invalid;return raw.good()&&meta.good()&&invalid==0;
    }
    void Shutdown(){if(!device_)return;for(auto& stage:sets_)for(auto h:stage)if(h!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(h);for(u32 i=0;i<4;++i){if(pipelines_[i]!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipelines_[i]);if(shaders_[i]!=handles::INVALID_SHADER)device_->DestroyShader(shaders_[i]);if(pipelineLayouts_[i]!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayouts_[i]);if(layouts_[i]!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layouts_[i]);}
        for(auto h:buffers_)if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);for(auto h:textures_)if(h!=handles::INVALID_RESOURCE)device_->DestroyTexture(h);if(readback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(readback_);if(lightReadback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(lightReadback_);device_=nullptr;ready_=false;prepared_=false;next_=0;recorded_=~0u;lightCaptureFrame_=~0u;
        buffers_.clear();textures_.clear();owners_=hits_=valid_=feedback_=aux_=readback_=lightReadback_=handles::INVALID_RESOURCE;
        grid_.fill(handles::INVALID_RESOURCE);output_.fill(handles::INVALID_RESOURCE);uniform_.fill(handles::INVALID_RESOURCE);
        shaders_.fill(handles::INVALID_SHADER);pipelines_.fill(handles::INVALID_PIPELINE);pipelineLayouts_.fill(handles::INVALID_PIPELINE_LAYOUT);layouts_.fill(handles::INVALID_DESCRIPTOR_SET_LAYOUT);for(auto& stage:sets_)stage.fill(handles::INVALID_DESCRIPTOR_SET);
    }
private:
    void Dispatch(DawnCommandBuffer& cmd,u32 stage,u32 slot,u32 groups){cmd.BindComputePipeline(pipelines_[stage]);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayouts_[stage],0,1,&sets_[stage][slot],0,nullptr);cmd.Dispatch(groups,groups,1);}
    static void Barrier(DawnCommandBuffer& cmd){cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::MemoryWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);}
    DawnDevice* device_=nullptr;Params params_{};bool ready_=false,prepared_=false;u32 width_=0,tileCount_=0,warmup_=90,next_=0,recorded_=~0u,sampleTiles_=0,lightCaptureFrame_=~0u;u64 bytes_=0;
    std::vector<ResourceHandle> buffers_,textures_;ResourceHandle owners_=handles::INVALID_RESOURCE,hits_=handles::INVALID_RESOURCE,valid_=handles::INVALID_RESOURCE,feedback_=handles::INVALID_RESOURCE,aux_=handles::INVALID_RESOURCE,readback_=handles::INVALID_RESOURCE,lightReadback_=handles::INVALID_RESOURCE;
    std::array<ResourceHandle,3> grid_{},output_{},uniform_{};
    std::array<ShaderHandle,4> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER,handles::INVALID_SHADER,handles::INVALID_SHADER};
    std::array<PipelineHandle,4> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE,handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
    std::array<PipelineLayoutHandle,4> pipelineLayouts_{handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT,handles::INVALID_PIPELINE_LAYOUT};
    std::array<DescriptorSetLayoutHandle,4> layouts_{handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT,handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    std::array<std::array<DescriptorSetHandle,3>,4> sets_{{{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET},{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET},{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET},{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET}}};
};
}
