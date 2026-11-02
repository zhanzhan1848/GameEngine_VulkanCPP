// Included inside the private GPU harness namespace. Exercises production
// irradiance/depth kernels and triple-slot inheritance; it does not claim to
// test SDF relocation or scene lifecycle (those need actual FarState captures).
TestResult TestFarSparseHistoryGPU() {
    GPU gpu;
    constexpr u32 n=8;
    std::array<u32,84> volume{};std::array<u32,128> globals{};
    auto vf=[&](unsigned at,float f){std::memcpy(reinterpret_cast<u8*>(volume.data())+at,&f,4);};
    volume[12]=Rays;volume[13]=n;volume[27]=7;vf(72,50);vf(80,.05f);vf(160,.01f);
    std::vector<float> rays(n*Rays*4),sh(n*36,0),depth(n*128),confidence(n*4,0),sky(n*2,0);
    for(u32 p=0;p<n;++p) {
        confidence[p*4+2]=1.f;
        for(u32 j=0;j<64;++j) depth[p*128+j]=-1.f;
        for(u32 r=0;r<Rays;++r) {
            auto i=(p*Rays+r)*4;rays[i]=.17f;rays[i+1]=.31f;rays[i+2]=.47f;rays[i+3]=2.f;
        }
    }
    auto g=gpu.buffer(sizeof(globals),BufferType::Constant,GPUMemoryUsage::Dynamic,globals.data());
    auto v=gpu.buffer(sizeof(volume),BufferType::Constant,GPUMemoryUsage::Dynamic,volume.data());
    auto r=gpu.buffer(rays.size()*4,BufferType::Structured,GPUMemoryUsage::Dynamic,rays.data());
    auto list=gpu.buffer(n*4,BufferType::Structured,GPUMemoryUsage::Dynamic);
    auto conf=gpu.buffer(confidence.size()*4,BufferType::Structured,GPUMemoryUsage::Dynamic,confidence.data());
    auto open=gpu.buffer(sky.size()*4,BufferType::Structured,GPUMemoryUsage::Dynamic,sky.data());
    ResourceHandle shSlots[3],depSlots[3];
    for(unsigned i=0;i<3;++i) {
        shSlots[i]=gpu.buffer(sh.size()*4,BufferType::Structured,GPUMemoryUsage::Dynamic,sh.data());
        depSlots[i]=gpu.buffer(depth.size()*4,BufferType::Structured,GPUMemoryUsage::Dynamic,depth.data());
    }
    primal::graphics::lumen::FarUpdatePolicy policy;policy.Initialize(n,2);
    std::vector<u8> prevSh(sh.size()*4),prevDep(depth.size()*4),prevConf(confidence.size()*4),prevSky(sky.size()*4);
    std::memcpy(prevDep.data(),depth.data(),prevDep.size());std::memcpy(prevConf.data(),confidence.data(),prevConf.size());
    u32 unchangedChecks=0;
    for(u32 frame=0;frame<10;++frame) {
        const u32 out=frame%3,hist=(frame+2)%3;
        const auto plan=policy.Select(true,false);
        volume[17]=frame;volume[21]=plan.indices.size();
        // A changing source makes accidentally re-running an unselected probe observable.
        for(u32 p=0;p<n;++p) for(u32 j=0;j<Rays;++j) rays[(p*Rays+j)*4]=.17f+.1f*frame;
        check(gpu.base->UpdateBufferData(v,volume.data(),sizeof(volume),0),"volume upload");
        check(gpu.base->UpdateBufferData(list,plan.indices.data(),plan.indices.size()*4,0),"list upload");
        check(gpu.base->UpdateBufferData(r,rays.data(),rays.size()*4,0),"rays upload");
        gpu.pipeline(DDGI_HISTORY_NEW_SPV,"ddgi_update_irradiance",
            {DescriptorType::UniformBuffer,DescriptorType::UniformBuffer,DescriptorType::StorageBuffer,DescriptorType::StorageBuffer,
             DescriptorType::StorageBuffer,DescriptorType::StorageBuffer,DescriptorType::StorageBuffer,DescriptorType::StorageBuffer},
            {{g,0,sizeof(globals)},{v,0,sizeof(volume)},{r,0,rays.size()*4},{shSlots[hist],0,sh.size()*4},
             {shSlots[out],0,sh.size()*4},{list,0,n*4},{conf,0,confidence.size()*4},{open,0,sky.size()*4}});
        auto nowSh=gpu.run(shSlots[out],sh.size()*4,1,1,shSlots[hist]);
        auto nowConf=gpu.read(conf,confidence.size()*4),nowSky=gpu.read(open,sky.size()*4);
        gpu.pipeline(DDGI_FAR_DEPTH_SPV,"ddgi_update_depth",
            {DescriptorType::UniformBuffer,DescriptorType::UniformBuffer,DescriptorType::StorageBuffer,
             DescriptorType::StorageBuffer,DescriptorType::StorageBuffer,DescriptorType::StorageBuffer,DescriptorType::StorageBuffer},
            {{g,0,sizeof(globals)},{v,0,sizeof(volume)},{r,0,rays.size()*4},{depSlots[hist],0,depth.size()*4},
             {depSlots[out],0,depth.size()*4},{list,0,n*4},{r,0,rays.size()*4}});
        auto nowDep=gpu.run(depSlots[out],depth.size()*4,1,1,depSlots[hist]);
        check(policy.Commit(plan,true,true),"commit recorded update");
        for(u32 p=0;p<n;++p) {
            if(std::find(plan.indices.begin(),plan.indices.end(),p)!=plan.indices.end()) {
                if(frame) check(std::memcmp(nowSh.data()+p*144,prevSh.data()+p*144,144)!=0,"selected SH consumed changing radiance");
                continue;
            }
            check(std::memcmp(nowSh.data()+p*144,prevSh.data()+p*144,144)==0,"unselected SH inherited exactly");
            check(std::memcmp(nowDep.data()+p*512,prevDep.data()+p*512,512)==0,"unselected depth inherited exactly");
            check(std::memcmp(nowConf.data()+p*16,prevConf.data()+p*16,16)==0,"unselected confidence unchanged");
            check(std::memcmp(nowSky.data()+p*8,prevSky.data()+p*8,8)==0,"unselected opensky unchanged");
            unchangedChecks+=4;
        }
        prevSh=std::move(nowSh);prevDep=std::move(nowDep);prevConf=std::move(nowConf);prevSky=std::move(nowSky);
    }
    std::cout<<"{\"test\":\"far_sparse_three_slot_history\",\"frames\":10,\"rays\":128,\"unchanged_checks\":"<<unchangedChecks<<"}\n";
    return TestResult::Passed;
}
