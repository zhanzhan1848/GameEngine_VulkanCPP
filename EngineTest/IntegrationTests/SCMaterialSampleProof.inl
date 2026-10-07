// Explicit opt-in synchronous Vulkan test diagnostic; no production atlas writes.
static bool RunSCMaterialSampleProof(RHIDeviceBase* device,const char* prefix) {
    const char* queryPath=std::getenv("SC_GPU_MATERIAL_QUERIES");
    if(!prefix || !prefix[0] || !queryPath || !queryPath[0]) return false;
    std::ifstream file(queryPath,std::ios::binary|std::ios::ate);
    if(!file || file.tellg()<16 || file.tellg()>16+16384*32) return false;
    std::vector<u8> query(size_t(file.tellg())); file.seekg(0);file.read(reinterpret_cast<char*>(query.data()),query.size());
    u32 count=0,reserved=0;std::memcpy(&count,query.data()+8,4);std::memcpy(&reserved,query.data()+12,4);
    if(!file || std::memcmp(query.data(),"PGMATQ01",8) || !count || count>16384 || reserved || query.size()!=16+size_t(count)*32) return false;
    for(u32 i=0;i<count;i++) {
        float bary[3];std::memcpy(bary,query.data()+16+i*32+16,12);
        for(float v:bary) if(!SCProofFinite(v) || v<0 || v>1.00002f) return false;
        if(std::abs(bary[0]+bary[1]+bary[2]-1.f)>2e-5f) return false;
    }
    auto* vk=static_cast<VulkanDevice*>(device);
    auto& draw=primal::graphics::nanite::GPUDrivenDrawPipeline::Get();
    const ResourceHandle source[4]={draw.GetGlobalVertexBuffer(),draw.GetGlobalElementBuffer(),draw.GetGlobalInstanceDataBuffer(),draw.GetMaterialDataBuffer()};
    u64 sizes[4]{};
    for(u32 i=0;i<4;i++) if(!vk->GetBufferByteSize(source[i],sizes[i]) || sizes[i]>64u*1024u*1024u) return false;
    if(sizes[0]%12 || sizes[1]!=sizes[0]*2 || sizes[2]%192 || sizes[3]%48) return false;
    const auto texture=draw.GetAlbedoTextureArray();auto* tex=vk->GetTexture(texture);
    if(!tex) return false;
    const auto desc=tex->GetTextureDesc();
    PRIMAL_LOG_STREAM(Info,"Render")<<"[SC_GPU_MATERIAL_INPUT] positions="<<sizes[0]<<" elements="<<sizes[1]
        <<" instances="<<sizes[2]<<" materials="<<sizes[3]<<" texture="<<u32(desc.format)<<" type="<<u32(desc.type)<<" mips="<<desc.mipLevels;
    if((desc.format!=DataFormat::RGBA8_UNorm && desc.format!=DataFormat::RGBA8_sRGB) || desc.type!=TextureType::Texture2DArray || !desc.mipLevels) return false;
    const u32 header[4]={count,u32(sizes[0]/12),u32(sizes[2]/192),u32(sizes[3]/48)};
    std::memcpy(query.data(),header,16);
    struct Resources {
        RHIDeviceBase* d;std::vector<ResourceHandle> buffers;
        ShaderHandle shader{handles::INVALID_SHADER};DescriptorSetLayoutHandle layout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
        PipelineLayoutHandle pl{handles::INVALID_PIPELINE_LAYOUT};DescriptorSetHandle set{handles::INVALID_DESCRIPTOR_SET};
        PipelineHandle pipe{handles::INVALID_PIPELINE};CommandBufferHandle command{handles::INVALID_COMMAND_BUFFER};SamplerHandle sampler{handles::INVALID_SAMPLER};
        ~Resources(){
            if(command!=handles::INVALID_COMMAND_BUFFER)d->DestroyCommandBuffer(command);
            if(pipe!=handles::INVALID_PIPELINE)d->DestroyPipeline(pipe);
            if(set!=handles::INVALID_DESCRIPTOR_SET)d->DestroyDescriptorSet(set);
            if(pl!=handles::INVALID_PIPELINE_LAYOUT)d->DestroyPipelineLayout(pl);
            if(layout!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)d->DestroyDescriptorSetLayout(layout);
            if(shader!=handles::INVALID_SHADER)d->DestroyShader(shader);
            if(sampler!=handles::INVALID_SAMPLER)d->DestroySampler(sampler);
            for(auto b:buffers)d->DestroyBuffer(b);
        }
    } r{device};
    auto buffer=[&](u64 bytes,bool readback){BufferDesc d{};d.size=bytes;d.type=BufferType::Structured;
        d.usage=d.memoryUsage=readback?GPUMemoryUsage::Readback:GPUMemoryUsage::Static;
        auto b=device->CreateBuffer(d);if(b!=handles::INVALID_RESOURCE)r.buffers.push_back(b);return b;};
    const u64 outputBytes=u64(count)*18*16;
    auto qb=buffer(query.size(),false),out=buffer(outputBytes,false),rb=buffer(outputBytes,true),qrb=buffer(query.size(),true);
    ResourceHandle readbacks[4]{};for(u32 i=0;i<4;i++)readbacks[i]=buffer(sizes[i],true);
    if(r.buffers.size()!=8 || !device->UpdateBufferData(qb,query.data(),query.size(),0)) return false;
    auto spv=LoadSpvBytes("Engine/Graphics/Vulkan/Shaders/Lumen/SCMaterialSampleProof.comp.spv");if(spv.empty()) return false;
    r.shader=device->CreateShader(spv.data(),spv.size(),ShaderStage::Compute,"main");
    DescriptorSetLayoutBinding bindings[7]{};
    for(u32 i=0;i<7;i++)bindings[i]={i,i==4?DescriptorType::SampledImage:(i==5?DescriptorType::Sampler:DescriptorType::StorageBuffer),1,ShaderStage::Compute,nullptr};
    DescriptorSetLayoutDesc ld{7,bindings};r.layout=device->CreateDescriptorSetLayout(ld);
    PipelineLayoutDesc pd{};pd.setLayoutCount=1;pd.setLayouts=&r.layout;r.pl=device->CreatePipelineLayout(pd);
    DescriptorSetDesc sd{};sd.layout=r.layout;r.set=device->CreateDescriptorSet(sd);
    SamplerDesc sampler{};sampler.minFilter=sampler.magFilter=sampler.mipFilter=FilterMode::Linear;
    sampler.addressU=sampler.addressV=sampler.addressW=TextureAddressMode::Clamp;r.sampler=device->CreateSampler(sampler);
    if(r.shader==handles::INVALID_SHADER || r.layout==handles::INVALID_DESCRIPTOR_SET_LAYOUT || r.pl==handles::INVALID_PIPELINE_LAYOUT || r.set==handles::INVALID_DESCRIPTOR_SET || r.sampler==handles::INVALID_SAMPLER) return false;
    DescriptorBufferInfo bi[5]={{qb,0,query.size()},{source[1],0,sizes[1]},{source[2],0,sizes[2]},{source[3],0,sizes[3]},{out,0,outputBytes}};
    DescriptorImageInfo ii[2]={{handles::INVALID_SAMPLER,texture,ResourceState::ShaderResource},{r.sampler,handles::INVALID_RESOURCE,ResourceState::ShaderResource}};
    WriteDescriptorSet writes[7]{};
    for(u32 i=0;i<7;i++)writes[i]={r.set,i,0,1,bindings[i].descriptorType,i==4?&ii[0]:(i==5?&ii[1]:nullptr),i<4?&bi[i]:(i==6?&bi[4]:nullptr)};
    device->UpdateDescriptorSets(7,writes);
    ComputePipelineDesc cp{};cp.computeShader=r.shader;cp.layout=r.pl;r.pipe=device->CreateComputePipeline(cp);
    r.command=device->CreateCommandBuffer(CommandQueueType::Graphics);
    if(r.pipe==handles::INVALID_PIPELINE || r.command==handles::INVALID_COMMAND_BUFFER) return false;
    auto* cmd=vk->GetCommandBuffer(r.command);if(!cmd->Reset() || !cmd->Begin()) return false;
    cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::AllCommands,PipelineStage::ComputeShader,
        AccessFlag::HostWrite|AccessFlag::MemoryWrite,AccessFlag::ShaderRead);
    cmd->BindComputePipeline(r.pipe);cmd->BindDescriptorSets(PipelineBindPoint::Compute,r.pl,0,1,&r.set,0,nullptr);cmd->Dispatch((count+31)/32,1,1);
    cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::ComputeShader|PipelineStage::AllCommands,PipelineStage::Transfer,
        AccessFlag::HostWrite|AccessFlag::ShaderWrite|AccessFlag::MemoryWrite,AccessFlag::TransferRead);
    cmd->CopyBuffer(out,rb,0,0,outputBytes);cmd->CopyBuffer(qb,qrb,0,0,query.size());
    for(u32 i=0;i<4;i++)cmd->CopyBuffer(source[i],readbacks[i],0,0,sizes[i]);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::AllCommands,AccessFlag::TransferRead,AccessFlag::ShaderRead);
    if(!cmd->End() || !cmd->Submit() || !cmd->WaitForCompletion()) return false;
    auto dump=[&](ResourceHandle b,u64 bytes,const char* suffix,const void* expected=nullptr){
        const auto* mapped=device->MapBuffer(b);if(!mapped)return false;
        bool match=!expected || std::memcmp(mapped,expected,size_t(bytes))==0;
        std::ofstream f(std::string(prefix)+suffix,std::ios::binary);f.write(static_cast<const char*>(mapped),bytes);
        device->UnmapBuffer(b);return match && bool(f);};
    if(!dump(rb,outputBytes,".raw") || !dump(qrb,query.size(),".queries.bin",query.data())) return false;
    const char* suffix[4]={".positions.bin",".elements.bin",".instances.bin",".materials.bin"};
    for(u32 i=0;i<4;i++)if(!dump(readbacks[i],sizes[i],suffix[i])) return false;
    std::ofstream meta(std::string(prefix)+".json");
    meta<<"{\"complete\":true,\"queries\":"<<count<<",\"result_stride\":288,\"resident_query_bit_identical\":true,\"source_handles\":["<<source[0]<<','<<source[1]<<','<<source[2]<<','<<source[3]
        <<"],\"albedo_handle\":"<<texture<<",\"albedo_size\":["<<desc.size.x<<','<<desc.size.y<<','<<desc.arraySize<<"],\"mip_levels\":"<<desc.mipLevels
        <<",\"format\":"<<u32(desc.format)<<",\"srgb_decoded\":"<<(desc.format==DataFormat::RGBA8_sRGB?"true":"false")
        <<",\"sampler\":\"independent linear-clamp mip0, matching CardFill center contract\",\"default_changed\":false,\"opacity_certified\":false}";
    PRIMAL_LOG_STREAM(Info,"Render")<<"[SC_GPU_MATERIAL_PROOF] complete queries="<<count;
    return bool(meta);
}
