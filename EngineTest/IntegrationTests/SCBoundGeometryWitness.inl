// Strict opt-in: post-frame query on the live SC-owned buffers, no scene rebuild.
static bool RunSCBoundGeometryWitness(RHIDeviceBase* device,
    primal::graphics::lumen::SurfaceCachePass* sc,
    primal::graphics::lumen::LumenDDGIPass* ddgi,u32 frame,const char* prefix,const char* queryPath) {
    if(!device||device->GetPlatform()!=RHIPlatform::Vulkan||!sc||!ddgi||!prefix||!queryPath)return false;
    const auto view=sc->GetGeometryWitnessView(frame);
    if(!view.ready||view.slot_epoch!=view.scene_epoch||ddgi->GetVolumeData().FrameIndex!=frame)return false;
    for(u32 i=0;i<4;++i)if(view.buffers[i]!=ddgi->GetMeasuredGeometrySource(i))return false;
    std::ifstream file(queryPath,std::ios::binary|std::ios::ate);
    if(!file||file.tellg()<16||file.tellg()>8208)return false;
    const auto bytes=size_t(file.tellg());std::vector<u8> packet(bytes);file.seekg(0);file.read(reinterpret_cast<char*>(packet.data()),bytes);
    u32 count=0,reserved=0;std::memcpy(&count,packet.data()+8,4);std::memcpy(&reserved,packet.data()+12,4);
    if(!file||std::memcmp(packet.data(),"PGRAYS01",8)||count<2||count>256||reserved||bytes!=16+count*32)return false;
    std::vector<float> rays(count*8);std::memcpy(rays.data(),packet.data()+16,rays.size()*4);
    for(u32 i=0;i<count;++i){const auto* r=rays.data()+8*i;
        for(u32 k=0;k<8;++k)if(!SCProofFinite(r[k]))return false;
        if(r[3]<0||r[7]<=r[3]||r[7]>10000||std::abs(r[4]*r[4]+r[5]*r[5]+r[6]*r[6]-1)>2e-5)return false;}
    struct Resources {
        RHIDeviceBase* d;ResourceHandle query=handles::INVALID_RESOURCE,out=handles::INVALID_RESOURCE,readback=handles::INVALID_RESOURCE;
        ShaderHandle shader=handles::INVALID_SHADER;DescriptorSetLayoutHandle sl=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
        PipelineLayoutHandle pl=handles::INVALID_PIPELINE_LAYOUT;DescriptorSetHandle set=handles::INVALID_DESCRIPTOR_SET;
        PipelineHandle pipe=handles::INVALID_PIPELINE;CommandBufferHandle cmd=handles::INVALID_COMMAND_BUFFER;
        ~Resources(){if(cmd!=handles::INVALID_COMMAND_BUFFER)d->DestroyCommandBuffer(cmd);
            if(pipe!=handles::INVALID_PIPELINE)d->DestroyPipeline(pipe);if(set!=handles::INVALID_DESCRIPTOR_SET)d->DestroyDescriptorSet(set);
            if(pl!=handles::INVALID_PIPELINE_LAYOUT)d->DestroyPipelineLayout(pl);if(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)d->DestroyDescriptorSetLayout(sl);
            if(shader!=handles::INVALID_SHADER)d->DestroyShader(shader);
            for(auto b:{query,out,readback})if(b!=handles::INVALID_RESOURCE)d->DestroyBuffer(b);}
    } r{device};
    auto buffer=[&](u64 size,GPUMemoryUsage memory){BufferDesc b{};b.size=size;b.type=BufferType::Structured;b.usage=b.memoryUsage=memory;return device->CreateBuffer(b);};
    const u64 outputBytes=u64(count)*64;r.query=buffer(bytes,GPUMemoryUsage::Dynamic);r.out=buffer(outputBytes,GPUMemoryUsage::Static);r.readback=buffer(outputBytes,GPUMemoryUsage::Readback);
    if(r.query==handles::INVALID_RESOURCE||r.out==handles::INVALID_RESOURCE||r.readback==handles::INVALID_RESOURCE)return false;
    u32 header[4]={count,frame,view.slot,u32(view.scene_epoch)};
    auto* mapped=device->MapBuffer(r.query);if(!mapped)return false;
    std::memcpy(mapped,header,16);std::memcpy(static_cast<u8*>(mapped)+16,rays.data(),rays.size()*4);device->UnmapBuffer(r.query);
    const char* conservativeSetting=std::getenv("SC_BOUND_GEOMETRY_CONSERVATIVE");
    const bool conservative=conservativeSetting && std::strcmp(conservativeSetting,"1")==0;
    auto spv=LoadSpvBytes(conservative?SC_BOUND_GEOMETRY_WITNESS_REPLACE_SPV:SC_BOUND_GEOMETRY_WITNESS_SPV);if(spv.empty())return false;
    r.shader=device->CreateShader(spv.data(),spv.size(),ShaderStage::Compute,"main");if(r.shader==handles::INVALID_SHADER)return false;
    DescriptorSetLayoutBinding bindings[6]{};const u32 locations[6]={0,1,13,14,15,16};
    for(u32 i=0;i<6;++i){bindings[i]={locations[i],DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};bindings[i].readonly=i!=1;}
    r.sl=device->CreateDescriptorSetLayout({6,bindings});if(r.sl==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
    r.pl=device->CreatePipelineLayout({1,&r.sl});if(r.pl==handles::INVALID_PIPELINE_LAYOUT)return false;
    r.set=device->CreateDescriptorSet({r.sl});if(r.set==handles::INVALID_DESCRIPTOR_SET)return false;
    DescriptorBufferInfo infos[6]={{r.query,0,bytes},{r.out,0,outputBytes}};
    for(u32 i=0;i<4;++i)infos[i+2]={view.buffers[i],0,view.sizes[i]};
    WriteDescriptorSet writes[6]{};for(u32 i=0;i<6;++i)writes[i]={r.set,locations[i],0,1,DescriptorType::StorageBuffer,nullptr,&infos[i]};device->UpdateDescriptorSets(6,writes);
    ComputePipelineDesc pd{};pd.computeShader=r.shader;pd.layout=r.pl;r.pipe=device->CreateComputePipeline(pd);if(r.pipe==handles::INVALID_PIPELINE)return false;
    r.cmd=device->CreateCommandBuffer(CommandQueueType::Graphics);auto* cmd=primal::graphics::rhi::GetCommandBuffer(r.cmd);if(!cmd||!cmd->Begin())return false;
    cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::HostWrite|AccessFlag::ShaderWrite,AccessFlag::ShaderRead);
    cmd->BindComputePipeline(r.pipe);cmd->BindDescriptorSets(PipelineBindPoint::Compute,r.pl,0,1,&r.set,0,nullptr);cmd->Dispatch((count+7)/8,1,1);
    cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);cmd->CopyBuffer(r.out,r.readback,0,0,outputBytes);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
    if(!cmd->End()||!cmd->Submit()||!cmd->WaitForCompletion())return false;
    mapped=device->MapBuffer(r.readback,0,outputBytes);if(!mapped)return false;std::vector<u8> result(outputBytes);std::memcpy(result.data(),mapped,outputBytes);device->UnmapBuffer(r.readback);
    // Echo and generation are mandatory, including MISS/UNKNOWN; never certify stale tails.
    for(u32 i=0;i<count;++i){const auto* record=result.data()+i*64;u32 identity[4];std::memcpy(identity,record+48,16);
        if(std::memcmp(record,rays.data()+i*8,32)||identity[0]!=i||identity[1]!=frame||identity[2]!=view.slot||identity[3]!=u32(view.scene_epoch))return false;}
    const std::string rawPath=std::string(prefix)+".results.bin",metaPath=std::string(prefix)+".json";
    std::ofstream raw(rawPath,std::ios::binary);raw.write(reinterpret_cast<const char*>(result.data()),result.size());raw.close();if(!raw)return false;
    std::ofstream meta(metaPath);meta<<"{\"complete\":true,\"frame\":"<<frame<<",\"slot\":"<<view.slot<<",\"count\":"<<count
      <<",\"scene_epoch\":"<<view.scene_epoch<<",\"slot_epoch\":"<<view.slot_epoch<<",\"source_digest\":"<<view.source_digest<<",\"ready\":true,\"ddgi_frame\":"<<ddgi->GetVolumeData().FrameIndex
      <<",\"conservative_query\":"<<(conservative?"true":"false")<<",\"sc_ddgi_four_handles_equal\":true,\"query_file\":\""<<queryPath<<"\",\"output_file\":\""<<rawPath<<"\",\"bindings\":[";
    for(u32 i=0;i<4;++i)meta<<(i?",":"")<<"{\"binding\":"<<13+i<<",\"resource\":"<<view.buffers[i]<<",\"bytes\":"<<view.sizes[i]<<",\"ddgi_source_resource\":"<<ddgi->GetMeasuredGeometrySource(i)<<"}";
    meta<<"],\"scope\":\"Post-frame live SC four-buffer query; no geometry rebuild. Query endpoints are supplied packet, not automatically current renderer receiver identity. No GPU barycentric/vertices exported.\"}\n";meta.close();
    return bool(meta);
}
