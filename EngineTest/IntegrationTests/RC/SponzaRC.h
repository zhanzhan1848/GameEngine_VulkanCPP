#pragma once
// Test executable only. All commands are submitted after a checked production
// frame fence, on the same graphics queue. No production resource is written.
#include <vk_mem_alloc.h>
#include <filesystem>
#include <array>
#include <bit>
namespace sponza_rc {
inline void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
struct alignas(16) CB {
    primal::math::m4x4 vp,inverse;
    float origin[4],camera[4],sky[4];
    u32 screen[4],levels[3][4],offsets[4];float jitter[4];
};
static_assert(sizeof(CB)==272);
struct Test {
    RHIDeviceBase* d; VulkanDevice* vk;
    DescriptorSetLayoutHandle layout=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    PipelineLayoutHandle pipelineLayout=handles::INVALID_PIPELINE_LAYOUT;
    ShaderHandle shader=handles::INVALID_SHADER;PipelineHandle pipe=handles::INVALID_PIPELINE;
    DescriptorSetHandle set=handles::INVALID_DESCRIPTOR_SET;
    ResourceHandle rays,merged,active,constants,out,readback,baselineReadback;
    std::vector<ResourceHandle> owned;
    u64 requestedBytes=0;VkQueryPool queries=VK_NULL_HANDLE;CB cb{};
    std::string directory;u32 samples=0;bool initialized=false;
    std::ofstream report;
    VmaTotalStatistics baselineMemory{};
    explicit Test(RHIDeviceBase* device):d(device),vk(static_cast<VulkanDevice*>(device)){}
    ~Test(){
        if(queries)vkDestroyQueryPool(vk->GetNativeDevice(),queries,nullptr);
        if(set!=handles::INVALID_DESCRIPTOR_SET)d->DestroyDescriptorSet(set);
        if(pipe!=handles::INVALID_PIPELINE)d->DestroyPipeline(pipe);
        if(shader!=handles::INVALID_SHADER)d->DestroyShader(shader);
        if(pipelineLayout!=handles::INVALID_PIPELINE_LAYOUT)d->DestroyPipelineLayout(pipelineLayout);
        if(layout!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)d->DestroyDescriptorSetLayout(layout);
        for(auto h:owned)d->DestroyBuffer(h);
    }
    ResourceHandle buffer(u64 n,BufferType type,GPUMemoryUsage memory,const void* data=nullptr){
        BufferDesc b{};b.size=n;b.type=type;b.memoryUsage=memory;b.name="SponzaRC.Private";
        auto h=d->CreateBuffer(b);check(h!=handles::INVALID_RESOURCE,"RC buffer allocation");owned.push_back(h);requestedBytes+=n;
        if(data)check(d->UpdateBufferData(h,data,n,0),"RC buffer upload");return h;
    }
    void initialize(u32 w,u32 h){
        directory=std::getenv("RC_OUTPUT")?std::getenv("RC_OUTPUT"):"/tmp/sponza-rc";
        std::filesystem::create_directories(directory);
        report.open(directory+"/samples.jsonl");check(bool(report),"RC open report");report.precision(12);
        cb.screen[0]=w;cb.screen[1]=h;cb.origin[3]=1.f;
        u32 offset=0;
        for(u32 l=0;l<3;++l){cb.offsets[l]=offset;cb.levels[l][0]=16u>>l;cb.levels[l][1]=8u>>l;cb.levels[l][2]=16u>>l;cb.levels[l][3]=4u<<l;offset+=(16u>>l)*(8u>>l)*(16u>>l)*(4u<<l)*(4u<<l);}
        cb.offsets[3]=offset;std::vector<std::array<float,4>> empty(offset,{0,0,0,-1});
        rays=buffer(offset*16,BufferType::Raw,GPUMemoryUsage::Dynamic,empty.data());
        merged=buffer(offset*16,BufferType::Raw,GPUMemoryUsage::Dynamic,empty.data());
        active=buffer(offset*4,BufferType::Raw,GPUMemoryUsage::Dynamic);
        constants=buffer(sizeof(cb),BufferType::Constant,GPUMemoryUsage::Dynamic);
        out=buffer(u64(w)*h*16,BufferType::Raw,GPUMemoryUsage::Static);
        readback=buffer(u64(w)*h*16,BufferType::Raw,GPUMemoryUsage::Readback);
        baselineReadback=buffer(u64(w)*h*8,BufferType::Raw,GPUMemoryUsage::Readback);
        std::vector<DescriptorSetLayoutBinding> bindings;
        for(u32 i=0;i<=9;++i){DescriptorType type=i==0?DescriptorType::UniformBuffer:
            (i==4||i==5||i==8)?DescriptorType::SampledImage:i==7?DescriptorType::SampledDepthImage:DescriptorType::StorageBuffer;
            bindings.push_back({i,type,1,ShaderStage::Compute,nullptr});}
        for(u32 i=13;i<=16;++i){bindings.push_back({i,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr});bindings.back().readonly=true;}
        layout=d->CreateDescriptorSetLayout({u32(bindings.size()),bindings.data()});check(layout!=handles::INVALID_DESCRIPTOR_SET_LAYOUT,"RC descriptor layout");
        pipelineLayout=d->CreatePipelineLayout({1,&layout});check(pipelineLayout!=handles::INVALID_PIPELINE_LAYOUT,"RC pipeline layout");
        std::ifstream f(RC_TEST_SPV,std::ios::binary|std::ios::ate);check(bool(f),"RC shader file");
        auto bytes=f.tellg();check(bytes>0&&size_t(bytes)%4==0,"RC SPIR-V size");std::vector<u32> code(size_t(bytes)/4);f.seekg(0);f.read(reinterpret_cast<char*>(code.data()),bytes);check(bool(f),"RC shader read");
        shader=d->CreateShader(code.data(),size_t(bytes),ShaderStage::Compute,"main");check(shader!=handles::INVALID_SHADER,"RC shader");
        ComputePipelineDesc p{};p.layout=pipelineLayout;p.computeShader=shader;pipe=d->CreateComputePipeline(p);check(pipe!=handles::INVALID_PIPELINE,"RC pipeline");
        set=d->CreateDescriptorSet({layout});check(set!=handles::INVALID_DESCRIPTOR_SET,"RC descriptor set");
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2;
        check(vkCreateQueryPool(vk->GetNativeDevice(),&qi,nullptr,&queries)==VK_SUCCESS,"RC timestamp pool");initialized=true;
    }
    static float half(u16 x){u32 s=u32(x&32768)<<16,e=(x>>10)&31,m=x&1023;if(e==31)return std::bit_cast<float>(s|0x7f800000u|(m<<13));if(!e){if(!m)return std::bit_cast<float>(s);int n=-14;while(!(m&1024)){m<<=1;--n;}return std::bit_cast<float>(s|(u32(n+127)<<23)|((m&1023)<<13));}return std::bit_cast<float>(s|((e+112)<<23)|(m<<13));}
    bool run(StandardRenderPipeline* pipeline,const RenderView& view,const math::v3& camera,u32 slot,u32 frame){
        auto* sc=pipeline->GetSurfaceCachePass();auto* gather=pipeline->GetGIGatherModule();
        auto& draw=nanite::GPUDrivenDrawPipeline::Get();
        check(sc&&gather,"RC source modules");
        auto baseline=gather->GetOutputTexture();u32 w=0,h=0;check(d->GetTextureSize(baseline,w,h)&&d->GetTextureFormat(baseline)==DataFormat::RGBA16_Float,"RC baseline RGBA16F");
        if(!initialized){vmaCalculateStatistics(vk->GetVmaAllocator(),&baselineMemory);initialize(w,h);}
        check(w==cb.screen[0]&&h==cb.screen[1],"RC fixed resolution");
        check(sc->PrepareGeometryConsumer(slot),"RC complete current geometry");auto geo=sc->GetGeometryWitnessView(slot);check(geo.ready,"RC geometry ready");
        cb.vp=view.GetViewProjectionMatrix();cb.inverse=rhimath::Inverse(cb.vp);
        const float nextOrigin[]={std::floor(camera.x/2.f)*2.f-8.f,std::floor(camera.y/2.f)*2.f-4.f,std::floor(camera.z/2.f)*2.f-8.f};
        bool reset=samples==0||std::memcmp(nextOrigin,cb.origin,12)!=0;
        std::memcpy(cb.origin,nextOrigin,12);
        if(reset){std::vector<std::array<float,4>> invalid(cb.offsets[3],{0,0,0,-1});check(d->UpdateBufferData(rays,invalid.data(),invalid.size()*16,0)&&d->UpdateBufferData(merged,invalid.data(),invalid.size()*16,0),"RC invalidate moved region");}
        cb.screen[2]=sc->GetCardGenerator().GetCardCount();
        auto sky=pipeline->GetDDGIPass()->GetVolumeData().SkyColor;cb.sky[0]=sky.x;cb.sky[1]=sky.y;cb.sky[2]=sky.z;
        math::v2 jitter{};check(draw.GetLastRasterJitterNDC(u32(pipeline->GetFrameCount()-1),jitter),"RC current raster jitter");cb.jitter[0]=jitter.x;cb.jitter[1]=jitter.y;
        std::vector<u32> activity(cb.offsets[3],0);u32 updated[3]{};
        for(u32 l=0;l<3;++l){u32 nx=cb.levels[l][0],ny=cb.levels[l][1],nz=cb.levels[l][2];float spacing=float(1u<<l);
            for(u32 z=0;z<nz;++z)for(u32 y=0;y<ny;++y)for(u32 x=0;x<nx;++x){
                float point[]={cb.origin[0]+(x+.5f)*spacing,cb.origin[1]+(y+.5f)*spacing,cb.origin[2]+(z+.5f)*spacing,1};float clip[4]{};
                const float* m=reinterpret_cast<const float*>(&cb.vp);for(u32 r=0;r<4;++r)for(u32 c=0;c<4;++c)clip[r]+=m[c*4+r]*point[c];
                // Coarse levels include a wider support halo for interpolation.
                bool visible=clip[3]>0&&std::abs(clip[0])<=clip[3]*(1.25f+.25f*l)+spacing&&std::abs(clip[1])<=clip[3]*(1.25f+.25f*l)+spacing&&clip[2]>=-spacing&&clip[2]<=clip[3]+spacing;
                u32 id=x+nx*(y+ny*z);activity[cb.offsets[l]+id]=visible?1:0;updated[l]+=visible;
            }}
        check(d->UpdateBufferData(active,activity.data(),activity.size()*4,0),"RC frustum upload");
        ResourceHandle buffers[]={constants,rays,merged,active,sc->GetCardDataBuffer(),out,geo.buffers[0],geo.buffers[1],geo.buffers[2],geo.buffers[3]};
        u32 binding[]={0,1,2,3,6,9,13,14,15,16};DescriptorBufferInfo bi[10]{};WriteDescriptorSet writes[14]{};
        for(u32 i=0;i<10;++i){u64 n=0;check(vk->GetBufferByteSize(buffers[i],n),"RC buffer range");bi[i]={buffers[i],0,n};writes[i]={set,binding[i],0,1,i==0?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer,nullptr,&bi[i]};}
        ResourceHandle textures[]={sc->GetLightingAtlas(slot),sc->GetDepthAtlas(),draw.GetGBufferDepthSampleable(),draw.GetGBufferNormal()};u32 tb[]={4,5,7,8};DescriptorImageInfo ti[4]{};
        for(u32 i=0;i<4;++i){ti[i]={handles::INVALID_SAMPLER,textures[i],ResourceState::ShaderResource};writes[10+i]={set,tb[i],0,1,i==2?DescriptorType::SampledDepthImage:DescriptorType::SampledImage,&ti[i],nullptr};}
        d->UpdateDescriptorSets(14,writes);
        // Phase constants are separate immutable UBOs: updating one UBO between
        // recorded dispatches would make every dispatch see the final phase.
        std::vector<ResourceHandle> phaseCB;std::vector<DescriptorSetHandle> phaseSets;
        auto command=d->CreateCommandBuffer(CommandQueueType::Graphics);auto* cmd=vk->GetCommandBuffer(command);check(cmd&&cmd->Reset()&&cmd->Begin(),"RC begin");
        cmd->MemoryBarrier(PipelineStage::AllCommands|PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::MemoryWrite|AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
        auto native=cmd->GetNativeCmdBuffer();vkCmdResetQueryPool(native,queries,0,2);vkCmdWriteTimestamp(native,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,queries,0);
        for(u32 phase:{0u,1u,2u,5u,4u,3u,6u}){
            cb.screen[3]=phase;auto c=buffer(sizeof(cb),BufferType::Constant,GPUMemoryUsage::Dynamic,&cb);phaseCB.push_back(c);
            auto ds=d->CreateDescriptorSet({layout});check(ds!=handles::INVALID_DESCRIPTOR_SET,"RC phase set");phaseSets.push_back(ds);
            for(auto& wr:writes)wr.dstSet=ds;bi[0].buffer=c;d->UpdateDescriptorSets(14,writes);
            cmd->BindComputePipeline(pipe);cmd->BindDescriptorSets(PipelineBindPoint::Compute,pipelineLayout,0,1,&ds,0,nullptr);
            u32 l=phase%3,n=phase==6?w*h:cb.levels[l][0]*cb.levels[l][1]*cb.levels[l][2]*cb.levels[l][3]*cb.levels[l][3];cmd->Dispatch((n+63)/64,1,1);
            cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
        }
        vkCmdWriteTimestamp(native,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,queries,1);
        cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
        cmd->CopyBuffer(out,readback,0,0,u64(w)*h*16);
        ResourceBarrier barrier{};barrier.resource=baseline;barrier.beforeState=ResourceState::Unknown;barrier.afterState=ResourceState::CopySource;barrier.subresource=~0u;barrier.queueFamily=~0u;cmd->InsertBarrier(&barrier,1);
        BufferTextureCopyRegion tr{};tr.imageSubresource.layerCount=1;tr.imageExtent={w,h,1};cmd->CopyTextureToBuffer(baseline,baselineReadback,&tr,1);
        barrier.beforeState=ResourceState::CopySource;barrier.afterState=ResourceState::ShaderResource;cmd->InsertBarrier(&barrier,1);
        cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
        check(cmd->End()&&cmd->Submit()&&cmd->WaitForCompletion(),"RC checked completion");d->DestroyCommandBuffer(command);
        for(auto ds:phaseSets)d->DestroyDescriptorSet(ds);
        for(auto c:phaseCB){d->DestroyBuffer(c);owned.erase(std::find(owned.begin(),owned.end(),c));requestedBytes-=sizeof(cb);}
        u64 ticks[2]{};check(vkGetQueryPoolResults(vk->GetNativeDevice(),queries,0,2,sizeof(ticks),ticks,8,VK_QUERY_RESULT_64_BIT)==VK_SUCCESS&&ticks[1]>=ticks[0],"RC timestamp results");
        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(vk->GetNativePhysicalDevice(),&props);double ms=(ticks[1]-ticks[0])*props.limits.timestampPeriod/1e6;
        auto* rc=static_cast<const float*>(d->MapBuffer(readback,0,u64(w)*h*16));auto* old=static_cast<const u16*>(d->MapBuffer(baselineReadback,0,u64(w)*h*8));check(rc&&old,"RC image maps");
        double error=0,coverage=0;u64 valid=0;for(u64 i=0;i<u64(w)*h;++i){check(std::isfinite(rc[4*i])&&std::isfinite(rc[4*i+1])&&std::isfinite(rc[4*i+2])&&std::isfinite(rc[4*i+3]),"RC finite output");coverage+=rc[4*i+3];if(rc[4*i+3]>.99f){++valid;for(u32 k=0;k<3;++k){double delta=rc[4*i+k]-half(old[4*i+k]);error+=delta*delta;}}}
        std::string prefix=directory+"/frame-"+std::to_string(frame);
        auto save=[&](const std::string& path,const void* data,u64 n){std::ofstream f(path,std::ios::binary);f.write(static_cast<const char*>(data),n);check(bool(f),"RC write output");};
        save(prefix+"-rc.rgba32f",rc,u64(w)*h*16);save(prefix+"-gi.rgba16f",old,u64(w)*h*8);
        d->UnmapBuffer(readback);d->UnmapBuffer(baselineReadback);
        double baselineMs=0;const auto& stats=pipeline->GetStats();for(const auto& [name,time]:stats.passExecutionTimes)if(name.find("DDGI")!=std::string::npos||name.find("ScreenProbe")!=std::string::npos||name.find("GIGather")!=std::string::npos)baselineMs+=time;
        VmaTotalStatistics memory{};vmaCalculateStatistics(vk->GetVmaAllocator(),&memory);
        report<<"{\"frame\":"<<frame<<",\"width\":"<<w<<",\"height\":"<<h<<",\"rc_gpu_ms\":"<<ms<<",\"baseline_selected_pass_ms\":"<<baselineMs<<",\"baseline_timestamp_fresh\":"<<(stats.timestampResultsFresh?"true":"false")<<",\"baseline_timestamp_sequence\":"<<stats.timestampResolvedSequence<<",\"baseline_vma_allocated_bytes\":"<<baselineMemory.total.statistics.allocationBytes<<",\"with_rc_vma_allocated_bytes\":"<<memory.total.statistics.allocationBytes<<",\"baseline_vma_reserved_bytes\":"<<baselineMemory.total.statistics.blockBytes<<",\"with_rc_vma_reserved_bytes\":"<<memory.total.statistics.blockBytes<<",\"rc_requested_bytes\":"<<requestedBytes<<",\"temporary_phase_ubo_bytes\":"<<7*sizeof(cb)<<",\"shared_geometry_bytes\":"<<geo.sizes[0]+geo.sizes[1]+geo.sizes[2]+geo.sizes[3]<<",\"coverage\":"<<coverage/(w*h)<<",\"fully_known_pixels\":"<<valid<<",\"rmse_known_only\":"<<(valid?std::sqrt(error/(valid*3)):-1)<<",\"region_reset\":"<<(reset?"true":"false")<<",\"updated_probes\":["<<updated[0]<<','<<updated[1]<<','<<updated[2]<<"],\"memory_scope\":\"requested resource sizes, not driver residency\",\"source_scope\":\"current SC radiance including existing GI; opaque geometry; constant DDGI sky; no independent multi-bounce solve\"}\n";report.flush();check(bool(report),"RC report write");++samples;
        PRIMAL_LOG_STREAM(Info,"Render")<<"[SPONZA_RC] frame="<<frame<<" gpu_ms="<<ms<<" coverage="<<coverage/(w*h)<<" output="<<directory;
        return samples>=8;
    }
};
}
