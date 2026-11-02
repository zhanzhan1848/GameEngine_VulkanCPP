// Included by the real Gather fixture only. Direct low-level shaders exercise
// floating-point inputs that CPU scene validation intentionally rejects.
void registerInstanceNormalNumericCases(const std::shared_ptr<TestSuite>& suite){
    suite->AddTestCase(TestCase("cache_gpu_float_boundaries_and_bit_mismatch",[]{
        GPU g;
        constexpr u32 count=12;
        std::array<ray_geometry::Triangle,count> tris{};
        std::array<ray_geometry::Instance,count> inst{};
        std::array<u32,count> owners{};
        for(u32 i=0;i<count;++i){tris[i].e1[0]=1;tris[i].e2[1]=1;inst[i].inverse_rows[0][0]=1;inst[i].inverse_rows[1][1]=1;inst[i].inverse_rows[2][2]=1;owners[i]=i;}
        // 0 identity, 1 signed zeros, 2 mirror+nonuniform, 3 skinny,
        // 4 degenerate, 5 subnormal/underflow, 6 NaN, 7 Inf,
        // 8 overflowing world length, 9 invalid owner, 10 negative inverse,
        // 11 very small but representable normal.
        tris[1].e1[1]=-0.f;tris[1].e2[0]=-0.f;
        inst[2].inverse_rows[0][0]=-2.f;inst[2].inverse_rows[1][1]=.5f;inst[2].inverse_rows[2][2]=3.f;
        tris[3].e1[1]=1.f;tris[3].e2[0]=1.f;tris[3].e2[1]=1.00001f;
        tris[4].e2[1]=0.f;
        tris[5].e1[0]=std::numeric_limits<float>::denorm_min();
        tris[6].e1[0]=std::numeric_limits<float>::quiet_NaN();
        tris[7].e1[0]=std::numeric_limits<float>::infinity();
        inst[8].inverse_rows[2][2]=std::numeric_limits<float>::max();owners[9]=count;
        inst[10].inverse_rows[2][2]=-3.f;tris[11].e1[0]=1e-10f;
        std::vector<u32> nan(count*4,0x7fc00001u),flags(count,0);
        ResourceHandle bufs[]={g.buffer(sizeof(tris),BufferType::Structured,GPUMemoryUsage::Dynamic,tris.data()),g.buffer(sizeof(inst),BufferType::Structured,GPUMemoryUsage::Dynamic,inst.data()),g.buffer(sizeof(owners),BufferType::Structured,GPUMemoryUsage::Dynamic,owners.data()),g.buffer(count*16,BufferType::Structured,GPUMemoryUsage::Dynamic,nan.data()),g.buffer(count*4,BufferType::Structured,GPUMemoryUsage::Dynamic,flags.data())};
        u32 nums[]={25,26,28,29,30};u64 sizes[]={sizeof(tris),sizeof(inst),sizeof(owners),count*16,count*4};
        DescriptorSetLayoutBinding bindings[5]{};for(u32 i=0;i<5;++i){bindings[i]={nums[i],DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};bindings[i].readonly=i<3;}
        g.sl=g.d->CreateDescriptorSetLayout({5,bindings});g.pl=g.d->CreatePipelineLayout({1,&g.sl});g.sets[0]=g.d->CreateDescriptorSet({g.sl});
        require(g.sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT&&g.pl!=handles::INVALID_PIPELINE_LAYOUT&&g.sets[0]!=handles::INVALID_DESCRIPTOR_SET,"numeric layouts");
        DescriptorBufferInfo infos[5]{};WriteDescriptorSet writes[5]{};
        for(u32 i=0;i<5;++i){infos[i]={bufs[i],0,sizes[i]};writes[i]={g.sets[0],nums[i],0,1,DescriptorType::StorageBuffer,nullptr,&infos[i]};}g.d->UpdateDescriptorSets(5,writes);
        const char* paths[]={GG_INSTANCE_PRODUCER_SPV,GG_INSTANCE_VERIFY_SPV};
        for(u32 i=0;i<2;++i){std::ifstream f(paths[i],std::ios::binary|std::ios::ate);require(bool(f)&&f.tellg()>0,"numeric shader exists");std::vector<u8> data(size_t(f.tellg()));f.seekg(0);f.read(reinterpret_cast<char*>(data.data()),data.size());require(bool(f),"numeric shader read");g.shaders[i]=g.d->CreateShader(data.data(),data.size(),ShaderStage::Compute,"main");ComputePipelineDesc pd{};pd.computeShader=g.shaders[i];pd.layout=g.pl;g.pipes[i]=g.d->CreateComputePipeline(pd);require(g.pipes[i]!=handles::INVALID_PIPELINE,"numeric pipeline");}
        auto rb=g.buffer(count*20,BufferType::Raw,GPUMemoryUsage::Readback);
        const auto run=[&](bool produce){CommandBufferHandle h;auto* c=g.begin(h);
            c->MemoryBarrier(PipelineStage::Host|PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::HostWrite|AccessFlag::ShaderWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);
            // Vulkan's RHI uses the currently bound pipeline's native layout;
            // the layout argument alone does not establish that state.
            c->BindComputePipeline(g.pipes[produce?0:1]);
            c->BindDescriptorSets(PipelineBindPoint::Compute,g.pl,0,1,&g.sets[0],0,nullptr);
            if(produce){c->Dispatch(1,1,1);c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);}
            c->BindComputePipeline(g.pipes[1]);c->Dispatch(1,1,1);
            c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);c->CopyBuffer(bufs[3],rb,0,0,count*16);c->CopyBuffer(bufs[4],rb,0,count*16,count*4);
            c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);g.finish(h,c);
            auto* mapped=static_cast<const u32*>(g.d->MapBuffer(rb,0,count*20));require(mapped,"numeric readback");std::vector<u32> result(mapped,mapped+count*5);g.d->UnmapBuffer(rb);return result;};
        auto missing=run(false);for(u32 i=0;i<count;++i)require(missing[count*4+i]==0,"unwritten NaN sentinel never admitted");
        auto result=run(true);const u32 expected[]={1,1,1,1,0,0,0,0,0,0,1,1};
        for(u32 i=0;i<count;++i)require(result[count*4+i]==expected[i],"GPU finite/invalid numerical gate");
        require(result[2]==std::bit_cast<u32>(1.f)&&result[3]==std::bit_cast<u32>(1.f),"identity exact localN.z and length");
        require(result[2*4+3]==std::bit_cast<u32>(3.f)&&result[10*4+3]==std::bit_cast<u32>(3.f),"mirror inverse world length exact");
        // Real completed GPU output is corrupted by one bit, then ONLY the
        // verifier runs. The gate must revoke that item, never accept tolerance.
        result[3]^=1u;require(g.d->UpdateBufferData(bufs[3],result.data(),count*16,0),"inject one-bit mismatch");
        auto mismatch=run(false);require(mismatch[count*4]==0&&mismatch[count*4+2]==1,"one-bit mismatch rejected independently");
        return TestResult::Passed;
    }));
}
void registerInstanceNormalLifecycleCases(const std::shared_ptr<TestSuite>& suite){
    suite->AddTestCase(TestCase("cache_fenced_slots_source_rebuild_and_missing_completion",[]{
        GPU g;g.initializePipelines();auto scene=buildGeometry(Config{});
        GIGatherInputs in{};in.geometry_ready=true;in.geometry_epoch=1;in.geometry_source_digest=123;in.frame_index=1;in.geometry_still_current=[] {return true;};
        const void* data[]={scene.top.data(),scene.bottom.data(),scene.triangles.data(),scene.instances.data()};
        const u64 sizes[]={scene.top.size()*sizeof(ray_geometry::Node),scene.bottom.size()*sizeof(ray_geometry::Node),scene.triangles.size()*sizeof(ray_geometry::Triangle),scene.instances.size()*sizeof(ray_geometry::Instance)};
        for(u32 i=0;i<4;++i){in.geometry[i]=g.buffer(sizes[i],BufferType::Structured,GPUMemoryUsage::Dynamic,data[i]);in.geometry_sizes[i]=sizes[i];}
        in.geometry_normal_owners=std::make_shared<ray_geometry::InstanceNormalOwnerCertificate>(ray_geometry::CertifyInstanceNormalOwners(scene,128ull*1024*1024,53ull*1024*1024));
        require(!g.cache.Prepare(in),"no host fence witness rejects preparation");
        ResourceHandle slots[3][3]{};
        for(u32 slot=0;slot<3;++slot){
            in.current_buffer_index=slot;in.frame_index=slot+1;
            CommandBufferHandle h;auto* c=g.begin(h);g.finish(h,c);require(g.d->FrameSlotCompletions().Publish(slot),"publish completed no-op upload fence");
            require(g.cache.Prepare(in)&&!g.cache.Available(in),"prepared resources alone never establish producer completion");
            for(u32 i=0;i<3;++i)slots[slot][i]=g.cache.Buffer(slot,i);
            c=g.begin(h);g.cache.Record(c,in);g.finish(h,c);
            require(g.cache.Available(in),"same command producer/verifier and barrier allow consumption");
            ++in.frame_index;require(!g.cache.Available(in),"CPU record flag alone cannot certify next frame");
            auto stale=in;++stale.geometry_epoch;require(!g.cache.Prepare(stale),"unpublished completion cannot free previous source resources");
            require(g.d->FrameSlotCompletions().Publish(slot),"publish actual completed producer fence");
            require(g.cache.Available(in),"real completion permits persistent reuse");
            auto wrong=in;wrong.geometry_sizes[2]+=16;require(!g.cache.Available(wrong),"size mismatch rejected");
            wrong=in;wrong.geometry[0]=handles::INVALID_RESOURCE;require(!g.cache.Available(wrong),"handle mismatch rejected");
            wrong=in;wrong.geometry_source_digest++;require(!g.cache.Available(wrong),"digest mismatch rejected");
            wrong=in;wrong.geometry_still_current=[] {return false;};require(!g.cache.Available(wrong),"late source invalidation rejected");
        }
        for(u32 i=0;i<3;++i)require(slots[0][i]!=slots[1][i]&&slots[1][i]!=slots[2][i]&&slots[0][i]!=slots[2][i],"three in-flight slots have distinct buffers");
        in.current_buffer_index=2;in.geometry_epoch++;in.frame_index++;
        require(g.cache.Prepare(in)&&!g.cache.Available(in),"completed slot scene rebuild invalidates GPU production");
        CommandBufferHandle h;auto* c=g.begin(h);g.cache.Record(c,in);g.finish(h,c);require(g.cache.Available(in),"rebuilt source is produced again");
        return TestResult::Passed;
    }));
}
