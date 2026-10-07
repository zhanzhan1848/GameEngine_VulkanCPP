// Included in SurfaceCachePass.cpp. Resources are private to each of three
// frame slots. Runtime reuse requires host fence completion; diagnostic
// snapshots additionally require explicit completion before CPU readback.
void SurfaceCachePass::ReleaseBoundaryLightingProof() {
    auto& p=BoundaryProof();
    if(device_){
        if(p.timings!=handles::INVALID_QUERY_POOL){
            // Diagnostic query-pool destruction is synchronous in Vulkan.
            device_->WaitIdle();device_->DestroyQueryPool(p.timings);
        }
        for(auto b:{p.points,p.visibility,p.results,p.staging,p.cache,p.cache_staging,p.mixed_index})if(b!=handles::INVALID_RESOURCE)device_->DestroyBuffer(b);
        for(auto b:p.radio_buffers)if(b!=handles::INVALID_RESOURCE)device_->DestroyBuffer(b);
        for(u32 i=0;i<5;++i){if(p.pipelines[i]!=handles::INVALID_PIPELINE)device_->DestroyPipeline(p.pipelines[i]);
            if(p.sets[i]!=handles::INVALID_DESCRIPTOR_SET)device_->DestroyDescriptorSet(p.sets[i]);
            if(p.layouts[i]!=handles::INVALID_PIPELINE_LAYOUT)device_->DestroyPipelineLayout(p.layouts[i]);
            if(p.set_layouts[i]!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device_->DestroyDescriptorSetLayout(p.set_layouts[i]);}
    }
    p={};
}
void SurfaceCachePass::BoundaryTimestamp(RHICommandBuffer* cmd,u32 query){
    const char* flag=std::getenv("SC_BOUNDARY_PROFILE");
    if(!boundary_continuous_requested_ || !flag || std::strcmp(flag,"1") || query>=12)return;
    auto& p=BoundaryProof();
    if(p.timing_frame!=p.frame){
        if(p.timings==handles::INVALID_QUERY_POOL){QueryPoolDesc d{};d.queryCount=12;p.timings=device_->CreateQueryPool(d);}
        p.timing_mask=0;p.timing_frame=p.frame;
        if(p.timings!=handles::INVALID_QUERY_POOL)cmd->ResetQueryPool(p.timings,0,12);
    }
    if(p.timings==handles::INVALID_QUERY_POOL)return;
    cmd->WriteTimestamp(p.timings,query);p.timing_mask|=1u<<query;
}
bool SurfaceCachePass::BoundaryLightingProofFrame(u32 frame){
    if(const char* sequence=std::getenv("SC_BOUNDARY_CACHE_PROOF_FRAMES");sequence && *sequence){
        // Bounded, strictly increasing frame list; reject malformed schedules.
        u32 count=0,previous=0;bool match=false;const char* at=sequence;
        while(*at){char* end=nullptr;const auto value=std::strtoul(at,&end,10);
            if(end==at || value>UINT32_MAX || !value || value<=previous || ++count>32 || (*end && *end!=','))return false;
            previous=u32(value);match=match || frame==previous;if(!*end)break;at=end+1;if(!*at)return false;}
        return match;
    }
    const char* requested=std::getenv("SC_BOUNDARY_LIGHT_PROOF_FRAME");
    return frame==u32(requested?std::strtoul(requested,nullptr,10):20);
}
bool SurfaceCachePass::BoundaryContinuousFrame(u32 frame){
    const char* enabled=std::getenv("SC_BOUNDARY_CONTINUOUS");
    if(!enabled || std::strcmp(enabled,"1"))return false;
    auto parse=[](const char* name,u32 fallback,u32& output){const char* value=std::getenv(name);
        if(!value){output=fallback;return true;}char* end=nullptr;const auto n=std::strtoul(value,&end,10);
        if(end==value || *end || value[0]=='-' || n>UINT32_MAX)return false;output=u32(n);return true;};
    u32 from,to;if(!parse("SC_BOUNDARY_CONTINUOUS_FROM",120,from) || !parse("SC_BOUNDARY_CONTINUOUS_TO",UINT32_MAX,to) || from>to)return false;
    return frame>=from && frame<=to;
}
bool SurfaceCachePass::PrepareBoundaryLightingProof(u32 frame,u32 slot) {
    if(slot>=3)return false;boundary_proof_slot_=slot;
    const char* prefix=std::getenv("SC_BOUNDARY_LIGHT_PROOF_PREFIX");
    auto& p=BoundaryProof();
    const bool continuous=boundary_continuous_requested_ && BoundaryContinuousFrame(frame);
    const bool readback=prefix && prefix[0] && BoundaryLightingProofFrame(frame);
    if(device_->GetPlatform()!=RHIPlatform::Vulkan || (!continuous && !readback))return false;
    if(continuous && !device_->FrameSlotCompletions().CanReuse(slot,p.completion_serial)){
        PRIMAL_LOG_STREAM(Error,"Render")<<"[SC_BOUNDARY_CONTINUOUS] slot completion missing slot="<<slot<<std::endl;return false;}
    const u64 completion=continuous?device_->FrameSlotCompletions().Serial(slot):0;
    // Read only the bank whose host fence has completed; never trust a failed query.
    if(continuous && p.timings!=handles::INVALID_QUERY_POOL && p.timing_mask==4095){
        u64 ticks[12]{};const double period=device_->GetTimestampPeriod();
        bool valid=device_->GetQueryPoolResults(p.timings,0,12,ticks,sizeof(u64)) && std::isfinite(period) && period>0;
        for(u32 i=0;i<12;i+=2)valid=valid && ticks[i]>0 && ticks[i+1]>=ticks[i];
        if(valid){const char* names[]={"shadow","base","trace","radio_resolve","compose","main"};
            for(u32 i=0;i<6;++i)PRIMAL_LOG_STREAM(Info,"Render")<<"[SC_BOUNDARY_PROFILE] frame="<<p.frame<<" slot="<<slot<<" stage="<<names[i]<<" ms="<<(ticks[i*2+1]-ticks[i*2])*period/1e6<<std::endl;
        }else PRIMAL_LOG_STREAM(Warn,"Render")<<"[SC_BOUNDARY_PROFILE] rejected frame="<<p.frame<<" slot="<<slot<<std::endl;
        p.timing_mask=0;
    }

    if(p.recorded && p.frame!=frame && !continuous && !p.dumped)return false;
    if(p.failed || (p.recorded && p.frame==frame))return false;
    const auto previousKey=p.cache_key;const u32 previousCount=p.count;const bool refresh=p.recorded;
    const u32 epoch=card_generator_.GetAllocationEpoch();
    auto fail=[&](){p.failed=true;PRIMAL_LOG_STREAM(Error,"Render")<<"[SC_BOUNDARY_LIGHT_PROOF] prepare failed"<<std::endl;return false;};
    if(!StaticPagesEnabled() || !geometry_first_requested_)return fail();
    if(const char* radius=std::getenv("PRIMAL_SC_GATHER_RADIUS");radius && std::atoi(radius)>=2){
        PRIMAL_LOG_STREAM(Error,"Render")<<"[SC_BOUNDARY_LIGHT_PROOF] only the default 8-probe gather is supported"<<std::endl;
        return fail();
    }
    const auto& cards=card_generator_.GetCards();const auto& refs=card_generator_.GetStaticRefs();
    std::vector<BoundaryLightPoint> points;
    u64 content=14695981039346656037ull;
    auto hash=[&](const void* data,size_t size){const auto* b=static_cast<const unsigned char*>(data);for(size_t j=0;j<size;++j)content=(content^b[j])*1099511628211ull;};
    for(u32 ci=0;ci<cards.size() && ci<refs.size();++ci){
        if((cards[ci]._pad1[1]&1u)==0)continue;
        const auto* mesh=BoundarySamplesForStaticSlot(refs[ci].mesh_slot);if(!mesh)continue;
        hash(&mesh->geometry_digest,sizeof(mesh->geometry_digest));hash(&mesh->layout_digest,sizeof(mesh->layout_digest));hash(&mesh->material_digest,sizeof(mesh->material_digest));
        hash(&cards[ci],sizeof(cards[ci]));hash(&refs[ci].world,sizeof(refs[ci].world));
        // SCB1 currently contains opaque diffuse attributes, no emission.
        if(cards[ci].mesh_instance_id<kMaxEmissiveInstances && instance_emissive_cpu_[cards[ci].mesh_instance_id].w>.5f){
            const auto& e=instance_emissive_cpu_[cards[ci].mesh_instance_id];if(e.x!=0 || e.y!=0 || e.z!=0)return fail();}
        if(std::getenv("PRIMAL_SC_EMISSIVE"))return fail();
        if(!AppendBoundaryLightPoints(*mesh,cards[ci],refs[ci].world,ci,epoch,frame,points))return fail();
    }
    if(points.empty())return fail();
    BoundaryRadianceCache::Key key{};
    const char* sequence=std::getenv("SC_BOUNDARY_CACHE_PROOF_FRAMES");
    if(continuous || (sequence && *sequence)){
        const char* radio=std::getenv("SC_BOUNDARY_RADIOSITY_PROOF");if(!continuous && (!radio || std::strcmp(radio,"1")))return fail();
        for(auto point:points){point.identity[3]=0;hash(&point,sizeof(point));}
        key={epoch,geometry_epoch_,content};
        if(readback && !boundary_radiance_cache_.Begin(key,frame,points))return fail();
        if(!readback)boundary_radiance_cache_.Clear();
    }
    const bool reuse=refresh && key.content && key==previousKey && previousCount==points.size();
    if(refresh && !reuse)ReleaseBoundaryLightingProof();
    p.frame=frame;p.epoch=epoch;p.count=u32(points.size());p.cache_key=key;p.cpu_points=points;p.reused=reuse;
    p.readback_requested=readback;p.completion_serial=completion;
    p.bytes=p.radio_bytes=0;p.recorded=p.dumped=p.shadowed=p.radio_traced=p.radio_recorded=p.cache_recorded=p.consumer_recorded=false;
    if(const char* automatic=std::getenv("SC_BOUNDARY_AUTO_INDEX");boundary_continuous_requested_ || (automatic && std::strcmp(automatic,"1")==0)){
        if(boundary_atlas_epoch_!=epoch || static_composite_epoch_!=epoch || card_generator_.IsPageTableEnabled())return fail();
        if(p.mixed_index==handles::INVALID_RESOURCE){
            if(!BuildBoundaryMixedIndex(u32(cards.size()),epoch,boundary_atlas_groups_,points,p.mixed_words))return fail();
            BufferDesc d{};d.size=p.mixed_words.size()*4;d.type=BufferType::Structured;d.usage=d.memoryUsage=GPUMemoryUsage::Dynamic;
            p.mixed_index=device_->CreateBuffer(d);if(p.mixed_index==handles::INVALID_RESOURCE ||
                !device_->UpdateBufferData(p.mixed_index,p.mixed_words.data(),d.size,0))return fail();
        }
    }
    if(reuse){
        // Runtime reuse requires a new successful host slot-fence serial.
        // Diagnostic-only reuse retains its submit/readback completion guard.
        std::vector<float> invalid(p.count,-1.f);
        if(!device_->UpdateBufferData(p.points,points.data(),points.size()*64,0) ||
           !device_->UpdateBufferData(p.visibility,invalid.data(),invalid.size()*4,0))return fail();
        PRIMAL_LOG_STREAM(Info,"Render")<<"[SC_BOUNDARY_CACHE] reused bank frame="<<frame<<std::endl;
        return true;
    }
    auto buffer=[&](u64 size,GPUMemoryUsage usage,const void* data){BufferDesc d{};d.size=size;d.type=BufferType::Structured;
        d.memoryUsage=d.usage=usage;auto b=device_->CreateBuffer(d);
        if(b!=handles::INVALID_RESOURCE && data && !device_->UpdateBufferData(b,data,size,0)){device_->DestroyBuffer(b);return handles::INVALID_RESOURCE;}return b;};
    std::vector<float> invalid(p.count,-1.f);
    p.points=buffer(points.size()*sizeof(BoundaryLightPoint),GPUMemoryUsage::Dynamic,points.data());
    p.visibility=buffer(invalid.size()*4,GPUMemoryUsage::Dynamic,invalid.data());
    // Eval writes every entry (including invalid zero) before any consumer.
    p.results=buffer(u64(p.count)*sizeof(BoundaryLightResult),GPUMemoryUsage::Static,nullptr);
    if(p.points==handles::INVALID_RESOURCE || p.visibility==handles::INVALID_RESOURCE || p.results==handles::INVALID_RESOURCE)return fail();
    for(u32 pass=0;pass<2;++pass){std::vector<DescriptorSetLayoutBinding> bindings;
        const u32 n=pass?19:14;
        for(u32 i=0;i<n;++i){DescriptorType type=DescriptorType::StorageBuffer;
            if(!pass){if(i<3 || (i>=9 && i<=12))type=DescriptorType::SampledImage;
                if(i==3 || i==4)type=DescriptorType::StorageImage;if(i==5)type=DescriptorType::UniformBuffer;}
            else {if(i<=3 || i==8 || i==11 || i==12 || i==16)type=DescriptorType::StorageImage;if(i==4)type=DescriptorType::UniformBuffer;}
            DescriptorSetLayoutBinding b{i,type,1,ShaderStage::Compute,nullptr};b.is3D=!pass && i<3;
            b.readonly=type==DescriptorType::StorageBuffer && (pass?i!=18:i!=8);bindings.push_back(b);}
        p.set_layouts[pass]=device_->CreateDescriptorSetLayout({n,bindings.data()});if(p.set_layouts[pass]==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return fail();
        PipelineLayoutDesc ld{};ld.setLayoutCount=1;ld.setLayouts=&p.set_layouts[pass];p.layouts[pass]=device_->CreatePipelineLayout(ld);
        if(p.layouts[pass]==handles::INVALID_PIPELINE_LAYOUT)return fail();p.sets[pass]=device_->CreateDescriptorSet({p.set_layouts[pass]});
        if(p.sets[pass]==handles::INVALID_DESCRIPTOR_SET)return fail();
        auto bytes=LoadShaderBytecode(device_,pass?"SurfaceCacheLightEval_boundary":"SurfaceCacheShadow_boundary");if(bytes.empty())return fail();
        auto shader=device_->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,pass?"surface_cache_light_eval":"surface_cache_shadow");
        if(shader==handles::INVALID_SHADER)return fail();ComputePipelineDesc desc{};desc.computeShader=shader;desc.layout=p.layouts[pass];
        p.pipelines[pass]=device_->CreateComputePipeline(desc);device_->DestroyShader(shader);if(p.pipelines[pass]==handles::INVALID_PIPELINE)return fail();
    }
    PRIMAL_LOG_STREAM(Info,"Render")<<"[SC_BOUNDARY_LIGHT_PROOF] points="<<p.count<<" epoch="<<p.epoch<<" frame="<<frame<<std::endl;
    return true;
}
bool SurfaceCachePass::ReadbackBoundaryLightingProof(const char* prefix) {
    auto& p=BoundaryProof();
    if(!p.readback_requested || !prefix || !prefix[0] || p.failed || !p.recorded || p.dumped || p.epoch!=card_generator_.GetAllocationEpoch())return false;
    const char* radiosity=std::getenv("SC_BOUNDARY_RADIOSITY_PROOF");
    const bool wantsRadio=boundary_continuous_requested_ || (radiosity && std::strcmp(radiosity,"1")==0);
    if(wantsRadio && !p.radio_recorded)return false;
    device_->WaitIdle(); // Opt-in test host only: Map strictly after completion.
    const auto* data=static_cast<const char*>(device_->MapBuffer(p.staging));if(!data)return false;
    const char* names[]={"points","results","visibility","params","shadow-params","lights","ddgi-sh","ddgi-depth","ddgi-opensky","cards"};
    bool ok=true;
    for(u32 i=0;i<10;++i)if(p.sizes[i]){std::ofstream file(std::string(prefix)+"."+names[i]+".bin",std::ios::binary|std::ios::trunc);
        file.write(data+p.offsets[i],p.sizes[i]);ok=ok && bool(file);}
    device_->UnmapBuffer(p.staging);
    if(wantsRadio){const auto* radio=static_cast<const char*>(device_->MapBuffer(p.radio_buffers[5]));if(!radio)return false;
        const char* names[]={"radio-hits","radio-triangles","radio-rays","radiosity","radio-contributions","radio-trace-params","radio-resolve-params","owner-lookups"};
        for(u32 i=0;i<8;++i){std::ofstream file(std::string(prefix)+"."+names[i]+".bin",std::ios::binary|std::ios::trunc);
            file.write(radio+p.radio_offsets[i],p.radio_sizes[i]);ok=ok && bool(file);}
        device_->UnmapBuffer(p.radio_buffers[5]);
    }
    if(p.cache_recorded){
        const auto* data=static_cast<const BoundaryRadianceEntry*>(device_->MapBuffer(p.cache_staging));if(!data)return false;
        ok=boundary_radiance_cache_.Publish(p.cache_key,p.frame,{data,p.count}) && ok;
        std::ofstream file(std::string(prefix)+".cache.bin",std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<const char*>(data),u64(p.count)*64);ok=bool(file)&&ok;
        device_->UnmapBuffer(p.cache_staging);
    }
    if(p.cache_recorded && !ReadbackBoundaryLookupProof(prefix))ok=false;
    std::ofstream meta(std::string(prefix)+".json");meta<<"{\"frame\":"<<p.frame<<",\"epoch\":"<<p.epoch<<",\"points\":"<<p.count
        <<",\"completion_serial\":"<<p.completion_serial<<",\"continuous_refresh\":"<<(boundary_continuous_requested_?"true":"false")<<",\"main_consumer_recorded\":"<<(p.consumer_recorded?"true":"false")<<",\"frame_slot\":"<<boundary_proof_slot_<<",\"automatic_mixed_index\":"<<(p.mixed_index!=handles::INVALID_RESOURCE?"true":"false")<<",\"mixed_index_buffer\":"<<p.mixed_index<<",\"resource_ids\":["<<p.points<<","<<p.cache<<","<<p.pipelines[2]<<","<<p.pipelines[3]<<","<<p.pipelines[4]<<"]"<<",\"resources_reused\":"<<(p.reused?"true":"false")<<",\"cache_recorded\":"<<(p.cache_recorded?"true":"false")<<",\"geometry_epoch\":"<<p.cache_key.geometry<<",\"content_key\":\""<<p.cache_key.content<<"\""<<",\"bytes\":"<<p.bytes<<",\"production_hit_enabled\":false,\"shadow_at_true_position\":true,\"ddgi_at_true_position\":true,\"tile_radiosity_available\":false,\"point_radiosity_recorded\":"<<(p.radio_recorded?"true":"false")<<"}";
    ok=ok && bool(meta);p.dumped=ok;return ok;
}

bool SurfaceCachePass::PrepareBoundaryRadiosityProof(u32 pass,const std::vector<DescriptorSetLayoutBinding>& bindings){
    const char* flag=std::getenv("SC_BOUNDARY_RADIOSITY_PROOF");auto& p=BoundaryProof();
    if((!boundary_continuous_requested_ && (!flag || std::strcmp(flag,"1")!=0)) || p.failed || !p.recorded || pass<2 || pass>3)return false;
    auto fail=[&](){p.failed=true;return false;};
    if(p.count>BoundaryMaxLightPoints || !p.count)return fail();
    if(p.pipelines[pass]!=handles::INVALID_PIPELINE)return true;
    if(pass==2){const u64 rays=u64(p.count)*32;const u64 sizes[]={rays*32,rays*4,rays*32,u64(p.count)*64,rays*16};
        for(u32 i=0;i<5;++i){BufferDesc d{};d.size=sizes[i];d.type=BufferType::Structured;d.usage=d.memoryUsage=GPUMemoryUsage::Static;
            p.radio_buffers[i]=device_->CreateBuffer(d);if(p.radio_buffers[i]==handles::INVALID_RESOURCE)return fail();}
    }
    p.set_layouts[pass]=device_->CreateDescriptorSetLayout({u32(bindings.size()),bindings.data()});
    if(p.set_layouts[pass]==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return fail();
    PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&p.set_layouts[pass];p.layouts[pass]=device_->CreatePipelineLayout(pl);
    if(p.layouts[pass]==handles::INVALID_PIPELINE_LAYOUT)return fail();p.sets[pass]=device_->CreateDescriptorSet({p.set_layouts[pass]});
    if(p.sets[pass]==handles::INVALID_DESCRIPTOR_SET)return fail();
    auto code=LoadShaderBytecode(device_,pass==2?"SurfaceCacheIndirectTrace_boundary":"SurfaceCacheIndirectResolve_boundary");if(code.empty())return fail();
    auto shader=device_->CreateShader(code.data(),code.size(),ShaderStage::Compute,pass==2?"surface_cache_indirect_trace":"surface_cache_indirect_resolve");
    if(shader==handles::INVALID_SHADER)return fail();ComputePipelineDesc desc{};desc.computeShader=shader;desc.layout=p.layouts[pass];
    p.pipelines[pass]=device_->CreateComputePipeline(desc);device_->DestroyShader(shader);
    if(p.pipelines[pass]==handles::INVALID_PIPELINE)return fail();return true;
}

bool SurfaceCachePass::RecordBoundaryRadianceCache(RHICommandBuffer* cmd,ResourceHandle params){
    const char* sequence=std::getenv("SC_BOUNDARY_CACHE_PROOF_FRAMES");auto& p=BoundaryProof();
    if(!boundary_continuous_requested_ && (!sequence || !*sequence))return true;
    if(p.failed || !p.radio_recorded || p.epoch!=card_generator_.GetAllocationEpoch() || p.cache_key.geometry!=geometry_epoch_)return false;
    auto fail=[&](){p.failed=true;return false;};
    if(p.cache==handles::INVALID_RESOURCE){
    BufferDesc d{};d.size=u64(p.count)*64;d.type=BufferType::Structured;d.usage=d.memoryUsage=GPUMemoryUsage::Static;
    p.cache=device_->CreateBuffer(d);
    if(p.cache==handles::INVALID_RESOURCE)return fail();
    DescriptorSetLayoutBinding bindings[5];for(u32 i=0;i<5;++i)bindings[i]={i,i==4?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};
    p.set_layouts[4]=device_->CreateDescriptorSetLayout({5,bindings});if(p.set_layouts[4]==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return fail();
    PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&p.set_layouts[4];p.layouts[4]=device_->CreatePipelineLayout(pl);
    if(p.layouts[4]==handles::INVALID_PIPELINE_LAYOUT)return fail();p.sets[4]=device_->CreateDescriptorSet({p.set_layouts[4]});if(p.sets[4]==handles::INVALID_DESCRIPTOR_SET)return fail();
    auto code=LoadShaderBytecode(device_,"SCBoundaryCompose");if(code.empty())return fail();auto shader=device_->CreateShader(code.data(),code.size(),ShaderStage::Compute,"main");
    if(shader==handles::INVALID_SHADER)return fail();ComputePipelineDesc pd{};pd.computeShader=shader;pd.layout=p.layouts[4];p.pipelines[4]=device_->CreateComputePipeline(pd);device_->DestroyShader(shader);
    if(p.pipelines[4]==handles::INVALID_PIPELINE)return fail();
    }
    DescriptorData descriptors[]={{0,DescriptorType::StorageBuffer,p.points},{1,DescriptorType::StorageBuffer,p.results},{2,DescriptorType::StorageBuffer,p.radio_buffers[3]},
        {3,DescriptorType::StorageBuffer,p.cache},{4,DescriptorType::UniformBuffer,params}};
    UpdateDescriptorSet(device_,p.sets[4],descriptors,5);
    cmd->MemoryBarrier(PipelineStage::ComputeShader|PipelineStage::Transfer|PipelineStage::Host,PipelineStage::ComputeShader,
        AccessFlag::ShaderWrite|AccessFlag::TransferRead|AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
    cmd->BindComputePipeline(p.pipelines[4]);cmd->BindDescriptorSets(PipelineBindPoint::Compute,p.layouts[4],0,1,&p.sets[4],0,nullptr);BoundaryTimestamp(cmd,8);cmd->Dispatch((p.count+63)/64,1,1);BoundaryTimestamp(cmd,9);
    if(p.readback_requested){
        if(p.cache_staging==handles::INVALID_RESOURCE){BufferDesc d{};d.size=u64(p.count)*64;d.memoryUsage=d.usage=GPUMemoryUsage::Readback;
            p.cache_staging=device_->CreateBuffer(d);if(p.cache_staging==handles::INVALID_RESOURCE)return fail();}
    cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
    cmd->CopyBuffer(p.cache,p.cache_staging,0,0,u64(p.count)*64);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::ComputeShader,AccessFlag::TransferRead,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
    }else cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead);
    p.cache_recorded=true;return true;
}

bool SurfaceCachePass::ReadbackBoundaryLookupProof(const char* prefix){
    const char* fixture=std::getenv("SC_BOUNDARY_LOOKUP_FIXTURE");if(!fixture || !*fixture)return true;
    auto& p=BoundaryProof();if(!p.cache_recorded || p.lookup_lighting==handles::INVALID_RESOURCE)return false;
    using Index=BoundaryPointIndex;
    struct Query{u32 identity[4];float position[4];};static_assert(sizeof(Query)==32);
    std::ifstream input(fixture,std::ios::binary|std::ios::ate);const auto bytes=input.tellg();if(bytes<24 || bytes>16*1024*1024)return false;
    input.seekg(0);u32 h[6]{};input.read(reinterpret_cast<char*>(h),24);
    if(!input || h[0]!=0x53435031u || h[1]!=1 || h[2]!=card_generator_.GetCardCount() || h[3]!=p.epoch ||
       !h[4] || h[4]>4096 || !h[5] || h[5]>4096)return false;
    std::vector<Index::Group> groups;
    for(u32 i=0;i<h[4];++i){u32 key[4]{};input.read(reinterpret_cast<char*>(key),16);
        if(!input || !key[2] || key[2]>Index::MaxGroupPoints || key[3])return false;
        Index::Group g{key[0],key[1],{}};g.points.resize(key[2]);input.read(reinterpret_cast<char*>(g.points.data()),key[2]*sizeof(Index::Point));if(!input)return false;
        if(p.mixed_index==handles::INVALID_RESOURCE)for(auto& point:g.points)if(point.source&0x80000000u){u32 original=point.source&0x7fffffffu,found=UINT32_MAX;
            for(u32 j=0;j<p.cpu_points.size();++j){const auto& bank=p.cpu_points[j];
                if(bank.identity[0]==g.card && bank.identity[1]==g.triangle && std::bit_cast<u32>(bank.albedo[3])==original){
                    if(found!=UINT32_MAX)return false;found=j;}}
            if(found==UINT32_MAX)return false;point.source=0x80000000u|found;
        }
        groups.push_back(std::move(g));
    }
    std::vector<Query> queries(h[5]);input.read(reinterpret_cast<char*>(queries.data()),queries.size()*sizeof(Query));if(!input || input.tellg()!=bytes)return false;
    for(auto& q:queries){if(q.identity[2]!=p.epoch || q.identity[3]!=0)return false;q.identity[3]=p.frame;}
    // Actual shader rejection fixtures: freshness, allocation, ID and bad input.
    const Query first=queries[0];Query bad=first;bad.identity[3]=p.frame-1;queries.push_back(bad);
    bad=first;bad.identity[2]=p.epoch-1;queries.push_back(bad);
    bad=first;bad.identity[1]=UINT32_MAX;queries.push_back(bad);
    bad=first;bad.identity[0]=h[2];queries.push_back(bad);
    bad=first;bad.position[0]=std::bit_cast<float>(0x7fc00001u);queries.push_back(bad);
    const u64 baseQueryCount=queries.size();
    // Read-only proof: source points and same-ID midpoints/centroids from the
    // actual current bank, sampled by ordinal rather than successful queries.
    const size_t stride=std::max<size_t>(1,(p.cpu_points.size()+1023)/1024);
    for(size_t i=0;i<p.cpu_points.size() && queries.size()<3840;i+=stride){
        const auto& a=p.cpu_points[i];Query q{};
        std::memcpy(q.identity,a.identity,16);std::memcpy(q.position,a.world_footprint,12);queries.push_back(q);
        for(size_t amount=2;amount<=3 && i+amount<=p.cpu_points.size() && queries.size()<3840;++amount){
            bool same=true;for(size_t j=1;j<amount;++j)same=same && std::memcmp(a.identity,p.cpu_points[i+j].identity,16)==0;
            if(!same)continue;
            for(size_t k=0;k<3;++k){float sum=0;for(size_t j=0;j<amount;++j)sum+=p.cpu_points[i+j].world_footprint[k];q.position[k]=sum/float(amount);}
            queries.push_back(q);
        }
    }
    std::vector<u32> words;
    const bool automatic=p.mixed_index!=handles::INVALID_RESOURCE;
    if(automatic)words=p.mixed_words;else if(!Index::Build(h[2],p.epoch,groups,words))return false;
    struct Resources{RHIDeviceBase* device;ResourceHandle b[7]{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};
        PipelineHandle pipeline{handles::INVALID_PIPELINE};PipelineLayoutHandle pl{handles::INVALID_PIPELINE_LAYOUT};DescriptorSetLayoutHandle sl{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
        DescriptorSetHandle set{handles::INVALID_DESCRIPTOR_SET};CommandBufferHandle command{handles::INVALID_COMMAND_BUFFER};
        ~Resources(){if(command!=handles::INVALID_COMMAND_BUFFER)device->DestroyCommandBuffer(command);
            if(pipeline!=handles::INVALID_PIPELINE)device->DestroyPipeline(pipeline);if(set!=handles::INVALID_DESCRIPTOR_SET)device->DestroyDescriptorSet(set);
            if(pl!=handles::INVALID_PIPELINE_LAYOUT)device->DestroyPipelineLayout(pl);if(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device->DestroyDescriptorSetLayout(sl);
            for(auto buffer:b)if(buffer!=handles::INVALID_RESOURCE)device->DestroyBuffer(buffer);}
    } r{device_};
    const u64 size[]={words.size()*4,queries.size()*32,queries.size()*128,queries.size()*128,u64(atlas_size_)*atlas_size_*8,u64(atlas_size_)*atlas_size_*4,u64(atlas_size_)*atlas_size_*4};
    const void* data[]={words.data(),queries.data(),nullptr,nullptr,nullptr,nullptr,nullptr};
    for(u32 i=0;i<7;++i){BufferDesc d{};d.size=size[i];d.type=BufferType::Structured;d.usage=d.memoryUsage=i>=3?GPUMemoryUsage::Readback:GPUMemoryUsage::Static;
        r.b[i]=device_->CreateBuffer(d);if(r.b[i]==handles::INVALID_RESOURCE || (data[i] && !device_->UpdateBufferData(r.b[i],data[i],size[i],0)))return false;}
    DescriptorSetLayoutBinding bindings[9];for(u32 i=0;i<9;++i)bindings[i]={i,(i==6 || i==7)?DescriptorType::SampledImage:DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};
    r.sl=device_->CreateDescriptorSetLayout({9,bindings});if(r.sl==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
    PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&r.sl;r.pl=device_->CreatePipelineLayout(pl);if(r.pl==handles::INVALID_PIPELINE_LAYOUT)return false;
    r.set=device_->CreateDescriptorSet({r.sl});if(r.set==handles::INVALID_DESCRIPTOR_SET)return false;
    auto code=LoadShaderBytecode(device_,"SCBoundaryLookupProof");if(code.empty())return false;auto shader=device_->CreateShader(code.data(),code.size(),ShaderStage::Compute,"main");if(shader==handles::INVALID_SHADER)return false;
    ComputePipelineDesc desc{};desc.layout=r.pl;desc.computeShader=shader;r.pipeline=device_->CreateComputePipeline(desc);device_->DestroyShader(shader);if(r.pipeline==handles::INVALID_PIPELINE)return false;
    DescriptorData descriptors[]={{0,DescriptorType::StorageBuffer,automatic?p.mixed_index:r.b[0]},{1,DescriptorType::StorageBuffer,r.b[1]},
        {2,DescriptorType::StorageBuffer,p.points},{3,DescriptorType::StorageBuffer,p.cache},{4,DescriptorType::StorageBuffer,card_generator_.GetCardDataBuffer()},
        {5,DescriptorType::StorageBuffer,triangle_atlas_buffer_},{6,DescriptorType::SampledImage,depth_atlas_},{7,DescriptorType::SampledImage,p.lookup_lighting},{8,DescriptorType::StorageBuffer,r.b[2]}};
    UpdateDescriptorSet(device_,r.set,descriptors,9);r.command=device_->CreateCommandBuffer(CommandQueueType::Graphics);auto* cmd=rhi::GetCommandBuffer(r.command);
    if(!cmd || !cmd->Begin())return false;
    cmd->MemoryBarrier(PipelineStage::AllCommands|PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::MemoryWrite|AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
    cmd->BindComputePipeline(r.pipeline);cmd->BindDescriptorSets(PipelineBindPoint::Compute,r.pl,0,1,&r.set,0,nullptr);cmd->Dispatch((2u*u32(queries.size())+7)/8,1,1);
    cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite|AccessFlag::ShaderRead,AccessFlag::TransferRead);
    cmd->CopyBuffer(r.b[2],r.b[3],0,0,size[3]);
    for(u32 i=0;i<2;++i){ResourceBarrier barrier{};barrier.resource=i?depth_atlas_:p.lookup_lighting;barrier.beforeState=ResourceState::ShaderResource;barrier.afterState=ResourceState::CopySource;barrier.subresource=UINT32_MAX;
        cmd->InsertBarrier(&barrier,1);BufferTextureCopyRegion region{};region.imageSubresource.layerCount=1;region.imageExtent={atlas_size_,atlas_size_,1};
        cmd->CopyTextureToBuffer(barrier.resource,r.b[4+i],&region,1);barrier.beforeState=ResourceState::CopySource;barrier.afterState=ResourceState::ShaderResource;cmd->InsertBarrier(&barrier,1);}
    cmd->CopyBuffer(triangle_atlas_buffer_,r.b[6],0,0,size[6]);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
    if(!cmd->End() || !cmd->Submit() || !cmd->WaitForCompletion())return false;
    bool ok=true;
    auto save=[&](const char* name,const void* data,u64 bytes){std::ofstream file(std::string(prefix)+name,std::ios::binary|std::ios::trunc);file.write(static_cast<const char*>(data),bytes);return bool(file);};
    ok=save(".lookup-index.bin",words.data(),words.size()*4) && save(".lookup-queries.bin",queries.data(),baseQueryCount*32) &&
        save(".lookup-source-binary-queries.bin",queries.data(),queries.size()*32);
    const char* names[]={".lookup-results.bin",".lookup-lighting.bin",".lookup-depth.bin",".lookup-ids.bin"};
    for(u32 i=3;i<7;++i){auto* data=device_->MapBuffer(r.b[i]);if(!data)return false;
        if(i==3){const u32* out=static_cast<const u32*>(data);for(u32 j=h[5];j<baseQueryCount;++j)ok=ok && out[16*j+3]==0;
            const u64 bytes=queries.size()*64;const auto* alternate=static_cast<const u8*>(data)+bytes;
            const bool equal=std::memcmp(data,alternate,bytes)==0;ok=ok && equal;
            ok=save(".lookup-source-binary-reference.bin",data,bytes) && save(".lookup-source-binary-results.bin",alternate,bytes) && ok;
            PRIMAL_LOG_STREAM(Info,"Render")<<"[SC_BOUNDARY_LOOKUP] queries="<<h[5]<<" negatives=5 frame="<<p.frame
                <<" source_binary_pairs="<<queries.size()<<" bitwise="<<(equal?"PASS":"FAIL")<<std::endl;
        }
        ok=save(names[i-3],data,i==3?baseQueryCount*64:size[i]) && ok;device_->UnmapBuffer(r.b[i]);}
    return ok;
}
