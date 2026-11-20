#pragma once
#include "DawnSponzaSurfaceCache.h"
#include "DawnSponzaShadows.h"
#include "Engine/Graphics/Lumen/DDGISlotMap.h"

namespace primal::tests::dawn_sponza {
// Static material-card radiance producer. Borrowed material/SDF/CSM resources
// must outlive this object. Record publishes encoded output, not CPU completion.
// Full-card refresh is deliberate here; the production amortized scheduler and
// tile radiosity is supplied explicitly by Record. The frozen default solo
// policy disables both probe feed and legacy self-lighting feedback.
class SurfaceCacheLighting {
public:
    struct alignas(16) ShadowUniform {
        float origin[4]{},extent[4]{},toward[4]{},policy[4]{50,1.5f,0,0};u32 counts[4]{};
        primal::math::m4x4 matrices[4]{};float shadowMeta[4]{4,.75f,0,0};
    };
    struct alignas(16) LightUniform {u32 counts[4]{};float grid[4]{};u32 probes[4]{},gates[4]{};float policy[4]{};};
    struct alignas(16) Light {float position[4]{},color[4]{150,150,150,0},direction[4]{.5f,-.7f,.3f,1};};
    struct Dispatch {u32 range[4],address[4];};
    static_assert(sizeof(ShadowUniform)==352&&sizeof(LightUniform)==80&&sizeof(Light)==48&&sizeof(Dispatch)==32);
    struct Inputs {
        // Frozen GIProbeFeedBootstrapEnabled() defaults to true. False is an
        // explicit legacy fixture mode, not a fallback for missing radiosity.
        bool radiositySolo=true;
        float radiosityWeight=.5f; // Frozen Vulkan host default, not shader fallback .25.
        u32 atlasSize=2048;ResourceHandle cards=handles::INVALID_RESOURCE;
        std::span<const graphics::lumen::SurfaceCacheCard> cardData;
        std::array<ResourceHandle,3> material{};ResourceHandle sdf=handles::INVALID_RESOURCE;
        std::array<ResourceHandle,4> csm{};ShadowUniform shadow{};Light light{};
    };
    SurfaceCacheLighting()=default;SurfaceCacheLighting(const SurfaceCacheLighting&)=delete;
    SurfaceCacheLighting& operator=(const SurfaceCacheLighting&)=delete;~SurfaceCacheLighting(){Shutdown();}
    bool Initialize(DawnDevice& device,const Inputs& in){
        if(device_||!std::isfinite(in.radiosityWeight)||in.radiosityWeight<=0||in.radiosityWeight>4||in.cardData.empty()||!in.atlasSize||in.atlasSize%32||in.cards==handles::INVALID_RESOURCE||in.sdf==handles::INVALID_RESOURCE)return false;
        for(auto h:in.material)if(h==handles::INVALID_RESOURCE)return false;for(auto h:in.csm)if(h==handles::INVALID_RESOURCE)return false;
        std::vector<Dispatch> dispatch;std::vector<bool> occupied(u64(in.atlasSize)*in.atlasSize);u64 texels=0;
        for(u32 i=0;i<in.cardData.size();++i){const auto& c=in.cardData[i];
            if(!c.resolution||u64(c.atlas_offset_x)+c.resolution>in.atlasSize||u64(c.atlas_offset_y)+c.resolution>in.atlasSize)return false;
            for(u32 y=0;y<c.resolution;++y)for(u32 x=0;x<c.resolution;++x){const auto at=u64(c.atlas_offset_y+y)*in.atlasSize+c.atlas_offset_x+x;if(occupied[at])return false;occupied[at]=true;}
            dispatch.push_back({{u32(texels),u32(c.resolution)*c.resolution,c.resolution,c.atlas_offset_x},{c.atlas_offset_y,i,0,0}});texels+=u64(c.resolution)*c.resolution;
        }
        WGPULimits limits=WGPU_LIMITS_INIT;if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success||in.atlasSize>limits.maxTextureDimension2D||
           u64(in.atlasSize)*in.atlasSize*4>limits.maxStorageBufferBindingSize||(texels+255)/256>limits.maxComputeWorkgroupsPerDimension)return false;
        device_=&device;size_=in.atlasSize;texels_=u32(texels);cards_=u32(in.cardData.size());shadow_=in.shadow;
        shadow_.counts[0]=texels_;shadow_.counts[1]=cards_;shadow_.counts[2]=size_;light_.counts[0]=texels_;light_.counts[1]=cards_;light_.counts[2]=1;
        light_.policy[1]=in.radiositySolo?1.f:0.f;std::memcpy(&light_.gates[1],&in.radiosityWeight,sizeof(float));
        auto buffer=[&](u64 bytes,const void* data,BufferType type,GPUMemoryUsage memory=GPUMemoryUsage::Static){BufferDesc d{};d.size=bytes;d.type=type;d.memoryUsage=memory;
            auto h=device.CreateBuffer(d);buffers_.push_back(h);if(h!=handles::INVALID_RESOURCE){if(data&&!device.UpdateBufferData(h,data,bytes))return handles::INVALID_RESOURCE;}return h;};
        const auto dispatchBuffer=buffer(dispatch.size()*32,dispatch.data(),BufferType::Raw);
        visibility_=buffer(u64(size_)*size_*4,nullptr,BufferType::Raw);const auto lights=buffer(sizeof(in.light),&in.light,BufferType::Raw);
        const float zeros[36]{};const auto dummy=buffer(sizeof(zeros),zeros,BufferType::Raw);
        if(dispatchBuffer==handles::INVALID_RESOURCE||visibility_==handles::INVALID_RESOURCE||lights==handles::INVALID_RESOURCE||dummy==handles::INVALID_RESOURCE)return false;
        auto texture=[&](){TextureDesc d{};d.type=TextureType::Texture2D;d.size={size_,size_,1};d.format=DataFormat::RGBA16_Float;
            d.usage=TextureUsage::ShaderResource|TextureUsage::UnorderedAccess|TextureUsage::CopySource;auto h=device.CreateTexture(d);textures_.push_back(h);return h;};
        for(u32 i=0;i<3;++i){lighting_[i]=texture();nodirect_[i]=texture();}
        // A separate zero atlas avoids sampled/storage aliasing. Explicitly clear
        // it in its own pass before any lighting dispatch.
        zero_=texture();zeroAux_=texture();
        for(auto h:buffers_)if(h==handles::INVALID_RESOURCE)return false;for(auto h:textures_)if(h==handles::INVALID_RESOURCE)return false;
        DescriptorSetLayoutBinding b[22]{};
        for(u32 i=0;i<22;++i){b[i].binding=i;b[i].descriptorCount=1;b[i].stageFlags=ShaderStage::Compute;b[i].descriptorType=DescriptorType::SampledImage;}
        for(u32 i:{0,1,10,12,17,18,19}){b[i].descriptorType=DescriptorType::StorageBuffer;b[i].readonly=i!=10;}
        b[2].descriptorType=b[11].descriptorType=DescriptorType::UniformBuffer;b[3].is3D=true;b[4].unfilterableFloat=true;
        for(u32 i=6;i<10;++i)b[i].descriptorType=DescriptorType::SampledDepthImage;
        b[20].descriptorType=b[21].descriptorType=DescriptorType::StorageImage;b[20].format=b[21].format=DataFormat::RGBA16_Float;
        layout_=device.CreateDescriptorSetLayout({22,b});if(layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
        PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&layout_;pipelineLayout_=device.CreatePipelineLayout(pl);if(pipelineLayout_==handles::INVALID_PIPELINE_LAYOUT)return false;
        std::ifstream file("Engine/Graphics/Dawn/shaders/Lumen/SurfaceCacheLighting.wgsl");std::string source((std::istreambuf_iterator<char>(file)),{});if(source.empty())return false;
        const char* entries[]{"surface_cache_shadow","surface_cache_light_eval","surface_cache_clear"};
        for(u32 i=0;i<3;++i){shaders_[i]=device.CreateShader(source.data(),source.size(),ShaderStage::Compute,entries[i]);if(shaders_[i]==handles::INVALID_SHADER)return false;
            ComputePipelineDesc cp{};cp.computeShader=shaders_[i];cp.layout=pipelineLayout_;pipelines_[i]=device.CreateComputePipeline(cp);if(pipelines_[i]==handles::INVALID_PIPELINE)return false;}
        for(u32 slot=0;slot<4;++slot){
            shadowBuffers_[slot]=buffer(sizeof(shadow_),&shadow_,BufferType::Constant,GPUMemoryUsage::Dynamic);
            lightBuffers_[slot]=buffer(sizeof(light_),&light_,BufferType::Constant,GPUMemoryUsage::Dynamic);
            if(shadowBuffers_[slot]==handles::INVALID_RESOURCE||lightBuffers_[slot]==handles::INVALID_RESOURCE)return false;
            sets_[slot]=device.CreateDescriptorSet({layout_});if(sets_[slot]==handles::INVALID_DESCRIPTOR_SET)return false;
            DescriptorBufferInfo bi[22]{};DescriptorImageInfo ii[22]{};WriteDescriptorSet writes[22]{};
            bi[0]={in.cards,0,u64(cards_)*64};bi[1]={dispatchBuffer,0,dispatch.size()*32};bi[2]={shadowBuffers_[slot],0,sizeof(shadow_)};
            bi[10]={visibility_,0,u64(size_)*size_*4};bi[11]={lightBuffers_[slot],0,sizeof(light_)};bi[12]={lights,0,sizeof(Light)};
            for(u32 i:{17,18,19})bi[i]={dummy,0,sizeof(zeros)};
            auto image=[&](u32 binding,ResourceHandle resource){ii[binding]={handles::INVALID_SAMPLER,resource,ResourceState::ShaderResource};};
            image(3,in.sdf);image(4,in.material[2]);image(5,in.material[1]);for(u32 i=0;i<4;++i)image(6+i,in.csm[i]);
            image(13,in.material[0]);image(14,zero_);image(16,zero_);
            // Production SC feedback uses N-2, distinct from temporal N-1.
            image(15,lighting_[graphics::lumen::DDGISlotsForFrame(slot+1).currentIndex]);
            image(20,slot<3?lighting_[slot]:zero_);image(21,slot<3?nodirect_[slot]:zeroAux_);
            // Clear set must not bind its output also as a sampled resource.
            if(slot==3){image(14,lighting_[0]);image(16,lighting_[0]);}
            for(u32 i=0;i<22;++i){auto& w=writes[i];w.dstSet=sets_[slot];w.dstBinding=i;w.descriptorCount=1;w.descriptorType=b[i].descriptorType;
                if(w.descriptorType==DescriptorType::StorageBuffer||w.descriptorType==DescriptorType::UniformBuffer)w.bufferInfo=&bi[i];else w.imageInfo=&ii[i];}
            device.UpdateDescriptorSets(22,writes);
        }
        ready_=true;PRIMAL_LOG_STREAM(Info,"DawnSurfaceLighting") << "cards=" << cards_ << " texels=" << texels_ << " atlas=" << size_ << " full_refresh=1 probe_feedback=0 radiosity_solo=" << in.radiositySolo << " tile_radiosity=0";return true;
    }
    bool Record(DawnCommandBuffer& cmd,u32 frame,const primal::math::m4x4* matrices,ResourceHandle indirect=handles::INVALID_RESOURCE,bool indirectEnabled=false){
        if(!ready_||frame!=nextFrame_||(indirectEnabled&&indirect==handles::INVALID_RESOURCE))return false;const u32 slot=graphics::lumen::DDGISlotsForFrame(frame).writeIndex;
        if(matrices)std::copy_n(matrices,4,shadow_.matrices);
        light_.gates[0]=indirectEnabled?1u:0u;
        if(!device_->UpdateBufferData(lightBuffers_[slot],&light_,sizeof(light_)))return false;
        DescriptorImageInfo indirectInfo{handles::INVALID_SAMPLER,indirect==handles::INVALID_RESOURCE?zero_:indirect,ResourceState::ShaderResource};
        WriteDescriptorSet indirectWrite{};indirectWrite.dstSet=sets_[slot];indirectWrite.dstBinding=16;indirectWrite.descriptorCount=1;indirectWrite.descriptorType=DescriptorType::SampledImage;indirectWrite.imageInfo=&indirectInfo;device_->UpdateDescriptorSets(1,&indirectWrite);
        if(!device_->UpdateBufferData(shadowBuffers_[slot],&shadow_,sizeof(shadow_)))return false;
        cmd.MemoryBarrier(PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::MemoryWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
        auto dispatch=[&](u32 p,u32 s,u32 x,u32 y){cmd.BindComputePipeline(pipelines_[p]);cmd.BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayout_,0,1,&sets_[s],0,nullptr);cmd.Dispatch(x,y,1);};
        if(frame==0){for(u32 i=0;i<4;++i)dispatch(2,i,(size_+7)/8,(size_+7)/8);cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);}
        dispatch(0,slot,(texels_+255)/256,1);cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead);
        dispatch(1,slot,(texels_+255)/256,1);cmd.MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::AllCommands,AccessFlag::ShaderWrite,AccessFlag::ShaderRead|AccessFlag::TransferRead);
        recorded_=frame;++nextFrame_;return true;
    }
    ResourceHandle Radiance(u32 frame,bool noDirect=false)const{return ready_&&nextFrame_&&recorded_==frame?(noDirect?nodirect_:lighting_)[graphics::lumen::DDGISlotsForFrame(frame).writeIndex]:handles::INVALID_RESOURCE;}
    ResourceHandle FeedbackRadiance(u32 frame)const{return Radiance(frame)!=handles::INVALID_RESOURCE?lighting_[graphics::lumen::DDGISlotsForFrame(frame+1u).currentIndex]:handles::INVALID_RESOURCE;}
    bool Capture(DawnCommandBuffer& cmd,u32 frame){
        if(Radiance(frame)==handles::INVALID_RESOURCE||readback_!=handles::INVALID_RESOURCE)return false;
        const u64 pixels=u64(size_)*size_;BufferDesc d{};d.size=pixels*20;d.memoryUsage=GPUMemoryUsage::Readback;readback_=device_->CreateBuffer(d);if(readback_==handles::INVALID_RESOURCE)return false;
        for(u32 i=0;i<2;++i){BufferTextureCopyRegion region{};region.bufferOffset=i*pixels*8;region.bufferRowLength=size_;region.bufferImageHeight=size_;region.imageExtent={size_,size_,1};region.imageSubresource={0,0,1};cmd.CopyTextureToBuffer(Radiance(frame,i!=0),readback_,&region,1);}
        cmd.CopyBuffer(visibility_,readback_,0,pixels*16,pixels*4);cmd.MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);return true;
    }
    // Caller must check End, Submit and WaitForCompletion before this CPU read.
    bool VerifyAndSave(const char* prefix){
        if(readback_==handles::INVALID_RESOURCE)return false;const u64 pixels=u64(size_)*size_,bytes=pixels*20;
        const auto* data=static_cast<const u8*>(device_->MapBuffer(readback_,0,bytes));if(!data)return false;
        u64 valid=0,lit=0,shadowed=0,invalid=0;float maximum=0;
        for(u64 p=0;p<pixels;++p){_Float16 a[4],b[4];float v;std::memcpy(a,data+p*8,8);std::memcpy(b,data+pixels*8+p*8,8);std::memcpy(&v,data+pixels*16+p*4,4);
            if(a[3]!=b[3]||(a[3]!=0&&a[3]!=1))++invalid;if(a[3]==1){++valid;lit+=a[0]>0||a[1]>0||a[2]>0;shadowed+=v<1;}
            if(!std::isfinite(v)||v<0||v>1)++invalid;
            for(u32 c=0;c<3;++c){const float x=a[c],y=b[c];maximum=std::max(maximum,x);if(!std::isfinite(x)||!std::isfinite(y)||x<0||x>64||y<0||y>x+.0625f)++invalid;}}
        std::ofstream raw(std::string(prefix)+"-radiance.bin",std::ios::binary);raw.write(reinterpret_cast<const char*>(data),bytes);raw.close();device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        std::ofstream meta(std::string(prefix)+"-radiance.json");meta << "{\"atlas_size\":" << size_ << ",\"native_frame\":" << recorded_ << ",\"cards\":" << cards_ << ",\"valid_texels\":" << valid << ",\"lit_texels\":" << lit << ",\"shadowed_valid_texels\":" << shadowed << ",\"invalid_components\":" << invalid << ",\"max_radiance\":" << maximum << ",\"completion_checked\":true,\"probe_feedback\":false,\"radiosity_solo\":" << (light_.policy[1]!=0?"true":"false") << ",\"tile_radiosity\":" << (light_.gates[0]?"true":"false") << ",\"final_consumer\":false}\n";meta.close();
        PRIMAL_LOG_STREAM(Info,"DawnSurfaceLighting") << "valid=" << valid << " lit=" << lit << " shadowed=" << shadowed << " invalid=" << invalid << " max=" << maximum;
        return raw.good()&&meta.good()&&invalid==0&&valid>0;
    }
    void Shutdown(){if(!device_)return;
        for(auto h:sets_)if(h!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(h);
        for(auto h:pipelines_)if(h!=handles::INVALID_PIPELINE)device_->DestroyPipeline(h);
        for(auto h:shaders_)if(h!=handles::INVALID_SHADER)device_->DestroyShader(h);
        if(pipelineLayout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayout_);if(layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layout_);
        for(auto h:buffers_)if(h!=handles::INVALID_RESOURCE)device_->DestroyBuffer(h);if(readback_!=handles::INVALID_RESOURCE)device_->DestroyBuffer(readback_);
        for(auto h:textures_)if(h!=handles::INVALID_RESOURCE)device_->DestroyTexture(h);
        device_=nullptr;ready_=false;buffers_.clear();textures_.clear();nextFrame_=0;readback_=handles::INVALID_RESOURCE;
        shaders_.fill(handles::INVALID_SHADER);pipelines_.fill(handles::INVALID_PIPELINE);sets_.fill(handles::INVALID_DESCRIPTOR_SET);
        pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
private:
    DawnDevice* device_=nullptr;bool ready_=false;u32 size_=0,texels_=0,cards_=0,nextFrame_=0,recorded_=0;ShadowUniform shadow_{};LightUniform light_{};
    std::vector<ResourceHandle> buffers_,textures_;ResourceHandle visibility_=handles::INVALID_RESOURCE,zero_=handles::INVALID_RESOURCE,zeroAux_=handles::INVALID_RESOURCE,readback_=handles::INVALID_RESOURCE;
    std::array<ResourceHandle,3> lighting_{},nodirect_{};std::array<ResourceHandle,4> shadowBuffers_{},lightBuffers_{};
    std::array<ShaderHandle,3> shaders_{handles::INVALID_SHADER,handles::INVALID_SHADER,handles::INVALID_SHADER};
    std::array<PipelineHandle,3> pipelines_{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
    std::array<DescriptorSetHandle,4> sets_{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};
    PipelineLayoutHandle pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;DescriptorSetLayoutHandle layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
};
}
