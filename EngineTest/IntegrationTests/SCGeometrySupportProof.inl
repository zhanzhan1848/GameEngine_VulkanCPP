// Vulkan test-only opt-in diagnostic. Included inside the test anonymous namespace.
// No EngineDLL exports, SurfaceCache bindings or production radiance changes.
struct SCProofTriangle { float a[4], e1[4], e2[4]; };
struct SCProofNode { float lo[4], hi[4]; u32 links[4]; };
struct SCProofResult { float closestDistance[4], baryTriangle[4], status[4]; };
static_assert(sizeof(SCProofTriangle)==48 && sizeof(SCProofNode)==48 && sizeof(SCProofResult)==48);

static bool SCProofFinite(float value) {
    // The test target uses -ffast-math: std::isfinite can be optimized away.
    u32 bits=0; std::memcpy(&bits,&value,4); return (bits&0x7f800000u)!=0x7f800000u;
}

static bool RunSCGeometrySupportProof(RHIDeviceBase* device,
    const std::vector<SCProofTriangle>& scene, const char* prefix) {
    const char* queryPath=std::getenv("SC_GPU_GEOMETRY_QUERIES");
    const bool rays=EnvEnabled("SC_GPU_GEOMETRY_RAYS");
    const size_t queryFloats=rays ? 8u : 4u;
    if (!prefix || !prefix[0] || !queryPath || !queryPath[0] || scene.empty()) return false;
    std::ifstream input(queryPath,std::ios::binary|std::ios::ate);
    if (!input || input.tellg()<16 || input.tellg()>std::streamoff(16+16384*queryFloats*4)) return false;
    const auto bytes=size_t(input.tellg()); input.seekg(0);
    std::vector<u8> file(bytes); input.read(reinterpret_cast<char*>(file.data()),bytes);
    u32 count=0, reserved=0;
    std::memcpy(&count,file.data()+8,4); std::memcpy(&reserved,file.data()+12,4);
    if (!input || std::memcmp(file.data(),rays ? "PGRAYS01" : "PGQUER01",8) || !count || count>16384 || reserved || bytes!=16+size_t(count)*queryFloats*4) return false;
    std::vector<float> queries(size_t(count)*queryFloats);
    std::memcpy(queries.data(),file.data()+16,queries.size()*4);
    for (u32 i=0;i<count;++i) {
        for (u32 k=0;k<queryFloats;++k) if (!SCProofFinite(queries[i*queryFloats+k])) return false;
        if(rays) {
            const auto* q=queries.data()+i*8;
            const float length2=q[4]*q[4]+q[5]*q[5]+q[6]*q[6];
            if(!SCProofFinite(length2) || std::abs(length2-1.f)>2e-5f || q[3]<0 || q[7]<=q[3] || q[7]>10000.f) return false;
        } else if (queries[i*4+3]<=0 || queries[i*4+3]>10) return false;
    }
    u32 stackLimit=64;
    if (const char* requested=std::getenv("SC_GPU_GEOMETRY_STACK_LIMIT")) {
        char* end=nullptr; const auto value=std::strtoul(requested,&end,10);
        if (end==requested || *end || value<1 || value>64) return false;
        stackLimit=u32(value);
    }
    std::vector<SCProofTriangle> triangles=scene;
    std::vector<SCProofNode> nodes;
    auto bounds=[](const SCProofTriangle& t,u32 k,bool maximum) {
        const float b=t.a[k]+t.e1[k], c=t.a[k]+t.e2[k];
        // Separate coordinate rounding slack; never enlarges the hit band.
        return maximum ? std::max({t.a[k],b,c})+1e-4f : std::min({t.a[k],b,c})-1e-4f;
    };
    auto build=[&](auto&& self,u32 begin,u32 end)->u32 {
        const u32 at=u32(nodes.size()); SCProofNode node{};
        for (u32 k=0;k<3;++k) {
            node.lo[k]=std::numeric_limits<float>::max(); node.hi[k]=-node.lo[k];
            for (u32 i=begin;i<end;++i) {
                node.lo[k]=std::min(node.lo[k],bounds(triangles[i],k,false));
                node.hi[k]=std::max(node.hi[k],bounds(triangles[i],k,true));
            }
        }
        nodes.push_back(node);
        if (end-begin<=16) { nodes[at].links[2]=begin; nodes[at].links[3]=end-begin; }
        else {
            u32 axis=0;
            for (u32 k=1;k<3;++k) if (node.hi[k]-node.lo[k]>node.hi[axis]-node.lo[axis]) axis=k;
            const u32 middle=begin+(end-begin)/2;
            std::nth_element(triangles.begin()+begin,triangles.begin()+middle,triangles.begin()+end,
                [&](const auto& a,const auto& b) {
                    const float ca=2*a.a[axis]+a.e1[axis]+a.e2[axis], cb=2*b.a[axis]+b.e1[axis]+b.e2[axis];
                    return ca==cb ? a.a[3]<b.a[3] : ca<cb;
                });
            const auto left=self(self,begin,middle), right=self(self,middle,end);
            nodes[at].links[0]=left; nodes[at].links[1]=right;
        }
        return at;
    };
    build(build,0,u32(triangles.size()));
    std::vector<u8> queryBuffer(16+queries.size()*4);
    const u32 counts[4]={u32(nodes.size()),u32(triangles.size()),count,stackLimit};
    std::memcpy(queryBuffer.data(),counts,16); std::memcpy(queryBuffer.data()+16,queries.data(),queries.size()*4);
    struct Resources {
        RHIDeviceBase* device;
        ResourceHandle buffers[8]{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};
        ShaderHandle shader{handles::INVALID_SHADER};
        DescriptorSetLayoutHandle layout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
        PipelineLayoutHandle pipelineLayout{handles::INVALID_PIPELINE_LAYOUT};
        DescriptorSetHandle set{handles::INVALID_DESCRIPTOR_SET};
        PipelineHandle pipeline{handles::INVALID_PIPELINE};
        CommandBufferHandle command{handles::INVALID_COMMAND_BUFFER};
        ~Resources() {
            if(command!=handles::INVALID_COMMAND_BUFFER) device->DestroyCommandBuffer(command);
            if(pipeline!=handles::INVALID_PIPELINE) device->DestroyPipeline(pipeline);
            if(set!=handles::INVALID_DESCRIPTOR_SET) device->DestroyDescriptorSet(set);
            if(pipelineLayout!=handles::INVALID_PIPELINE_LAYOUT) device->DestroyPipelineLayout(pipelineLayout);
            if(layout!=handles::INVALID_DESCRIPTOR_SET_LAYOUT) device->DestroyDescriptorSetLayout(layout);
            if(shader!=handles::INVALID_SHADER) device->DestroyShader(shader);
            for(auto b:buffers) if(b!=handles::INVALID_RESOURCE) device->DestroyBuffer(b);
        }
    } resources{device};
    const u64 sizes[8]={nodes.size()*48,triangles.size()*48,queryBuffer.size(),u64(count)*48,u64(count)*48,nodes.size()*48,triangles.size()*48,queryBuffer.size()};
    const void* sources[3]={nodes.data(),triangles.data(),queryBuffer.data()};
    for(u32 i=0;i<8;++i) {
        BufferDesc desc{}; desc.size=sizes[i]; desc.type=BufferType::Structured;
        desc.usage=desc.memoryUsage=i>=4 ? GPUMemoryUsage::Readback : GPUMemoryUsage::Static;
        resources.buffers[i]=device->CreateBuffer(desc);
        if(resources.buffers[i]==handles::INVALID_RESOURCE ||
            (i<3 && !device->UpdateBufferData(resources.buffers[i],sources[i],sizes[i],0))) return false;
    }
    const bool echo=EnvEnabled("SC_GPU_GEOMETRY_ECHO");
    const bool trace=EnvEnabled("SC_GPU_GEOMETRY_TRACE");
    const bool fixed=EnvEnabled("SC_GPU_GEOMETRY_FIXED_ARRAYS");
    if (u32(echo)+u32(trace)+u32(fixed)+u32(rays)>1) return false;
    if (fixed && (nodes.size()!=33013 || triangles.size()!=262267 || count!=94)) return false;
    auto spv=LoadSpvBytes(rays ? "Engine/Graphics/Vulkan/shaders/Lumen/SCGeometrySupportProof_ray.comp.spv"
                       : (echo ? "Engine/Graphics/Vulkan/shaders/Lumen/SCGeometrySupportProof_echo.comp.spv"
                             : (trace ? "Engine/Graphics/Vulkan/shaders/Lumen/SCGeometrySupportProof_trace.comp.spv"
                                      : (fixed ? "Engine/Graphics/Vulkan/shaders/Lumen/SCGeometrySupportProof_fixed.comp.spv"
                                               : "Engine/Graphics/Vulkan/shaders/Lumen/SCGeometrySupportProof.comp.spv"))));
    if(spv.empty()) return false;
    resources.shader=device->CreateShader(spv.data(),spv.size(),ShaderStage::Compute,"main");
    if(resources.shader==handles::INVALID_SHADER) return false;
    DescriptorSetLayoutBinding bindings[4]{};
    for(u32 i=0;i<4;++i) bindings[i]={i,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};
    DescriptorSetLayoutDesc layout{4,bindings}; resources.layout=device->CreateDescriptorSetLayout(layout);
    if(resources.layout==handles::INVALID_DESCRIPTOR_SET_LAYOUT) return false;
    PipelineLayoutDesc pl{}; pl.setLayoutCount=1; pl.setLayouts=&resources.layout;
    resources.pipelineLayout=device->CreatePipelineLayout(pl);
    if(resources.pipelineLayout==handles::INVALID_PIPELINE_LAYOUT) return false;
    DescriptorSetDesc sd{}; sd.layout=resources.layout; resources.set=device->CreateDescriptorSet(sd);
    if(resources.set==handles::INVALID_DESCRIPTOR_SET) return false;
    DescriptorBufferInfo infos[4]{}; WriteDescriptorSet writes[4]{};
    for(u32 i=0;i<4;++i) {
        infos[i]={resources.buffers[i],0,sizes[i]};
        writes[i]={resources.set,i,0,1,DescriptorType::StorageBuffer,nullptr,&infos[i]};
    }
    device->UpdateDescriptorSets(4,writes);
    ComputePipelineDesc cp{}; cp.computeShader=resources.shader; cp.layout=resources.pipelineLayout;
    resources.pipeline=device->CreateComputePipeline(cp);
    if(resources.pipeline==handles::INVALID_PIPELINE) return false;
    resources.command=device->CreateCommandBuffer(CommandQueueType::Graphics);
    if(resources.command==handles::INVALID_COMMAND_BUFFER) return false;
    auto* cmd=static_cast<VulkanDevice*>(device)->GetCommandBuffer(resources.command);
    if(!cmd->Reset() || !cmd->Begin()) return false;
    cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::Transfer,PipelineStage::ComputeShader,
                       AccessFlag::HostWrite|AccessFlag::TransferWrite,AccessFlag::ShaderRead);
    ResourceBarrier barrier{}; barrier.resource=resources.buffers[3];
    barrier.beforeState=ResourceState::Unknown; barrier.afterState=ResourceState::UnorderedAccess;
    cmd->InsertBarrier(&barrier,1);
    cmd->BindComputePipeline(resources.pipeline);
    cmd->BindDescriptorSets(PipelineBindPoint::Compute,resources.pipelineLayout,0,1,&resources.set,0,nullptr);
    cmd->Dispatch((count+31)/32,1,1);
    // Vulkan InsertBarrier currently skips buffers (texture-only backend).
    // A fence waits for completion but cannot repair an earlier racing copy.
    cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,
                       AccessFlag::ShaderWrite,AccessFlag::TransferRead);
    barrier.beforeState=ResourceState::UnorderedAccess; barrier.afterState=ResourceState::CopySource;
    cmd->InsertBarrier(&barrier,1);
    ResourceBarrier destination{}; destination.resource=resources.buffers[4];
    destination.beforeState=ResourceState::Unknown; destination.afterState=ResourceState::CopyDest;
    cmd->InsertBarrier(&destination,1);
    cmd->CopyBuffer(resources.buffers[3],resources.buffers[4],0,0,sizes[3]);
    // Independently read back every resident input, not just CPU upload bytes.
    cmd->MemoryBarrier(PipelineStage::Host,PipelineStage::Transfer,AccessFlag::HostWrite,AccessFlag::TransferRead);
    for(u32 i=0;i<3;++i) {
        ResourceBarrier source{}; source.resource=resources.buffers[i];
        source.beforeState=ResourceState::ShaderResource; source.afterState=ResourceState::CopySource;
        cmd->InsertBarrier(&source,1);
        destination.resource=resources.buffers[i+5]; cmd->InsertBarrier(&destination,1);
        cmd->CopyBuffer(resources.buffers[i],resources.buffers[i+5],0,0,sizes[i]);
    }
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
    if(!cmd->End() || !cmd->Submit() || !cmd->WaitForCompletion()) return false;
    const void* mapped=device->MapBuffer(resources.buffers[4]);
    if(!mapped) return false;
    std::vector<u8> output(sizes[3]); std::memcpy(output.data(),mapped,output.size());
    device->UnmapBuffer(resources.buffers[4]);
    std::vector<u8> resident[3];
    for(u32 i=0;i<3;++i) {
        const void* data=device->MapBuffer(resources.buffers[i+5]);
        if(!data) return false;
        resident[i].resize(sizes[i]); std::memcpy(resident[i].data(),data,sizes[i]);
        device->UnmapBuffer(resources.buffers[i+5]);
        if(std::memcmp(resident[i].data(),sources[i],sizes[i])) return false;
    }
    auto write=[&](const char* suffix,const void* data,size_t size) {
        std::ofstream file(std::string(prefix)+suffix,std::ios::binary|std::ios::trunc);
        file.write(static_cast<const char*>(data),size); return bool(file);
    };
    std::vector<float> oracle; oracle.reserve(scene.size()*9);
    for(const auto& tri:scene) for(const float* v:{tri.a,tri.e1,tri.e2}) oracle.insert(oracle.end(),v,v+3);
    if(!write(".raw",output.data(),output.size()) || !write(".nodes.bin",resident[0].data(),sizes[0]) ||
       !write(".triangles.bin",resident[1].data(),sizes[1]) || !write(".queries.bin",resident[2].data(),sizes[2]) ||
       !write(".scene-triangles.bin",oracle.data(),oracle.size()*4)) return false;
    std::ofstream meta(std::string(prefix)+".json");
    meta << "{\"complete\":true,\"queries\":" << count << ",\"triangles\":" << triangles.size()
         << ",\"nodes\":" << nodes.size() << ",\"stack_limit\":" << stackLimit
         << ",\"result_stride\":48,\"geometry_stride\":48,\"node_stride\":48,\"geometry_bytes\":" << sizes[0]+sizes[1]
         << ",\"fixed_array_diagnostic\":" << (fixed?"true":"false")
         << ",\"ray_geometry_diagnostic\":" << (rays?"true":"false")
         << ",\"resident_inputs_bit_identical\":true,\"gpu_distance_tolerance_m\":0.00002,\"default_changed\":false,\"alpha_certified\":false,\"ray_first_hit_certified\":false}";
    PRIMAL_LOG_STREAM(Info,"Render") << "[SC_GPU_GEOMETRY_PROOF] complete queries=" << count
        << " triangles=" << triangles.size() << " nodes=" << nodes.size() << " stack=" << stackLimit;
    return bool(meta);
}
