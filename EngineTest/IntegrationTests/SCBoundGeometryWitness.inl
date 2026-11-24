// Strict opt-in: post-frame query on the live SC-owned buffers, no scene rebuild.
static bool RunSCBoundGeometryWitness(RHIDeviceBase* device,
    primal::graphics::lumen::SurfaceCachePass* sc,
    primal::graphics::lumen::LumenDDGIPass* ddgi,u32 frame,const char* prefix,const char* queryPath,
    bool sceneOnly=false) {
    if (!device || !sc || !ddgi || !prefix) return false;
    const bool d3d12 = device->GetPlatform() == RHIPlatform::D3D12;
    if (d3d12 && !sceneOnly) return false;
    if (!d3d12 && device->GetPlatform() != RHIPlatform::Vulkan) return false;
    const auto view = sc->GetGeometryWitnessView(frame);
    if (!view.ready || !view.source_digest || !view.scene_epoch || view.slot_epoch != view.scene_epoch ||
        view.slot != frame % 3u) return false;
    bool sameSource = true;
    for (u32 i = 0; i < 4; ++i) {
        u64 bytes = 0;
        if (!device->GetBufferByteSize(view.buffers[i], bytes) || !bytes || bytes != view.sizes[i]) return false;
        sameSource &= view.buffers[i] == ddgi->GetMeasuredGeometrySource(i);
    }
    if (!sceneOnly && (!sameSource || ddgi->GetVolumeData().FrameIndex != frame)) return false;

    std::vector<float> rays;
    if (sceneOnly) {
        // One workgroup. Six world-axis rays from the locked camera and a ray
        // outside the scene pointing away from its bounds (known miss).
        for (u32 axis = 0; axis < 3; ++axis) for (float sign : {-1.f, 1.f}) {
            float r[8] = {0.f,5.f,0.f,0.f,0.f,0.f,0.f,100.f};
            r[4 + axis] = sign;
            rays.insert(rays.end(), r, r+8);
        }
        rays.insert(rays.end(), {0.f,1000.f,0.f,0.f,0.f,1.f,0.f,100.f});
    } else {
        if (!queryPath) return false;
        std::ifstream file(queryPath,std::ios::binary|std::ios::ate);
        if(!file||file.tellg()<16||file.tellg()>8208)return false;
        const auto packetBytes=size_t(file.tellg());std::vector<u8> packet(packetBytes);file.seekg(0);file.read(reinterpret_cast<char*>(packet.data()),packetBytes);
        u32 count=0,reserved=0;std::memcpy(&count,packet.data()+8,4);std::memcpy(&reserved,packet.data()+12,4);
        if(!file||std::memcmp(packet.data(),"PGRAYS01",8)||count<2||count>256||reserved||packetBytes!=16+count*32)return false;
        rays.resize(count*8);std::memcpy(rays.data(),packet.data()+16,rays.size()*4);
    }
    const u32 count = u32(rays.size()/8);
    const u64 bytes = 16 + rays.size()*sizeof(float);
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
    auto buffer=[&](u64 size,GPUMemoryUsage memory,u32 flags) {
        BufferDesc b{};b.size=size;b.type=BufferType::Structured;b.usage=b.memoryUsage=memory;
        b.bindFlags=flags;return device->CreateBuffer(b);
    };
    const char* conservativeSetting=std::getenv("SC_BOUND_GEOMETRY_CONSERVATIVE");
    const bool conservative=conservativeSetting && std::strcmp(conservativeSetting,"1")==0;
    if (d3d12 && conservative) return false;
    const bool includeSurface = sceneOnly && !conservative && sc->GetCardGenerator().HasOwnerCardDirectory();
    if (sceneOnly && !includeSurface)
        PRIMAL_LOG_STREAM(Info,"Render") << "[F350SCMeasuredSurface] unavailable: owner directory or shader variant";
    const u64 outputBytes=u64(count)*(includeSurface ? 128 : sceneOnly ? 96 : 64);
    r.query=buffer(bytes,GPUMemoryUsage::Dynamic,u32(BufferUsageFlags::Storage));
    r.out=buffer(outputBytes,GPUMemoryUsage::Static,u32(ResourceUsage::UnorderedAccess) |
        u32(BufferUsageFlags::Storage) | u32(BufferUsageFlags::TransferSrc));
    r.readback=buffer(outputBytes,GPUMemoryUsage::Readback,u32(BufferUsageFlags::TransferDst));
    if(r.query==handles::INVALID_RESOURCE||r.out==handles::INVALID_RESOURCE||r.readback==handles::INVALID_RESOURCE)return false;
    u32 header[4]={count | (sceneOnly ? 0x80000000u : 0u),frame,view.slot,u32(view.scene_epoch)};
    auto* mapped=device->MapBuffer(r.query);if(!mapped)return false;
    std::memcpy(mapped,header,16);std::memcpy(static_cast<u8*>(mapped)+16,rays.data(),rays.size()*4);device->UnmapBuffer(r.query);
    std::string shaderPath = d3d12 ? "EngineTest/shaders/d3d12/Tests/SCBoundGeometryWitness.comp.cso"
        : conservative ? SC_BOUND_GEOMETRY_WITNESS_REPLACE_SPV : SC_BOUND_GEOMETRY_WITNESS_SPV;
    if (includeSurface) {
        shaderPath = d3d12 ? "EngineTest/shaders/d3d12/Tests/SCBoundSurfaceWitness.comp.cso"
            : (std::filesystem::path(shaderPath).parent_path() / "SCBoundSurfaceWitness.comp.spv").string();
    }
    auto spv=LoadSpvBytes(shaderPath.c_str());if(spv.empty())return false;
    r.shader=device->CreateShader(spv.data(),spv.size(),ShaderStage::Compute,"main");if(r.shader==handles::INVALID_SHADER)return false;

    constexpr u32 locations[] = {0,1,13,14,15,16,2,3,4,5};
    const u32 bindingCount = includeSurface ? 10u : 6u;
    DescriptorSetLayoutBinding bindings[10]{};
    for (u32 i=0; i<bindingCount; ++i) {
        bindings[i] = {locations[i], i<8 ? DescriptorType::StorageBuffer : DescriptorType::SampledImage,
                       1, ShaderStage::Compute, nullptr};
        bindings[i].readonly = i != 1;
    }
    r.sl=device->CreateDescriptorSetLayout({bindingCount,bindings});if(r.sl==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
    r.pl=device->CreatePipelineLayout({1,&r.sl});if(r.pl==handles::INVALID_PIPELINE_LAYOUT)return false;
    r.set=device->CreateDescriptorSet({r.sl});if(r.set==handles::INVALID_DESCRIPTOR_SET)return false;
    DescriptorBufferInfo infos[8]={{r.query,0,bytes},{r.out,0,outputBytes}};
    for(u32 i=0;i<4;++i)infos[i+2]={view.buffers[i],0,view.sizes[i]};
    DescriptorImageInfo images[2]{};
    const auto cache = sc->GetCacheView(frame);
    if (includeSurface) {
        if (!cache.card_count || !cache.lookup_count ||
            cache.depth_atlas == handles::INVALID_RESOURCE || cache.lighting_atlas == handles::INVALID_RESOURCE) return false;
        using namespace primal::graphics::lumen;
        infos[6]={cache.card_data,0,u64(cache.card_count)*sizeof(SurfaceCacheCard)};
        infos[7]={cache.card_lookup,0,u64(cache.lookup_count)*sizeof(SurfaceCacheCardLookup)};
        for (u32 i=6; i<8; ++i) {
            u64 available=0;
            if (!device->GetBufferByteSize(infos[i].buffer,available) || infos[i].range>available) return false;
        }
        images[0]={handles::INVALID_SAMPLER,cache.depth_atlas,ResourceState::ShaderResource};
        images[1]={handles::INVALID_SAMPLER,cache.lighting_atlas,ResourceState::ShaderResource};
    }
    WriteDescriptorSet writes[10]{};
    for (u32 i=0; i<bindingCount; ++i)
        writes[i]={r.set,locations[i],0,1,bindings[i].descriptorType,i<8 ? nullptr : &images[i-8],
                   i<8 ? &infos[i] : nullptr};
    device->UpdateDescriptorSets(bindingCount,writes);
    ComputePipelineDesc pd{};pd.computeShader=r.shader;pd.layout=r.pl;r.pipe=device->CreateComputePipeline(pd);if(r.pipe==handles::INVALID_PIPELINE)return false;
    r.cmd=device->CreateCommandBuffer(CommandQueueType::Graphics);auto* cmd=primal::graphics::rhi::GetCommandBuffer(r.cmd);if(!cmd||!cmd->Begin())return false;
    cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::HostWrite|AccessFlag::ShaderWrite,AccessFlag::ShaderRead);
    if (d3d12 && includeSurface) {
        cmd->RequireBufferState(cache.card_data, ResourceState::ShaderResource);
        cmd->RequireBufferState(cache.card_lookup, ResourceState::ShaderResource);
        ResourceBarrier readable[2]{};
        for (u32 i=0; i<2; ++i) {
            readable[i].resource = i ? cache.lighting_atlas : cache.depth_atlas;
            readable[i].beforeState = ResourceState::Unknown;
            readable[i].afterState = ResourceState::ShaderResource;
            readable[i].subresource = 0xffffffffu;
        }
        cmd->InsertBarrier(readable,2);
    }
    if (d3d12) {
        ResourceBarrier output{};
        output.resource=r.out;output.beforeState=ResourceState::Unknown;output.afterState=ResourceState::UnorderedAccess;
        cmd->InsertBarrier(&output,1);
    }
    cmd->BindComputePipeline(r.pipe);cmd->BindDescriptorSets(PipelineBindPoint::Compute,r.pl,0,1,&r.set,0,nullptr);cmd->Dispatch((count+7)/8,1,1);
    cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);cmd->CopyBuffer(r.out,r.readback,0,0,outputBytes);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
    if(!cmd->End()||!cmd->Submit()||!cmd->WaitForCompletion())return false;
    mapped=device->MapBuffer(r.readback,0,outputBytes);if(!mapped)return false;std::vector<u8> result(outputBytes);std::memcpy(result.data(),mapped,outputBytes);device->UnmapBuffer(r.readback);
    // Echo and generation are mandatory, including MISS/UNKNOWN; never certify stale tails.
    for(u32 i=0;i<count;++i){const auto* record=result.data()+i*64;u32 identity[4];std::memcpy(identity,record+48,16);
        if(std::memcmp(record,rays.data()+i*8,32)||identity[0]!=i||identity[1]!=frame||identity[2]!=view.slot||identity[3]!=u32(view.scene_epoch))return false;}
    if (sceneOnly) {
        bool sawHit = false;
        for (u32 i=0; i<count; ++i) {
            float hit[2];u32 ids[2];
            std::memcpy(hit,result.data()+i*64+32,sizeof(hit));
            std::memcpy(ids,result.data()+i*64+40,sizeof(ids));
            if (hit[0] != 0.f && hit[0] != 1.f) return false;
            sawHit |= hit[0] == 1.f;
            if (hit[0] == 1.f && (!std::isfinite(hit[1]) || hit[1] <= 0 || hit[1] > rays[i*8+7])) return false;
            if (i+1==count && hit[0]!=0.f) return false;
            const u8* measured = result.data() + count * 64 + i * 32;
            float segment[2], normal[3]; u32 identity[2];
            std::memcpy(segment, measured, sizeof(segment));
            std::memcpy(identity, measured + 8, sizeof(identity));
            std::memcpy(normal, measured + 16, sizeof(normal));
            const float normalLength2 = normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2];
            const bool agrees = hit[0] == segment[0] && (hit[0] == 0.f ||
                (std::abs(hit[1] - segment[1]) <= 0.0001f * std::max(1.f, hit[1]) &&
                 ids[0] == identity[0] && ids[1] == identity[1] && std::abs(normalLength2 - 1.f) < 0.001f));
            PRIMAL_LOG_STREAM(Info,"Render") << "[F350DDGIRay] index=" << i << " status=" << segment[0]
                << " distance=" << segment[1] << " owner=" << identity[0] << " primitive=" << identity[1]
                << " normal=" << normal[0] << "," << normal[1] << "," << normal[2] << " agrees=" << agrees;
            if (!agrees) return false;
            if (includeSurface) {
                // A rejected surface has a valid geometric distance but no verified radiance.
                const u8* surface = result.data() + count*96 + i*32;
                float light[3]; u32 card, proof[4];
                std::memcpy(light,surface,sizeof(light));
                std::memcpy(&card,surface+12,sizeof(card));
                std::memcpy(proof,surface+16,sizeof(proof));
                if (proof[3]!=i || proof[0]>16 || proof[1]>proof[0] ||
                    (proof[1] ? card>=cache.card_count : card!=0xffffffffu) ||
                    !std::isfinite(light[0]) || !std::isfinite(light[1]) || !std::isfinite(light[2])) return false;
                PRIMAL_LOG_STREAM(Info,"Render") << "[F350SCMeasuredSurface] index=" << i
                    << " candidates=" << proof[0] << " accepted=" << proof[1] << " reject=" << proof[2]
                    << " card=" << card << " radiance=" << light[0] << "," << light[1] << "," << light[2]
                    << " lighting=" << cache.lighting_atlas << " depth=" << cache.depth_atlas;
            }
            PRIMAL_LOG_STREAM(Info,"Render") << "[F350SceneRay] frame=" << frame << " slot=" << view.slot
                << " epoch=" << view.scene_epoch << " digest=" << view.source_digest << " index=" << i
                << " status=" << hit[0] << " distance=" << hit[1] << " owner=" << ids[0] << " primitive=" << ids[1]
                << " ddgi_source_matches=" << sameSource;
        }
        return sawHit;
    }
    const std::string rawPath=std::string(prefix)+".results.bin",metaPath=std::string(prefix)+".json";
    std::ofstream raw(rawPath,std::ios::binary);raw.write(reinterpret_cast<const char*>(result.data()),result.size());raw.close();if(!raw)return false;
    std::ofstream meta(metaPath);meta<<"{\"complete\":true,\"frame\":"<<frame<<",\"slot\":"<<view.slot<<",\"count\":"<<count
      <<",\"scene_epoch\":"<<view.scene_epoch<<",\"slot_epoch\":"<<view.slot_epoch<<",\"source_digest\":"<<view.source_digest<<",\"ready\":true,\"ddgi_frame\":"<<ddgi->GetVolumeData().FrameIndex
      <<",\"conservative_query\":"<<(conservative?"true":"false")<<",\"sc_ddgi_four_handles_equal\":true,\"query_file\":\""<<queryPath<<"\",\"output_file\":\""<<rawPath<<"\",\"bindings\":[";
    for(u32 i=0;i<4;++i)meta<<(i?",":"")<<"{\"binding\":"<<13+i<<",\"resource\":"<<view.buffers[i]<<",\"bytes\":"<<view.sizes[i]<<",\"ddgi_source_resource\":"<<ddgi->GetMeasuredGeometrySource(i)<<"}";
    meta<<"],\"scope\":\"Post-frame live SC four-buffer query; no geometry rebuild. Query endpoints are supplied packet, not automatically current renderer receiver identity. No GPU barycentric/vertices exported.\"}\n";meta.close();
    return bool(meta);
}
