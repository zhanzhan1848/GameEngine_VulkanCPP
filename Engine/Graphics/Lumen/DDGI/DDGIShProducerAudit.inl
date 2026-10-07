// Private Vulkan producer audit. All shadow writes bind private clones.
struct DDGIShProducerAudit {
    RHIDeviceBase* device{};
    std::string prefix;
    u32 probeCount{}, rays{}, lastFrame{}, lastSlot{};
    bool failed{}, flushed{};
    std::vector<ResourceHandle> buffers;
    ResourceHandle shadowSH{}, shadowConf{}, shadowOpen{}, probeAudit{}, rayAudit{};
    ResourceHandle actualAfter[3]{}, before[9]{};
    u64 sourceBytes[9]{};
    ResourceHandle lastSources[9]{};
    ShaderHandle shaders[2]{handles::INVALID_SHADER,handles::INVALID_SHADER};
    PipelineHandle pipelines[2]{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
    DescriptorSetLayoutHandle setLayout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    PipelineLayoutHandle layout{handles::INVALID_PIPELINE_LAYOUT};
    DescriptorSetHandle sets[2][3]{};
    DDGIShProducerAudit(RHIDeviceBase* d):device(d) {
        for(auto& row:sets) for(auto& v:row)v=handles::INVALID_DESCRIPTOR_SET;
        for(auto& v:actualAfter)v=handles::INVALID_RESOURCE;
        for(auto& v:before)v=handles::INVALID_RESOURCE;
    }
    ~DDGIShProducerAudit() {
        for(auto& row:sets)for(auto v:row)if(v!=handles::INVALID_DESCRIPTOR_SET)device->DestroyDescriptorSet(v);
        for(auto v:pipelines)if(v!=handles::INVALID_PIPELINE)device->DestroyPipeline(v);
        if(layout!=handles::INVALID_PIPELINE_LAYOUT)device->DestroyPipelineLayout(layout);
        if(setLayout!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device->DestroyDescriptorSetLayout(setLayout);
        for(auto v:shaders)if(v!=handles::INVALID_SHADER)device->DestroyShader(v);
        for(auto v:buffers)if(v!=handles::INVALID_RESOURCE)device->DestroyBuffer(v);
    }
    static bool Requested() {
        const char* p=std::getenv("DDGI_SH_PRODUCER_AUDIT_PREFIX");const char* d=std::getenv("DDGI_DUMP");
        return p&&*p&&d&&std::strcmp(d,"2")==0;
    }
    bool Reject(const char* reason) {
        failed=true;PRIMAL_LOG_STREAM(Error,"Render")<<"[DDGI_SH_PRODUCER_AUDIT] rejected: "<<reason<<std::endl;return false;
    }
    ResourceHandle Make(u64 size,const char* name) {
        BufferDesc desc{};desc.size=size;desc.type=BufferType::Structured;
        desc.usage=desc.memoryUsage=GPUMemoryUsage::Dynamic;
        desc.bindFlags=static_cast<u32>(BufferUsageFlags::Storage|BufferUsageFlags::TransferSrc|BufferUsageFlags::TransferDst);
        desc.name=name;auto h=device->CreateBuffer(desc);buffers.push_back(h);return h;
    }
    bool Init(u32 count,u32 rayCount,const ResourceHandle* src) {
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
        if(device->GetPlatform()!=RHIPlatform::Vulkan||!Requested()||!count||count>4096u||rayCount!=128u)return Reject("requires bounded Vulkan DUMP2 producer source");
        prefix=std::getenv("DDGI_SH_PRODUCER_AUDIT_PREFIX");probeCount=count;rays=rayCount;
        auto* vk=static_cast<VulkanDevice*>(device);
        for(u32 i=0;i<9;i++)if(src[i]==handles::INVALID_RESOURCE||!vk->GetBufferByteSize(src[i],sourceBytes[i]))return Reject("actual input extent unavailable");
        const u64 cap=u64(count)*rays;
        if(sourceBytes[1]<sizeof(DDGIVolumeData)||sourceBytes[1]>4096||sourceBytes[2]!=cap*16||sourceBytes[3]!=u64(count)*9*16||sourceBytes[4]<u64(count)*4||sourceBytes[5]!=u64(count)*16||sourceBytes[6]!=u64(count)*8||sourceBytes[7]!=u64(count)*16||sourceBytes[8]!=cap*27*16)return Reject("actual producer extent mismatch");
        shadowSH=Make(sourceBytes[3],"DDGIShProducerPrivateSH");shadowConf=Make(sourceBytes[5],"DDGIShProducerPrivateConfidence");shadowOpen=Make(sourceBytes[6],"DDGIShProducerPrivateOpenSky");
        probeAudit=Make(u64(count)*80*16,"DDGIShProducerProbeAudit");rayAudit=Make(cap*10*16,"DDGIShProducerRayAudit");
        for(u32 i=0;i<3;i++)actualAfter[i]=Make(sourceBytes[i==0?3:i==1?5:6],"DDGIShProducerActualAfter");
        for(u32 i=0;i<9;i++)before[i]=Make(sourceBytes[i],"DDGIShProducerBefore");
        for(auto h:buffers)if(h==handles::INVALID_RESOURCE)return Reject("private allocation failed");
        for(auto h:{probeAudit,rayAudit}) {
            u64 size{};if(!vk->GetBufferByteSize(h,size))return Reject("private audit extent missing");
            void* p=device->MapBuffer(h);if(!p)return Reject("private initialization Map failed");std::memset(p,0,static_cast<size_t>(size));device->UnmapBuffer(h);
        }
        const char* paths[2]={"Engine/Graphics/Vulkan/Shaders/Lumen/DDGIShProducerAudit_shadow.comp.spv","Engine/Graphics/Vulkan/Shaders/Lumen/DDGIShProducerAudit_compare.comp.spv"};
        const char* entries[2]={"ddgi_sh_producer_shadow","ddgi_sh_producer_compare"};
        for(u32 i=0;i<2;i++) {
            std::ifstream f(paths[i],std::ios::binary|std::ios::ate);if(!f)return Reject("private module missing");
            auto size=f.tellg();if(size<=0||u64(size)%4)return Reject("private module extent invalid");
            std::vector<u8> b(static_cast<size_t>(size));f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),size);if(!f)return Reject("private module read failed");
            shaders[i]=device->CreateShader(b.data(),b.size(),ShaderStage::Compute,entries[i]);
        }
        DescriptorSetLayoutBinding binding[15]{};
        for(u32 i=0;i<15;i++)binding[i]={i,i<2?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};
        DescriptorSetLayoutDesc sd{15,binding};setLayout=device->CreateDescriptorSetLayout(sd);
        PipelineLayoutDesc ld{};ld.setLayoutCount=1;ld.setLayouts=&setLayout;layout=device->CreatePipelineLayout(ld);
        for(u32 i=0;i<2;i++) {
            ComputePipelineDesc pd{};pd.computeShader=shaders[i];pd.layout=layout;pipelines[i]=device->CreateComputePipeline(pd);
            for(u32 j=0;j<3;j++){DescriptorSetDesc ds{};ds.layout=setLayout;sets[i][j]=device->CreateDescriptorSet(ds);}
        }
        if(setLayout==handles::INVALID_DESCRIPTOR_SET_LAYOUT||layout==handles::INVALID_PIPELINE_LAYOUT)return Reject("private layout creation failed");
        for(u32 i=0;i<2;i++) {
            if(shaders[i]==handles::INVALID_SHADER||pipelines[i]==handles::INVALID_PIPELINE)return Reject("private pipeline creation failed");
            for(u32 j=0;j<3;j++)if(sets[i][j]==handles::INVALID_DESCRIPTOR_SET)return Reject("private descriptor creation failed");
        }
        return true;
#else
        return Reject("Vulkan build required");
#endif
    }
    void Bind(RHICommandBuffer* cmd,u32 variant,u32 slot,const ResourceHandle* src,ResourceHandle actualSH) {
        ResourceHandle hs[15]={src[0],src[1],src[2],src[3],shadowSH,src[4],shadowConf,shadowOpen,probeAudit,rayAudit,src[7],src[8],actualSH,src[5],src[6]};
        u64 sizes[15]={sourceBytes[0],sourceBytes[1],sourceBytes[2],sourceBytes[3],sourceBytes[3],sourceBytes[4],sourceBytes[5],sourceBytes[6],u64(probeCount)*80*16,u64(probeCount)*rays*10*16,sourceBytes[7],sourceBytes[8],sourceBytes[3],sourceBytes[5],sourceBytes[6]};
        DescriptorBufferInfo infos[15]{};WriteDescriptorSet writes[15]{};
        for(u32 i=0;i<15;i++){infos[i]={hs[i],0,sizes[i]};writes[i]={sets[variant][slot],i,0,1,i<2?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer,nullptr,&infos[i]};}
        device->UpdateDescriptorSets(15,writes);cmd->BindComputePipeline(pipelines[variant]);cmd->BindDescriptorSets(PipelineBindPoint::Compute,layout,0,1,&sets[variant][slot],0,nullptr);
    }
    bool Before(RHICommandBuffer* cmd,u32 frame,u32 slot,u32 count,u32 rc,u32 updates,const ResourceHandle* src,ResourceHandle actualSH,PipelineHandle productionPipeline,PipelineLayoutHandle productionLayout,DescriptorSetHandle productionSet) {
        if(failed||!cmd||slot>2||updates>count)return Reject("invalid producer encoding state");
        if(!probeCount&&!Init(count,rc,src))return false;
        if(count!=probeCount||rc!=rays)return Reject("reconfigured producer dimensions");
        if(flushed)return true;
        lastFrame=frame;lastSlot=slot;for(u32 i=0;i<9;i++)lastSources[i]=src[i];
        cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::AllCommands|PipelineStage::ComputeShader|PipelineStage::Transfer,PipelineStage::Transfer,AccessFlag::HostWrite|AccessFlag::MemoryWrite|AccessFlag::ShaderWrite|AccessFlag::TransferWrite,AccessFlag::TransferRead|AccessFlag::TransferWrite);
        cmd->CopyBuffer(actualSH,shadowSH,0,0,sourceBytes[3]);cmd->CopyBuffer(src[5],shadowConf,0,0,sourceBytes[5]);cmd->CopyBuffer(src[6],shadowOpen,0,0,sourceBytes[6]);
        for(u32 i=0;i<9;i++)cmd->CopyBuffer(src[i],before[i],0,0,sourceBytes[i]);
        cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::Transfer|PipelineStage::ComputeShader|PipelineStage::AllCommands,PipelineStage::ComputeShader,AccessFlag::HostWrite|AccessFlag::TransferWrite|AccessFlag::ShaderWrite|AccessFlag::MemoryWrite,AccessFlag::ShaderRead|AccessFlag::UniformRead|AccessFlag::ShaderWrite);
        Bind(cmd,0,slot,src,actualSH);cmd->Dispatch((updates+63)/64,1,1);
        cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderRead|AccessFlag::ShaderWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
        std::ofstream log(prefix+".encodings.jsonl",std::ios::app);if(!log)return Reject("encoding metadata open failed");
        log<<"{\"production_pipeline\":"<<productionPipeline<<",\"production_layout\":"<<productionLayout<<",\"production_set\":"<<productionSet<<",\"private_handles\":["<<shadowSH<<','<<shadowConf<<','<<shadowOpen<<','<<probeAudit<<','<<rayAudit<<"],\"frame\":"<<frame<<",\"slot\":"<<slot<<",\"updates\":"<<updates<<",\"production_output\":"<<actualSH<<",\"shadow_pipeline\":"<<pipelines[0]<<",\"compare_pipeline\":"<<pipelines[1]<<",\"layout\":"<<layout<<",\"shadow_set\":"<<sets[0][slot]<<",\"compare_set\":"<<sets[1][slot]<<",\"source_handles\":[";
        for(u32 i=0;i<9;i++)log<<(i?",":"")<<src[i];log<<"]}\n";return bool(log)||Reject("encoding metadata write failed");
    }
    void After(RHICommandBuffer* cmd,u32 slot,u32 updates,const ResourceHandle* src,ResourceHandle actualSH) {
        if(failed||flushed)return;
        cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
        Bind(cmd,1,slot,src,actualSH);cmd->Dispatch((updates+63)/64,1,1);
        cmd->MemoryBarrier(PipelineStage::ComputeShader|PipelineStage::Transfer,PipelineStage::Transfer,AccessFlag::ShaderRead|AccessFlag::ShaderWrite|AccessFlag::TransferWrite,AccessFlag::TransferRead|AccessFlag::TransferWrite);
        cmd->CopyBuffer(actualSH,actualAfter[0],0,0,sourceBytes[3]);cmd->CopyBuffer(src[5],actualAfter[1],0,0,sourceBytes[5]);cmd->CopyBuffer(src[6],actualAfter[2],0,0,sourceBytes[6]);
        cmd->MemoryBarrier(PipelineStage::Transfer|PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::TransferRead|AccessFlag::TransferWrite|AccessFlag::ShaderWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
    }
    bool Flush(u32 frame) {
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
        if(flushed)return !failed;
        const char* requested=std::getenv("DDGI_SH_PRODUCER_AUDIT_FRAME");if(!requested)return Reject("requested dump frame missing");
        const u32 target=static_cast<u32>(std::strtoul(requested,nullptr,10));if(frame<target)return !failed;
        if(failed||frame!=target||lastFrame!=frame)return Reject("actual producer dump frame missing");
        auto* vk=static_cast<VulkanDevice*>(device);std::vector<ResourceHandle> src={probeAudit,rayAudit,shadowSH,shadowConf,shadowOpen,actualAfter[0],actualAfter[1],actualAfter[2]};
        const char* suffix[]={".probe.raw",".ray.raw",".shadow-sh.raw",".shadow-confidence.raw",".shadow-opensky.raw",".actual-sh.raw",".actual-confidence.raw",".actual-opensky.raw",".before-global.raw",".before-volume.raw",".before-rays.raw",".before-history.raw",".before-list.raw",".before-confidence.raw",".before-opensky.raw",".before-origins.raw",".before-hn.raw"};
        for(auto h:before)src.push_back(h);
        std::vector<ResourceHandle> reads;std::vector<u64> sizes;bool okay=true;
        for(auto h:src){u64 size{};if(!vk->GetBufferByteSize(h,size)){okay=false;break;}sizes.push_back(size);BufferDesc bd{};bd.size=size;bd.type=BufferType::Structured;bd.usage=bd.memoryUsage=GPUMemoryUsage::Readback;bd.bindFlags=static_cast<u32>(BufferUsageFlags::TransferDst);auto rb=device->CreateBuffer(bd);reads.push_back(rb);if(rb==handles::INVALID_RESOURCE){okay=false;break;}}
        auto command=device->CreateCommandBuffer(CommandQueueType::Graphics);auto* cmd=vk->GetCommandBuffer(command);
        okay=okay&&cmd&&cmd->Reset()&&cmd->Begin();
        if(okay){cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::AllCommands|PipelineStage::ComputeShader|PipelineStage::Transfer,PipelineStage::Transfer,AccessFlag::HostWrite|AccessFlag::MemoryWrite|AccessFlag::ShaderWrite|AccessFlag::TransferWrite,AccessFlag::TransferRead);for(u32 i=0;i<src.size();i++)cmd->CopyBuffer(src[i],reads[i],0,0,sizes[i]);cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);okay=cmd->End()&&cmd->Submit()&&cmd->WaitForCompletion();}
        if(okay)for(u32 i=0;i<src.size();i++){const void* p=device->MapBuffer(reads[i]);if(!p){okay=false;break;}std::ofstream f(prefix+suffix[i],std::ios::binary|std::ios::trunc);if(f)f.write(static_cast<const char*>(p),static_cast<std::streamsize>(sizes[i]));device->UnmapBuffer(reads[i]);if(!f){okay=false;break;}}
        if(command!=handles::INVALID_COMMAND_BUFFER)device->DestroyCommandBuffer(command);for(auto h:reads)if(h!=handles::INVALID_RESOURCE)device->DestroyBuffer(h);
        std::ofstream meta(prefix+".json",std::ios::trunc);meta<<"{\"schema\":\"ddgi_sh_producer_v1\",\"complete\":"<<(okay?"true":"false")<<",\"frame\":"<<frame<<",\"probe_count\":"<<probeCount<<",\"rays\":"<<rays<<",\"probe_vec4_stride\":80,\"ray_vec4_stride\":10,\"readback_completed\":"<<(okay?"true":"false")<<",\"begin_end_submit_checked\":true,\"completion_checked\":true,\"explicit_buffer_dependencies\":true,\"map_after_completion\":true,\"dispatch_before_inputs\":true,\"private_shadow_outputs_only\":true,\"production_candidate\":false}\n";
        okay=okay&&bool(meta);flushed=okay;return okay||Reject("producer readback failed; partial raw retained");
#else
        return Reject("Vulkan required");
#endif
    }
};
