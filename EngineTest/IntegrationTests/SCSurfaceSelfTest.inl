// Executes the production composite and lookup helpers on isolated resources.
static bool RunSCSurfaceSelfTest(RHIDeviceBase* device) {
    using primal::graphics::lumen::StaticCompositeInfo;
    struct Resources {
        RHIDeviceBase* d; std::vector<ResourceHandle> buffers,textures;
        std::vector<ShaderHandle> shaders; std::vector<PipelineHandle> pipes;
        std::vector<DescriptorSetHandle> sets; std::vector<PipelineLayoutHandle> layouts;
        std::vector<DescriptorSetLayoutHandle> setLayouts;
        CommandBufferHandle command=handles::INVALID_COMMAND_BUFFER;
        ~Resources() {
            if(command!=handles::INVALID_COMMAND_BUFFER)d->DestroyCommandBuffer(command);
            for(auto p:pipes)d->DestroyPipeline(p);
            for(auto s:sets)d->DestroyDescriptorSet(s);
            for(auto l:layouts)d->DestroyPipelineLayout(l);
            for(auto l:setLayouts)d->DestroyDescriptorSetLayout(l);
            for(auto s:shaders)d->DestroyShader(s);
            for(auto t:textures)d->DestroyTexture(t);
            for(auto b:buffers)d->DestroyBuffer(b);
        }
    } r{device};
    auto buffer=[&](const void* p,u64 n,bool readback=false) {
        BufferDesc d{};d.size=n;d.type=BufferType::Structured;
        d.memoryUsage=readback?GPUMemoryUsage::Readback:GPUMemoryUsage::Dynamic;
        auto b=device->CreateBuffer(d); if(b==handles::INVALID_RESOURCE)return b;
        r.buffers.push_back(b);
        if(p) {auto* mapped=device->MapBuffer(b);if(!mapped)return handles::INVALID_RESOURCE;std::memcpy(mapped,p,n);device->UnmapBuffer(b);}
        return b;
    };
    std::vector<u32> pool(3u*3u*64u*64u);
    StaticCompositeInfo info[3]{};
    const float centers[3][3]={{10,20,30},{-5,6,7},{8,-9,10}};
    const float extent[3][3]={{.25,1,2},{.25,.5,1},{.5,.5,1}};
    const float scales[3][3]={{1,1,1},{2,3,4},{.5,2,-2}};
    const float depths[3]={.1,.3,.2};
    for(u32 c=0;c<3;c++) {
        auto& m=info[c];m.meta0[0]=c*64;m.meta0[2]=m.meta0[3]=64;
        m.meta1[0]=c*4096;m.meta1[1]=c==0?3:0;m.meta1[2]=c==2?2:256+c;m.meta1[3]=256+c;
        m.local0[3]=extent[c][0];m.local1[0]=extent[c][1];m.local1[1]=extent[c][2];
        m.local0[0]=.25;m.local0[1]=.5;m.local0[2]=1.;
        std::memcpy(m.center_w,centers[c],12);
        m.mcol0[0]=scales[c][0];m.mcol1[1]=scales[c][1];m.mcol2[2]=scales[c][2];
        float bits=depths[c];u32 db;std::memcpy(&db,&bits,4);
        // oct(-X)=(0,.5), oct(.6,-.8,0)=(5/7,3/14), oct(-Z)=(1,1).
        const u32 oct=c==0?0x38000000u:c==1?0x32DB39B7u:0x3C003C00u;
        for(u32 plane=0;plane<3;plane++) {
            m.meta2[plane]=(c*3+plane)*4096;
            std::fill(pool.begin()+m.meta2[plane],pool.begin()+m.meta2[plane]+4096,
                plane==0?0xFF808040u:plane==1?oct:db);
        }
        pool[m.meta2[0]]=0; // honest hole, not a surface
    }
    pool.resize(4u*3u*4096u,7u);
    for(u32 c=0;c<3;c++) info[c].meta2[3]=9u*4096u+c*4096u;
    auto ib=buffer(info,sizeof(info)),pb=buffer(pool.data(),pool.size()*4);
    auto ids=buffer(nullptr,192u*64u*4u);
    u32 supportWords[48]{};supportWords[0]=0x53434831u;supportWords[1]=7;supportWords[2]=8;supportWords[3]=48;
    const u32 directory[16]={20,2,26,0,999,1,0,16385,26,1,29,1,32,1,35,1};
    std::memcpy(supportWords+4,directory,sizeof(directory));
    const u32 records[18]={7,40,3,9,43,2,7,999,2,7,45,65,7,45,1,7,45,2};
    std::memcpy(supportWords+20,records,sizeof(records));
    auto support=buffer(supportWords,sizeof(supportWords));if(support==handles::INVALID_RESOURCE)return false;
    auto out=buffer(nullptr,112*16),rb=buffer(nullptr,112*16,true);
    if(ib==handles::INVALID_RESOURCE || pb==handles::INVALID_RESOURCE || ids==handles::INVALID_RESOURCE || out==handles::INVALID_RESOURCE || rb==handles::INVALID_RESOURCE)return false;
    const DataFormat formats[4]={DataFormat::RGBA8_UNorm,DataFormat::RG16_Float,DataFormat::R32_Float,DataFormat::RGBA16_Float};
    ResourceHandle atlas[4];
    for(u32 i=0;i<4;i++) {
        TextureDesc d{};d.size={192,64,1};d.format=formats[i];d.type=TextureType::Texture2D;
        d.usage=TextureUsage::UnorderedAccess|TextureUsage::ShaderResource;
        atlas[i]=device->CreateTexture(d);if(atlas[i]==handles::INVALID_RESOURCE)return false;r.textures.push_back(atlas[i]);
    }
    auto pipeline=[&](const char* path,const char* entry,const std::vector<DescriptorType>& types,
                      const std::vector<ResourceHandle>& sources) {
        auto spv=LoadSpvBytes(path);if(spv.empty())return false;
        auto shader=device->CreateShader(spv.data(),spv.size(),ShaderStage::Compute,entry);
        if(shader==handles::INVALID_SHADER)return false;r.shaders.push_back(shader);
        std::vector<DescriptorSetLayoutBinding> bindings;
        for(u32 i=0;i<types.size();i++)bindings.push_back({i,types[i],1,ShaderStage::Compute,nullptr});
        auto sl=device->CreateDescriptorSetLayout({u32(bindings.size()),bindings.data()});r.setLayouts.push_back(sl);
        PipelineLayoutDesc ld{};ld.setLayoutCount=1;ld.setLayouts=&sl;
        auto layout=device->CreatePipelineLayout(ld);r.layouts.push_back(layout);
        auto set=device->CreateDescriptorSet({sl});r.sets.push_back(set);
        std::vector<DescriptorBufferInfo> bi(types.size());std::vector<DescriptorImageInfo> ii(types.size());std::vector<WriteDescriptorSet> w;
        for(u32 i=0;i<types.size();i++) {
            bi[i]={sources[i],0,0};ii[i]={handles::INVALID_SAMPLER,sources[i],types[i]==DescriptorType::StorageImage?ResourceState::UnorderedAccess:ResourceState::ShaderResource};
            w.push_back({set,i,0,1,types[i],types[i]==DescriptorType::StorageBuffer?nullptr:&ii[i],types[i]==DescriptorType::StorageBuffer?&bi[i]:nullptr});
        }
        device->UpdateDescriptorSets(u32(w.size()),w.data());
        ComputePipelineDesc pd{};pd.computeShader=shader;pd.layout=layout;auto p=device->CreateComputePipeline(pd);
        if(p==handles::INVALID_PIPELINE)return false;r.pipes.push_back(p);return true;
    };
    if(!pipeline("Engine/Graphics/Vulkan/Shaders/Lumen/SurfaceCacheStaticComposite.comp.spv","surface_cache_static_composite",
        {DescriptorType::StorageImage,DescriptorType::StorageImage,DescriptorType::StorageImage,DescriptorType::StorageBuffer,DescriptorType::StorageBuffer,DescriptorType::StorageImage,DescriptorType::StorageBuffer},
        {atlas[0],atlas[1],atlas[2],ib,pb,atlas[3],ids}) ||
       !pipeline("Engine/Graphics/Vulkan/Shaders/Lumen/SCSurfaceLookupTest.comp.spv","sc_surface_lookup_test",
        {DescriptorType::SampledImage,DescriptorType::SampledImage,DescriptorType::SampledImage,DescriptorType::StorageBuffer,DescriptorType::SampledImage,DescriptorType::StorageBuffer},
        {atlas[2],atlas[0],atlas[1],out,atlas[3],support}))return false;
    r.command=device->CreateCommandBuffer(CommandQueueType::Graphics);auto* cmd=rhi::GetCommandBuffer(r.command);
    if(!cmd || !cmd->Begin())return false;
    ResourceBarrier barriers[4]{};
    for(u32 i=0;i<4;i++){barriers[i].resource=atlas[i];barriers[i].beforeState=ResourceState::Unknown;barriers[i].afterState=ResourceState::UnorderedAccess;barriers[i].subresource=0xFFFFFFFF;}
    cmd->InsertBarrier(barriers,4);
    cmd->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead);
    cmd->BindComputePipeline(r.pipes[0]);cmd->BindDescriptorSets(PipelineBindPoint::Compute,r.layouts[0],0,1,&r.sets[0],0,nullptr);cmd->Dispatch(48,1,1);
    for(auto& b:barriers){b.beforeState=ResourceState::UnorderedAccess;b.afterState=ResourceState::ShaderResource;}
    cmd->InsertBarrier(barriers,4);
    cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead);
    cmd->BindComputePipeline(r.pipes[1]);cmd->BindDescriptorSets(PipelineBindPoint::Compute,r.layouts[1],0,1,&r.sets[1],0,nullptr);cmd->Dispatch(14,1,1);
    cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
    cmd->CopyBuffer(out,rb,0,0,112*16);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
    if(!cmd->End() || !cmd->Submit() || !cmd->WaitForCompletion())return false;
    const auto* results=static_cast<const u32*>(device->MapBuffer(rb));if(!results)return false;
    bool ok=true;
    for(u32 i=0;i<112;i++){float t;std::memcpy(&t,results+i*4+2,4);ok &= results[i*4]==1;
        std::cerr<<"[SC_SURFACE_SELFTEST] case="<<i<<" pass="<<results[i*4]<<" hit="<<results[i*4+1]<<" t="<<t<<" reject="<<results[i*4+3]<<std::endl;}
    device->UnmapBuffer(rb);return ok;
}
