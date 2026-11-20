#pragma once
#include "DawnSponzaGeometry.h"
#include "Engine/Graphics/Lumen/SurfaceCache/CardGenerator.h"
#include "Engine/Graphics/Lumen/SurfaceCache/OwnerCardRanges.h"
#include "Engine/Graphics/Nanite/MaterialDataBuilder.h"
#include "Engine/Graphics/SceneDataAdapter.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Graphics/RHI/Core/RHIMath.h"

namespace primal::tests::dawn_sponza {
// Static scene capture only. Lighting and consumers must not interpret these
// material atlases as radiance; that is a separate producer.
class SurfaceCacheCapture {
public:
    SurfaceCacheCapture()=default;SurfaceCacheCapture(const SurfaceCacheCapture&)=delete;
    SurfaceCacheCapture& operator=(const SurfaceCacheCapture&)=delete;~SurfaceCacheCapture(){Shutdown();}
    struct alignas(16) Params {primal::math::m4x4 viewProjection;float center[4],forward[4],tint[4];u32 meta[4];};
    static_assert(sizeof(Params)==128);
    bool Initialize(DawnDevice& device,std::span<const graphics::SceneDataMeshInfo> meshes,std::span<const ResourceHandle> albedos,SamplerHandle sampler,const char* capture=nullptr){
        if(device_||meshes.empty()||meshes.size()!=albedos.size())return false;device_=&device;
        generator_.Initialize(&device,2048,32,4096);
        for(u32 i=0;i<meshes.size();++i){
            if(!meshes[i].mesh||albedos[i]==handles::INVALID_RESOURCE||!meshes[i].materialInstance)return false;
            const auto id=content::get_rhi_mesh_id(meshes[i].meshEntityId);graphics::rhi::RHICardBakeData bake;
            if(content::get_rhi_mesh_card_bake(id,bake))generator_.RegisterMeshBaked(bake,graphics::rhi::math::MatrixIdentity(),i);
            else {graphics::rhi::RHIMeshAsset asset{};if(!content::get_rhi_mesh_asset(id,asset)||!asset.num_vertices||asset.position_buffer.size()<size_t(asset.num_vertices)*12)return false;
                float lo[3]{INFINITY,INFINITY,INFINITY},hi[3]{-INFINITY,-INFINITY,-INFINITY};
                const auto* positions=reinterpret_cast<const float*>(asset.position_buffer.data());
                for(u32 v=0;v<asset.num_vertices;++v)for(u32 a=0;a<3;++a){const float value=positions[v*3+a];if(!std::isfinite(value))return false;lo[a]=std::min(lo[a],value);hi[a]=std::max(hi[a],value);}
                generator_.RegisterMesh({lo[0],lo[1],lo[2]},{hi[0],hi[1],hi[2]},i);}
            const auto blas=content::get_rhi_mesh_blas(id);if(!blas||!generator_.BindGeometryIdentity(i,blas->geometry_digest))return false;
            meshes_.push_back(meshes[i].mesh);
        }
        generator_.RebuildCardAllocation();cards_=generator_.GetCards();
        if(cards_.empty()||generator_.GetCapacityTruncatedMeshCount())return false;
        if(!graphics::lumen::BuildOwnerCardDirectory(cards_,generator_.GetLookups(),directory_))return false;
        auto buffer=[&](u64 size,const void* data,BufferType type){BufferDesc d{};d.size=size;d.type=type;d.memoryUsage=GPUMemoryUsage::Static;
            auto h=device.CreateBuffer(d);if(h!=handles::INVALID_RESOURCE&&data&&!device.UpdateBufferData(h,data,size)){device.DestroyBuffer(h);return handles::INVALID_RESOURCE;}return h;};
        // Own checked uploads, independent of CardGenerator's legacy Map path.
        cardsBuffer_=buffer(cards_.size()*64,cards_.data(),BufferType::Raw);directoryBuffer_=buffer(directory_.size()*48,directory_.data(),BufferType::Raw);
        if(cardsBuffer_==handles::INVALID_RESOURCE||directoryBuffer_==handles::INVALID_RESOURCE)return false;
        const DataFormat formats[]{DataFormat::RGBA8_UNorm,DataFormat::RG16_Float,DataFormat::R32_Float,DataFormat::D32_Float};
        for(u32 i=0;i<4;++i){TextureDesc d{};d.size={2048,2048,1};d.type=TextureType::Texture2D;d.format=formats[i];d.usage=i==3?TextureUsage::DepthStencil:TextureUsage::RenderTarget|TextureUsage::ShaderResource|TextureUsage::CopySource;
            atlases_[i]=device.CreateTexture(d);if(atlases_[i]==handles::INVALID_RESOURCE)return false;}
        std::ifstream in("Engine/Graphics/Dawn/shaders/Lumen/SurfaceCacheCapture.wgsl");std::string source((std::istreambuf_iterator<char>(in)),{});if(source.empty())return false;
        vertex_=device.CreateShader(source.data(),source.size(),ShaderStage::Vertex,"card_capture_vertex");fragment_=device.CreateShader(source.data(),source.size(),ShaderStage::Pixel,"card_capture_fragment");
        if(vertex_==handles::INVALID_SHADER||fragment_==handles::INVALID_SHADER)return false;
        DescriptorSetLayoutBinding b[]{{0,DescriptorType::UniformBuffer,1,ShaderStage::Vertex|ShaderStage::Pixel,nullptr},{1,DescriptorType::SampledImage,1,ShaderStage::Pixel,nullptr},{2,DescriptorType::Sampler,1,ShaderStage::Pixel,nullptr}};
        layout_=device.CreateDescriptorSetLayout({3,b});if(layout_==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
        PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&layout_;pipelineLayout_=device.CreatePipelineLayout(pl);if(pipelineLayout_==handles::INVALID_PIPELINE_LAYOUT)return false;
        GraphicsPipelineDesc p{};p.vertexShader=vertex_;p.pixelShader=fragment_;p.layout=pipelineLayout_;p.topology=PrimitiveTopology::TriangleList;
        p.cullMode=CullMode::None;p.enableDepthTest=true;p.enableDepthWrite=true;p.depthFunc=ComparisonFunc::Less;p.depthStencilFormat=DataFormat::D32_Float;
        p.renderTargetCount=3;for(u32 i=0;i<3;++i)p.renderTargetFormats[i]=formats[i];
        p.vertexAttributes.push_back({0,0,DataFormat::RGB32_Float,0});p.vertexAttributes.push_back({1,0,DataFormat::R32_UInt,12});p.vertexAttributes.push_back({2,0,DataFormat::R32_UInt,16});p.vertexAttributes.push_back({3,0,DataFormat::R32_UInt,20});p.vertexAttributes.push_back({4,0,DataFormat::RG32_Float,24});p.vertexBindings.push_back({0,32,true});
        pipeline_=device.CreateGraphicsPipeline(p);if(pipeline_==handles::INVALID_PIPELINE)return false;
        WGPULimits limits=WGPU_LIMITS_INIT;if(wgpuDeviceGetLimits(device.GetNativeDevice(),&limits)!=WGPUStatus_Success)return false;
        stride_=(sizeof(Params)+limits.minUniformBufferOffsetAlignment-1)/limits.minUniformBufferOffsetAlignment*limits.minUniformBufferOffsetAlignment;
        std::vector<u8> all(cards_.size()*stride_);sets_.resize(cards_.size(),handles::INVALID_DESCRIPTOR_SET);
        for(u32 i=0;i<cards_.size();++i){Params params{};if(!MakeParams(cards_[i],params))return false;
            const auto owner=cards_[i].mesh_instance_id;if(owner>=meshes.size())return false;
            const auto material=graphics::nanite::MaterialDataBuilder::ExtractMaterialData(meshes[owner].materialInstance.get());
            std::copy_n(material.albedo_tint,3,params.tint);params.tint[3]=1;params.meta[0]=material.flags;params.meta[1]=owner;
            std::memcpy(all.data()+i*stride_,&params,sizeof(params));}
        params_=buffer(all.size(),all.data(),BufferType::Constant);if(params_==handles::INVALID_RESOURCE)return false;
        for(u32 i=0;i<cards_.size();++i){sets_[i]=device.CreateDescriptorSet({layout_});if(sets_[i]==handles::INVALID_DESCRIPTOR_SET)return false;
            DescriptorBufferInfo info{params_,i*stride_,sizeof(Params)};DescriptorImageInfo image{sampler,albedos[cards_[i].mesh_instance_id],ResourceState::ShaderResource};WriteDescriptorSet w[3]{};
            for(u32 k=0;k<3;++k){w[k].dstSet=sets_[i];w[k].dstBinding=k;w[k].descriptorCount=1;w[k].descriptorType=b[k].descriptorType;if(k==0)w[k].bufferInfo=&info;else w[k].imageInfo=&image;}device.UpdateDescriptorSets(3,w);}
        if(capture){std::ofstream f(std::string(capture)+"-cards.bin",std::ios::binary);f.write(reinterpret_cast<const char*>(cards_.data()),cards_.size()*64);f.close();
            std::ofstream d(std::string(capture)+"-directory.bin",std::ios::binary);d.write(reinterpret_cast<const char*>(directory_.data()),directory_.size()*48);d.close();if(!f.good()||!d.good())return false;}
        ready_=true;PRIMAL_LOG_STREAM(Info,"DawnSurfaceCache") << "cards=" << cards_.size() << " owners=" << directory_.size() << " baked=" << generator_.GetBakedMeshCount() << " fallback=" << generator_.GetFallbackMeshCount();return true;
    }
    bool Record(DawnCommandBuffer& cmd){
        if(!ready_)return false;
        // Frozen default capture keeps rotating after the first full coverage.
        const size_t batch=captureSeeded_?8u:32u;
        const size_t faces=std::min(cards_.size(),batch);
        const size_t first=captureCursor_;
        cmd.MemoryBarrier(PipelineStage::Host,PipelineStage::VertexShader|PipelineStage::FragmentShader,AccessFlag::HostWrite,AccessFlag::ShaderRead);
        for(size_t face=0;face<faces;++face){const size_t ci=(first+face)%cards_.size();const auto& card=cards_[ci];RenderPassDesc pass{};pass.colorAttachments.resize(3);
            const DataFormat formats[]{DataFormat::RGBA8_UNorm,DataFormat::RG16_Float,DataFormat::R32_Float};
            for(u32 k=0;k<3;++k){auto& a=pass.colorAttachments[k];a.texture=atlases_[k];a.format=formats[k];a.loadOp=(captured_==0&&face==0)?LoadAction::Clear:LoadAction::Load;a.storeOp=StoreAction::Store;a.clearValue.color={0,0,0,0};}
            pass.depthAttachment.texture=atlases_[3];pass.depthAttachment.format=DataFormat::D32_Float;pass.depthAttachment.loadOp=LoadAction::Clear;pass.depthAttachment.storeOp=StoreAction::Store;pass.depthAttachment.clearValue.depth=1;
            pass.viewport={{float(card.atlas_offset_x),float(card.atlas_offset_y)},{float(card.resolution),float(card.resolution)},0,1};pass.scissor={{int(card.atlas_offset_x),int(card.atlas_offset_y)},{card.resolution,card.resolution}};
            cmd.BeginRenderPass(pass);cmd.BindGraphicsPipeline(pipeline_);cmd.BindDescriptorSets(PipelineBindPoint::Graphics,pipelineLayout_,0,1,&sets_[ci],0,nullptr);meshes_[card.mesh_instance_id]->Draw(&cmd,1,0,0);cmd.EndRenderPass();
        }
        captured_=std::min(cards_.size(),captured_+faces);
        encodedDraws_+=faces;
        captureCursor_=(first+faces)%cards_.size();
        if(captureCursor_==0&&faces>0)captureSeeded_=true;
        if(const char* audit=std::getenv("SPONZA_SURFACE_SCHEDULE_AUDIT");audit&&audit[0]=='1')
            PRIMAL_LOG_STREAM(Info,"DawnSurfaceCache") << "[SC_CAPTURE_SCHEDULE] frame=" << recordFrames_
                << " first=" << first << " faces=" << faces << " cursor=" << captureCursor_
                << " seeded=" << captureSeeded_ << " covered=" << captured_ << " draws=" << encodedDraws_;
        ++recordFrames_;
        return true;
    }
    // True after all cards have been drawn at least once (not the budget switch).
    // CPU publication still requires successful submission/completion.
    bool RecordedAllCards()const{return ready_&&captured_==cards_.size();}
    bool Capture(DawnCommandBuffer& cmd){
        if(!RecordedAllCards()||readback_!=handles::INVALID_RESOURCE)return false;
        BufferDesc d{};d.size=3ull*2048*2048*4;d.memoryUsage=GPUMemoryUsage::Readback;
        readback_=device_->CreateBuffer(d);if(readback_==handles::INVALID_RESOURCE)return false;
        for(u32 k=0;k<3;++k){ResourceBarrier b{};b.resource=atlases_[k];b.beforeState=ResourceState::RenderTarget;b.afterState=ResourceState::CopySource;b.subresource=~0u;cmd.InsertBarrier(&b,1);
            BufferTextureCopyRegion region{};region.bufferOffset=u64(k)*2048*2048*4;region.bufferRowLength=2048;region.bufferImageHeight=2048;region.imageExtent={2048,2048,1};region.imageSubresource={0,0,1};
            cmd.CopyTextureToBuffer(atlases_[k],readback_,&region,1);}
        cmd.MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);return true;
    }
    bool VerifyAndSave(const char* prefix){
        if(!RecordedAllCards()||readback_==handles::INVALID_RESOURCE)return false;
        constexpr u64 pixels=2048ull*2048,bytes=pixels*12;
        const auto* data=static_cast<const u8*>(device_->MapBuffer(readback_,0,bytes));if(!data)return false;
        std::vector<u8> covered(pixels,0);u32 nonempty=0,valid=0,invalid=0;std::vector<u32> perCard;
        for(const auto& card:cards_){u32 count=0;for(u32 y=0;y<card.resolution;++y)for(u32 x=0;x<card.resolution;++x){
                const u64 at=u64(card.atlas_offset_y+y)*2048+card.atlas_offset_x+x;
                if(covered[at])++invalid;covered[at]=1;count+=data[at*4+3]>=128;}
            nonempty+=count>0;perCard.push_back(count);}
        for(u64 i=0;i<pixels;++i){if(data[i*4+3]==0)continue;++valid;if(!covered[i]||data[i*4+3]<128)++invalid;
            float depth;std::memcpy(&depth,data+pixels*8+i*4,4);if(!std::isfinite(depth)||depth<.0499f)++invalid;
            _Float16 oct[2];std::memcpy(oct,data+pixels*4+i*4,4);for(auto v:oct)if(!std::isfinite(float(v))||v<0||v>1)++invalid;}
        std::ofstream raw(std::string(prefix)+"-atlas.bin",std::ios::binary);raw.write(reinterpret_cast<const char*>(data),bytes);raw.close();
        std::ofstream meta(std::string(prefix)+"-atlas.json");meta << "{\"atlas_size\":2048,\"cards\":" << cards_.size() << ",\"encoded_cards\":" << captured_ << ",\"encoded_draws\":" << encodedDraws_ << ",\"record_frames\":" << recordFrames_ << ",\"capture_cursor\":" << captureCursor_ << ",\"capture_seeded\":" << (captureSeeded_?"true":"false") << ",\"nonempty_cards\":" << nonempty << ",\"valid_texels\":" << valid << ",\"invalid_texels\":" << invalid << ",\"completion_checked\":true,\"radiance_connected\":false,\"card_valid_texels\":[";
        for(size_t i=0;i<perCard.size();++i){if(i)meta << ',';meta << perCard[i];}meta << "]}\n";meta.close();
        const bool ok=raw.good()&&meta.good()&&invalid==0&&valid>0;device_->UnmapBuffer(readback_);device_->DestroyBuffer(readback_);readback_=handles::INVALID_RESOURCE;
        PRIMAL_LOG_STREAM(Info,"DawnSurfaceCache") << "captured=" << captured_ << " nonempty=" << nonempty << " valid=" << valid << " invalid=" << invalid;return ok;
    }
    size_t CapturedCards()const{return captured_;}
    const std::vector<graphics::lumen::SurfaceCacheCard>& CardData()const{return cards_;}
    bool Ready()const{return ready_;}
    u32 OwnerCount()const{return u32(directory_.size());}
    ResourceHandle Atlas(u32 i)const{return i<3?atlases_[i]:handles::INVALID_RESOURCE;}
    ResourceHandle Cards()const{return cardsBuffer_;}ResourceHandle Directory()const{return directoryBuffer_;}
    ResourceHandle GridOffsets()const{return ready_?generator_.GetGridOffsetsBuffer():handles::INVALID_RESOURCE;}
    ResourceHandle GridCards()const{return ready_?generator_.GetGridCardsBuffer():handles::INVALID_RESOURCE;}
    static bool MakeParams(const graphics::lumen::SurfaceCacheCard& card,Params& p){
        namespace rm=graphics::rhi::math;const u32 axis=card.axis_direction&255u,dir=(card.axis_direction>>8u)&255u;if(axis>2||dir>1)return false;
        primal::math::v3 fwd{},up{};float hu,hv,half;bool mirror;
        if(axis==0){fwd={dir?1.f:-1.f,0,0};up={0,0,-1};hu=card.extent.y;hv=card.extent.z;half=card.extent.x;mirror=!dir;}
        else if(axis==1){fwd={0,dir?1.f:-1.f,0};up={0,0,-1};hu=card.extent.x;hv=card.extent.z;half=card.extent.y;mirror=dir;}
        else{fwd={0,0,dir?1.f:-1.f};up={0,-1,0};hu=card.extent.x;hv=card.extent.y;half=card.extent.z;mirror=!dir;}
        if(hu<=0||hv<=0||half<=0||card.resolution==0||card.atlas_offset_x+card.resolution>2048||card.atlas_offset_y+card.resolution>2048)return false;
        const primal::math::v3 center{card.center.x,card.center.y,card.center.z};const auto view=rm::CreateLookAtMatrix(center,center+fwd,up);
        const float far=2*half*graphics::rhi::card_surface_identity::CaptureFarScale+graphics::rhi::card_surface_identity::CaptureFarBias;
        p.viewProjection=rm::CreateOrthographicMatrix(mirror?hu:-hu,mirror?-hu:hu,-hv,hv,.05f,far)*view;
        p.center[0]=center.x;p.center[1]=center.y;p.center[2]=center.z;p.center[3]=1;p.forward[0]=fwd.x;p.forward[1]=fwd.y;p.forward[2]=fwd.z;return true;
    }
    void Shutdown(){if(!device_)return;for(auto s:sets_)if(s!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(s);sets_.clear();
        if(pipeline_!=handles::INVALID_PIPELINE)device_->DestroyPipeline(pipeline_);if(pipelineLayout_!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(pipelineLayout_);if(layout_!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(layout_);
        for(auto s:{vertex_,fragment_})if(s!=handles::INVALID_SHADER)device_->DestroyShader(s);
        for(auto t:atlases_)if(t!=handles::INVALID_RESOURCE)device_->DestroyTexture(t);
        for(auto b:{readback_,params_,cardsBuffer_,directoryBuffer_,generator_.GetCardDataBuffer(),generator_.GetCardLookupBuffer(),generator_.GetGridOffsetsBuffer(),generator_.GetGridCardsBuffer(),generator_.GetPageTableBuffer()})if(b!=handles::INVALID_RESOURCE)device_->DestroyBuffer(b);
        device_=nullptr;ready_=false;captured_=0;captureCursor_=0;captureSeeded_=false;encodedDraws_=0;recordFrames_=0;cards_.clear();directory_.clear();meshes_.clear();generator_=graphics::lumen::CardGenerator{};
        readback_=params_=cardsBuffer_=directoryBuffer_=handles::INVALID_RESOURCE;atlases_.fill(handles::INVALID_RESOURCE);vertex_=fragment_=handles::INVALID_SHADER;pipeline_=handles::INVALID_PIPELINE;pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
private:
    DawnDevice* device_=nullptr;bool ready_=false;size_t captured_=0,captureCursor_=0;
    bool captureSeeded_=false;u64 encodedDraws_=0,recordFrames_=0,stride_=0;
    graphics::lumen::CardGenerator generator_;std::vector<graphics::lumen::SurfaceCacheCard> cards_;std::vector<graphics::lumen::SurfaceCacheCardLookup> directory_;std::vector<graphics::RenderMesh*> meshes_;
    std::vector<DescriptorSetHandle> sets_;ResourceHandle readback_=handles::INVALID_RESOURCE,params_=handles::INVALID_RESOURCE,cardsBuffer_=handles::INVALID_RESOURCE,directoryBuffer_=handles::INVALID_RESOURCE;
    std::array<ResourceHandle,4> atlases_{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};
    ShaderHandle vertex_=handles::INVALID_SHADER,fragment_=handles::INVALID_SHADER;PipelineHandle pipeline_=handles::INVALID_PIPELINE;PipelineLayoutHandle pipelineLayout_=handles::INVALID_PIPELINE_LAYOUT;DescriptorSetLayoutHandle layout_=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
};
}
