// Opt-in Vulkan proof. Does not bind auxiliary radiance to production lookup.
static bool RunSCBoundaryIndexTest(RHIDeviceBase* device) {
    using Index=primal::graphics::lumen::BoundaryPointIndex;
    std::vector<Index::Group> groups{{0,7,{}},{1,7,{}},{0,8,{}},{0,9,{}}};
    uint32_t seed=123456789;
    auto random=[&](){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return float(seed&65535)/32768.f-1.f;};
    for(u32 i=0;i<4096;++i)groups[0].points.push_back({{10*random(),10*random(),10*random()},i|(i%2?0x80000000u:0u)});
    for(u32 i=0;i<8;++i){groups[1].points.push_back({{float(i),0,0},0x80010000u+i});groups[2].points.push_back({{0,float(i),0},0x80020000u+i});}
    // All equal distances defeat pruning: reject traversal exhaustion rather
    // than report an incomplete set. 2048 points require 511 visited nodes.
    groups[3].points.resize(2048,{{0,0,0},0x80030000u});
    for(u32 i=0;i<2048;++i)groups[3].points[i].source=0x80030000u+i;
    std::vector<u32> words;if(!Index::Build(3,41,groups,words))return false;
    struct Query {u32 card,triangle,epoch,pad;float x,y,z,w;};static_assert(sizeof(Query)==32);
    std::vector<Query> queries;
    for(u32 i=0;i<32;++i)queries.push_back({0,7,41,0,8*random(),8*random(),8*random(),0});
    queries.push_back({1,7,41,0,0,0,0,0});queries.push_back({0,8,41,0,0,0,0,0});
    queries.push_back({2,7,41,0,0,0,0,0});queries.push_back({0,100,41,0,0,0,0,0});
    queries.push_back({0,7,40,0,0,0,0,0});queries.push_back({3,7,41,0,0,0,0,0});
    queries.push_back({0,7,41,0,std::bit_cast<float>(0x7fc00000u),0,0,0});
    queries.push_back({0,9,41,0,0,0,0,0});
    const char* fixture=std::getenv("SC_BOUNDARY_INDEX_FIXTURE");
    if(fixture && !fixture[0])fixture=nullptr;
    if(fixture && fixture[0]) {
        std::ifstream input(fixture,std::ios::binary|std::ios::ate);
        auto bytes=input.tellg();if(bytes<24 || bytes>16*1024*1024)return false;
        input.seekg(0);u32 h[6]{};input.read(reinterpret_cast<char*>(h),sizeof(h));
        if(h[0]!=0x53435031u || h[1]!=1 || h[2]==0 || h[2]>Index::MaxCards || h[4]==0 || h[4]>4096 || h[5]==0 || h[5]>4096)return false;
        groups.clear();queries.clear();
        for(u32 i=0;i<h[4];++i){u32 key[4]{};input.read(reinterpret_cast<char*>(key),sizeof(key));
            if(!input || key[2]==0 || key[2]>Index::MaxGroupPoints || key[3]!=0)return false;
            Index::Group g{key[0],key[1],{}};g.points.resize(key[2]);
            input.read(reinterpret_cast<char*>(g.points.data()),g.points.size()*sizeof(Index::Point));if(!input)return false;
            groups.push_back(std::move(g));}
        queries.resize(h[5]);input.read(reinterpret_cast<char*>(queries.data()),queries.size()*sizeof(Query));
        if(!input || input.tellg()!=bytes || !Index::Build(h[2],h[3],groups,words))return false;
    }
    const u32 baseQueryCount=u32(queries.size());
    for(u32 i=0;i<baseQueryCount;++i){auto q=queries[i];q.pad|=1u;queries.push_back(q);}
    const u32 queryCount=u32(queries.size());
    struct Resources {
        RHIDeviceBase* device;
        ResourceHandle buffers[4]{handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE,handles::INVALID_RESOURCE};
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
    const void* data[]={words.data(),queries.data(),nullptr,nullptr};
    const u64 size[]={words.size()*4,queries.size()*sizeof(Query),queryCount*34*4,queryCount*34*4};
    for(u32 i=0;i<4;++i){BufferDesc desc{};desc.size=size[i];desc.type=BufferType::Structured;
        desc.usage=desc.memoryUsage=i==3?GPUMemoryUsage::Readback:GPUMemoryUsage::Static;
        r.buffers[i]=device->CreateBuffer(desc);
        if(r.buffers[i]==handles::INVALID_RESOURCE||(data[i]&&!device->UpdateBufferData(r.buffers[i],data[i],size[i],0)))return false;}
    auto spv=LoadSpvBytes("Engine/Graphics/Vulkan/shaders/Lumen/SCBoundaryIndexTest.comp.spv");if(spv.empty())return false;
    r.shader=device->CreateShader(spv.data(),spv.size(),ShaderStage::Compute,"main");if(r.shader==handles::INVALID_SHADER)return false;
    const u32 numbers[]={0,1,2};DescriptorSetLayoutBinding bindings[3]{};
    for(u32 i=0;i<3;++i)bindings[i]={numbers[i],DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};
    r.layout=device->CreateDescriptorSetLayout({3,bindings});if(r.layout==handles::INVALID_DESCRIPTOR_SET_LAYOUT)return false;
    PipelineLayoutDesc pl{};pl.setLayoutCount=1;pl.setLayouts=&r.layout;r.pl=device->CreatePipelineLayout(pl);if(r.pl==handles::INVALID_PIPELINE_LAYOUT)return false;
    r.set=device->CreateDescriptorSet({r.layout});if(r.set==handles::INVALID_DESCRIPTOR_SET)return false;
    DescriptorBufferInfo infos[3]{};WriteDescriptorSet writes[3]{};
    for(u32 i=0;i<3;++i){infos[i]={r.buffers[i],0,size[i]};writes[i]={r.set,numbers[i],0,1,DescriptorType::StorageBuffer,nullptr,&infos[i]};}
    device->UpdateDescriptorSets(3,writes);ComputePipelineDesc cp{};cp.computeShader=r.shader;cp.layout=r.pl;
    r.pipeline=device->CreateComputePipeline(cp);if(r.pipeline==handles::INVALID_PIPELINE)return false;
    r.command=device->CreateCommandBuffer(CommandQueueType::Graphics);if(r.command==handles::INVALID_COMMAND_BUFFER)return false;
    auto* cmd=static_cast<VulkanDevice*>(device)->GetCommandBuffer(r.command);if(!cmd->Reset()||!cmd->Begin())return false;
    cmd->MemoryBarrier(PipelineStage::Host|PipelineStage::Transfer,PipelineStage::ComputeShader,AccessFlag::HostWrite|AccessFlag::TransferWrite,AccessFlag::ShaderRead);
    cmd->BindComputePipeline(r.pipeline);cmd->BindDescriptorSets(PipelineBindPoint::Compute,r.pl,0,1,&r.set,0,nullptr);cmd->Dispatch((queryCount+7)/8,1,1);
    cmd->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
    cmd->CopyBuffer(r.buffers[2],r.buffers[3],0,0,size[2]);
    cmd->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
    if(!cmd->End()||!cmd->Submit()||!cmd->WaitForCompletion())return false;
    const auto* output=static_cast<const u32*>(device->MapBuffer(r.buffers[3]));if(!output)return false;
    u32 passed=0;
    for(u32 i=0;i<queryCount;++i){const auto& q=queries[i];bool valid=true;
        if(!fixture && i%baseQueryCount>=34){valid=output[34*i]==0 && output[34*i+1]==0;
            for(u32 k=0;k<32;++k)valid=valid && output[34*i+2+k]==0xffffffffu;
        }else{
            const Index::Group* selected=nullptr;
            for(const auto& g:groups)if(g.card==q.card && g.triangle==q.triangle)selected=&g;
            if(!selected){device->UnmapBuffer(r.buffers[3]);return false;}
            const auto& points=selected->points;
            std::vector<std::pair<double,u32>> expected;
            for(const auto& p:points){double dx=double(p.position[0])-q.x,dy=double(p.position[1])-q.y,dz=double(p.position[2])-q.z;
                expected.emplace_back(dx*dx+dy*dy+dz*dz,p.source);}
            std::sort(expected.begin(),expected.end());u32 n=std::min<u32>(32,u32(expected.size()));
            valid=output[34*i]==1 && output[34*i+1]==n;
            for(u32 k=0;k<n;++k){u32 source=output[34*i+2+k];
                if(!fixture){valid=valid && source==expected[k].second;continue;}
                auto at=std::find_if(expected.begin(),expected.end(),[source](const auto& p){return p.second==source;});
                valid=valid && at!=expected.end() && at->first<=expected[n-1].first+1e-5;
                for(u32 j=0;j<k;++j)valid=valid && source!=output[34*i+2+j];
            }
        }
        if(i>=baseQueryCount)for(u32 k=0;k<34;++k)valid=valid && output[34*i+k]==output[34*(i-baseQueryCount)+k];
        if(valid)++passed;else std::cerr<<"[SC_BOUNDARY_INDEX_SELFTEST] failed query "<<i<<" status="<<output[34*i]<<" count="<<output[34*i+1]<<std::endl;
    }
    if(fixture && fixture[0]){
        std::ofstream file(std::string(fixture)+".results.bin",std::ios::binary|std::ios::trunc);
        file.write(reinterpret_cast<const char*>(output),baseQueryCount*34*4);
        if(!file){device->UnmapBuffer(r.buffers[3]);return false;}
    }
    device->UnmapBuffer(r.buffers[3]);
    std::cerr<<"[SC_BOUNDARY_INDEX_SELFTEST] "<<passed<<"/"<<queryCount<< (passed==queryCount?" PASS (both insertion paths bitwise equivalent)":" FAIL")<<std::endl;
    return passed==queryCount;
}
