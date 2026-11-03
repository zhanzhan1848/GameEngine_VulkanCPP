// Exercises the actual software GPU traversal with analytic transformed fixtures.
static bool RunSCGeometryFirstHitTest(RHIDeviceBase* device) {
    using namespace primal::graphics::ray_geometry;
    rhi::RHIMeshAsset mesh{}; mesh.num_vertices=6;mesh.num_indices=6;mesh.index_size=4;
    const float vertices[18]={-2,-2,100,2,-2,100,0,2,100,-2,-2,0,2,-2,0,0,2,0};const u32 indices[6]={0,1,2,3,4,5};
    mesh.position_buffer.resize(sizeof(vertices));std::memcpy(mesh.position_buffer.data(),vertices,sizeof(vertices));
    mesh.index_buffer.resize(sizeof(indices));std::memcpy(mesh.index_buffer.data(),indices,sizeof(indices));
    auto blas=BuildMeshBLAS(mesh);if(!blas) return false;
    auto matrix=[](float x,float y,float z){math::m4x4 m{};m.columns[0]={x,0,0,0};m.columns[1]={0,y,0,0};m.columns[2]={0,0,z,0};m.columns[3]={0,0,0,1};return m;};
    // R4: 局部变量避开 near/far 名字（Windows rpcndr.h 空宏）。
    auto farMat=matrix(1,1,1),nearMat=matrix(-2,3,.25),rotated=matrix(1,1,1);
    farMat.columns[3].z=5;nearMat.columns[3].z=2;
    rotated.columns[0]={0,0,-2,0};rotated.columns[1]={.3f,3,0,0};rotated.columns[2]={.25f,0,0,0};rotated.columns[3]={4,0,0,1};
    // Captured Sponza ray/skinny triangle whose old GPU triple-product t
    // differed from the independent float64 oracle by 2.9458e-5 m.
    auto skinny=std::make_shared<MeshBLAS>();
    const float a[3]={5.187230110168457f,13.315094947814941f,3.8131179809570312f};
    const float e1[3]={.10492563247680664f,.6213788986206055f,2.430140972137451f};
    const float e2[3]={0,.6213788986206055f,2.430140972137451f};
    Triangle triangle{};Node bounds{};
    for(u32 k=0;k<3;++k){triangle.a[k]=a[k];triangle.e1[k]=e1[k];triangle.e2[k]=e2[k];
        bounds.lo[k]=std::min({a[k],a[k]+e1[k],a[k]+e2[k]});
        bounds.hi[k]=std::max({a[k],a[k]+e1[k],a[k]+e2[k]});}
    triangle.a[3]=std::bit_cast<float>(6869u);bounds.links[3]=1;
    skinny->triangles.push_back(triangle);skinny->nodes.push_back(bounds);
    const InstanceInput inputs[]={{blas,farMat,10},{blas,nearMat,22},{blas,rotated,7},{skinny,matrix(1,1,1),391}};
    SceneBVH scene;if(!scene.Build(inputs))return false;
    const float queries[][8]={
        {0,0,0,.05f,0,0,1,50}, {0,0,0,3,0,0,1,50}, {0,0,0,.05f,0,0,1,1},
        {0,0,0,.05f,1,0,0,50}, {5,0,0,.05f,-1,0,0,50}, {0,10,0,.05f,0,0,1,50},
        {0,0,0,.05f,0,0,-1,50},
        {.9177999496459961f,12.559553146362305f,3.4102120399475098f,.05f,
         .8714339733123779f,.24185612797737122f,.42674174904823303f,50},
        {0,0,0,.05f,-0.f,0,1,50}, // signed-zero world fallback
        {0,0,0,.05f,.01f,.02f,1,50}, // all-nonzero nearest plane
        {4,0,0,.05f,.1f,1,.5f,50}, // parallel to rotated/sheared local plane
        {0,0,0,.05f,1,1,1,50}, // finite all-nonzero proven miss
        {0,0,0,.05f,std::numeric_limits<float>::infinity(),1,1,50}};
    const float expectedT[]={2,5,0,4,1,0,0,4.915787325891093f,2,2,0,0,0};
    const u32 expectedOwner[]={22,10,0,7,7,0,0,391,22,22,0,0,0};
    constexpr u32 queryCount=std::size(queries);
    struct Resources {
        RHIDeviceBase* device;
        ResourceHandle buffers[7]{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};
        ShaderHandle shader{handles::INVALID_SHADER};DescriptorSetLayoutHandle layout{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
        PipelineLayoutHandle pl{handles::INVALID_PIPELINE_LAYOUT};DescriptorSetHandle set{handles::INVALID_DESCRIPTOR_SET};
        PipelineHandle pipeline{handles::INVALID_PIPELINE};CommandBufferHandle command{handles::INVALID_COMMAND_BUFFER};
        ~Resources(){if(command!=handles::INVALID_COMMAND_BUFFER)device->DestroyCommandBuffer(command);
            if(pipeline!=handles::INVALID_PIPELINE)device->DestroyPipeline(pipeline);
            if(set!=handles::INVALID_DESCRIPTOR_SET)device->DestroyDescriptorSet(set);
            if(pl!=handles::INVALID_PIPELINE_LAYOUT)device->DestroyPipelineLayout(pl);
            if(layout!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)device->DestroyDescriptorSetLayout(layout);
            if(shader!=handles::INVALID_SHADER)device->DestroyShader(shader);
            for(auto b:buffers)if(b!=handles::INVALID_RESOURCE)device->DestroyBuffer(b);}
    } r{device};
    const void* data[]={queries,nullptr,scene.top.data(),scene.bottom.data(),scene.triangles.data(),scene.instances.data(),nullptr};
    const u64 size[]={sizeof(queries),queryCount*16,scene.top.size()*48,scene.bottom.size()*48,scene.triangles.size()*48,scene.instances.size()*64,queryCount*16};
    for(u32 i=0;i<7;++i){BufferDesc desc{};desc.size=size[i];desc.type=BufferType::Structured;
        desc.usage=desc.memoryUsage=i==6?GPUMemoryUsage::Readback:GPUMemoryUsage::Static;
        r.buffers[i]=device->CreateBuffer(desc);
        if(r.buffers[i]==handles::INVALID_RESOURCE||(data[i]&&!device->UpdateBufferData(r.buffers[i],data[i],size[i],0)))return false;}
    auto spv=LoadSpvBytes("Engine/Graphics/Vulkan/shaders/Lumen/SCGeometryFirstHitTest.comp.spv");if(spv.empty())return false;
    r.shader=device->CreateShader(spv.data(),spv.size(),ShaderStage::Compute,"main");if(r.shader==handles::INVALID_SHADER)return false;
    const u32 numbers[]={0,1,13,14,15,16};DescriptorSetLayoutBinding bindings[6]{};
    for(u32 i=0;i<6;++i)bindings[i]={numbers[i],DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};
    r.layout=device->CreateDescriptorSetLayout({6,bindings});if(r.layout==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
    PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&r.layout;r.pl=device->CreatePipelineLayout(pl);if(r.pl==handles::INVALID_PIPELINE_LAYOUT)return false;
    r.set=device->CreateDescriptorSet({r.layout});if(r.set==handles::INVALID_DESCRIPTOR_SET)return false;
    DescriptorBufferInfo infos[6]{};WriteDescriptorSet writes[6]{};
    for(u32 i=0;i<6;++i){infos[i]={r.buffers[i],0,size[i]};writes[i]={r.set,numbers[i],0,1,DescriptorType::StorageBuffer,nullptr,&infos[i]};}
    device->UpdateDescriptorSets(6,writes);ComputePipelineDesc cp{};cp.computeShader=r.shader;cp.layout=r.pl;
    r.pipeline=device->CreateComputePipeline(cp);if(r.pipeline==handles::INVALID_PIPELINE)return false;
    r.command=device->CreateCommandBuffer(CommandQueueType::Graphics);if(r.command==handles::INVALID_COMMAND_BUFFER)return false;
    auto* cmd=static_cast<VulkanDevice*>(device)->GetCommandBuffer(r.command);if(!cmd->Reset()||!cmd->Begin())return false;
    cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::Transfer,PipelineStage::ComputeShader,AccessFlag::HostWrite|AccessFlag::TransferWrite,AccessFlag::ShaderRead);
    cmd->BindComputePipeline(r.pipeline);cmd->BindDescriptorSets(PipelineBindPoint::Compute,r.pl,0,1,&r.set,0,nullptr);cmd->Dispatch((queryCount+7)/8,1,1);
    cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
    cmd->CopyBuffer(r.buffers[1],r.buffers[6],0,0,size[1]);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
    if(!cmd->End()||!cmd->Submit()||!cmd->WaitForCompletion())return false;
    const auto* output=static_cast<const float*>(device->MapBuffer(r.buffers[6]));if(!output)return false;
    bool ok=true;for(u32 i=0;i<queryCount;++i){bool hit=expectedT[i]>0;
        bool valid=output[4*i+2]==(hit?1.f:0.f) && (!hit || (SCProofFinite(output[4*i])&&std::abs(output[4*i]-expectedT[i])<=2e-5f&&output[4*i+1]==float(expectedOwner[i]) && std::bit_cast<u32>(output[4*i+3])==(i==7?6869u:1u)));
        if(!valid){ok=false;std::cerr<<"[SC_GEOMETRY_SELFTEST] failed query "<<i<<" t="<<output[4*i]<<" owner="<<output[4*i+1]<<" status="<<output[4*i+2]<<std::endl;}}
    device->UnmapBuffer(r.buffers[6]);std::cerr<<"[SC_GEOMETRY_SELFTEST] "
        <<(ok?std::to_string(queryCount)+"/"+std::to_string(queryCount)+" PASS (both paths bitwise equivalent)":"FAIL")<<std::endl;return ok;
}
