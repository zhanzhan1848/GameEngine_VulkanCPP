// Exact archived native239 EnvironmentVisibility Trace/Resolve reference replay.
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/Utilities/GeometryBVHStore.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace primal;using namespace primal::graphics;using namespace primal::graphics::rhi;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct GPU {
    RHIDeviceBase* d=nullptr;VulkanDevice* vk=nullptr;
    std::vector<ResourceHandle> buffers,textures;
    ShaderHandle shaders[2]{handles::INVALID_SHADER,handles::INVALID_SHADER};
    DescriptorSetLayoutHandle sl=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    PipelineLayoutHandle pl=handles::INVALID_PIPELINE_LAYOUT;
    PipelineHandle pipes[2]{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
    DescriptorSetHandle sets[2]{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};
    GPU(){DeviceDesc desc{};desc.platform=RHIPlatform::Vulkan;desc.enableValidation=true;desc.enableDebug=true;d=CreateRHIDevice(desc);require(d!=nullptr,"Create Vulkan device");vk=static_cast<VulkanDevice*>(d);}
    ~GPU(){if(!d)return;for(auto s:sets)if(s!=handles::INVALID_DESCRIPTOR_SET)d->DestroyDescriptorSet(s);for(auto p:pipes)if(p!=handles::INVALID_PIPELINE)d->DestroyPipeline(p);if(pl!=handles::INVALID_PIPELINE_LAYOUT)d->DestroyPipelineLayout(pl);if(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)d->DestroyDescriptorSetLayout(sl);for(auto s:shaders)if(s!=handles::INVALID_SHADER)d->DestroyShader(s);for(auto t:textures)d->DestroyTexture(t);for(auto b:buffers)d->DestroyBuffer(b);d->Shutdown();}
    ResourceHandle buffer(u64 bytes,BufferType type,GPUMemoryUsage mem,const void* data=nullptr){
        BufferDesc bd{};bd.size=bytes;bd.type=type;bd.memoryUsage=mem;bd.name="EnvironmentVisibilityTest";
        auto h=d->CreateBuffer(bd);require(h!=handles::INVALID_RESOURCE,"Create buffer");buffers.push_back(h);if(data)require(d->UpdateBufferData(h,data,bytes,0),"Upload buffer");return h;
    }
    VulkanCommandBuffer* begin(CommandBufferHandle& handle){handle=d->CreateCommandBuffer(CommandQueueType::Compute);auto* c=vk->GetCommandBuffer(handle);require(c&&c->Reset()&&c->Begin(),"Begin commands");return c;}
    void finish(CommandBufferHandle handle,VulkanCommandBuffer* c){auto fail=[](const char* op){std::fprintf(stderr,"[ENV_CROSS_RAY_AUDIT_IMAGE] %s failed; pending resources retained until process exit\n",op);std::fflush(stderr);std::_Exit(7);};if(!c->End())fail("End");if(!c->Submit(0))fail("Submit");if(!c->WaitForCompletion())fail("Wait");d->DestroyCommandBuffer(handle);}
    void barrier(VulkanCommandBuffer* c,ResourceHandle h,ResourceState before,ResourceState after){ResourceBarrier b{};b.resource=h;b.beforeState=before;b.afterState=after;b.subresource=0xffffffffu;b.queueFamily=0xffffffffu;c->InsertBarrier(&b,1);}
    ResourceHandle texture(u32 w,u32 h,DataFormat format,const void* data,u64 bytes,bool output=false){
        TextureDesc td{};td.type=TextureType::Texture2D;td.size={w,h,1};td.format=format;td.mipLevels=1;
        td.usage=TextureUsage::ShaderResource|TextureUsage::CopyDest|TextureUsage::CopySource;
        if(!data||output)td.usage=td.usage|TextureUsage::UnorderedAccess;
        auto t=d->CreateTexture(td);require(t!=handles::INVALID_RESOURCE,"Create typed texture");textures.push_back(t);
        CommandBufferHandle handle;auto* c=begin(handle);
        if(data){auto staging=buffer(bytes,BufferType::Raw,GPUMemoryUsage::Dynamic,data);BufferTextureCopyRegion region{};region.imageSubresource.layerCount=1;region.imageExtent={w,h,1};
            c->MemoryBarrier(PipelineStage::Host,PipelineStage::Transfer,AccessFlag::HostWrite,AccessFlag::TransferRead);
            c->CopyBufferToTexture(staging,t,&region,1);
            c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::ComputeShader,AccessFlag::TransferWrite,AccessFlag::ShaderWrite|AccessFlag::ShaderRead);
            barrier(c,t,ResourceState::CopyDest,output?ResourceState::UnorderedAccess:ResourceState::ShaderResource);}
        else barrier(c,t,ResourceState::Unknown,ResourceState::UnorderedAccess);
        finish(handle,c);return t;
    }
    void initializePipelines(int mode){
        const bool candidate=mode==2;
        DescriptorSetLayoutBinding bindings[11]={
            {0,DescriptorType::SampledDepthImage,1,ShaderStage::Compute,nullptr},
            {1,DescriptorType::SampledImage,1,ShaderStage::Compute,nullptr},
            {2,DescriptorType::SampledImage,1,ShaderStage::Compute,nullptr},
            {3,DescriptorType::StorageImage,1,ShaderStage::Compute,nullptr},
            {4,DescriptorType::UniformBuffer,1,ShaderStage::Compute,nullptr},
            {5,DescriptorType::SampledImage,1,ShaderStage::Compute,nullptr},
            {13,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr},
            {14,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr},
            {15,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr},
            {16,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr},
            {28,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr}};
        for(u32 i=6;i<10;++i)bindings[i].readonly=true;
        sl=d->CreateDescriptorSetLayout({11,bindings});require(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT,"Create matching descriptor layout");
        pl=d->CreatePipelineLayout({1,&sl});require(pl!=handles::INVALID_PIPELINE_LAYOUT,"Create pipeline layout");
        const char* paths[]={mode==0?ENV_PAIR_AUDIT_OLD_REF_SPV:(candidate?ENV_COST_AUDIT_SPV:ENV_PAIR_AUDIT_REF_SPV),ENV_VIS_TEST_RESOLVE_SPV};
#ifdef ENV_VIS_WG32_TEST
        if(candidate)paths[0]=ENV_VIS_WG32_SPV;
#else
        (void)candidate;
#endif
        std::string stagePaths[2];
#ifdef ENV_VIS_WG32_TEST
        require(std::getenv("ENV_VIS_TEST_SHADER_DIRECTORY")==nullptr,"WG32 comparison forbids shared shader directory override");
#endif
        require(!std::getenv("ENV_VIS_TEST_SHADER_DIRECTORY"),"archived replay forbids shader override");
        if(const char* dir=std::getenv("ENV_VIS_TEST_SHADER_DIRECTORY")){
            stagePaths[0]=std::string(dir)+"/EnvironmentVisibility_trace.comp.spv";
            stagePaths[1]=std::string(dir)+"/EnvironmentVisibility_resolve.comp.spv";
            paths[0]=stagePaths[0].c_str();paths[1]=stagePaths[1].c_str();
        }
        for(u32 p=0;p<2;++p){std::ifstream f(paths[p],std::ios::binary|std::ios::ate);require(bool(f),"Open private production SPV");auto n=f.tellg();require(n>0,"Nonempty SPV");std::vector<u8> bytes(static_cast<size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),n);require(bool(f),"Read SPV");
            u64 shaderHash=14695981039346656037ull;for(u8 byte:bytes){shaderHash^=byte;shaderHash*=1099511628211ull;}
            if(p==1)require(shaderHash==7235070505685952031ull,"frozen original resolve shader hash");
            if(p==0 && mode==0)require(shaderHash==2217160481799002807ull,"frozen old reference shader hash");
            static u64 referenceHash=0;if(p==0&&!candidate)referenceHash=shaderHash;if(p==0&&candidate)require(referenceHash && referenceHash!=shaderHash,"audit reference and pair shaders differ");
            std::fprintf(stderr,"[ENV_CROSS_RAY_AUDIT_SHADER] mode=%d stage=%s fnv1a64=%llu path=%s\n",mode,p?"resolve":"trace",static_cast<unsigned long long>(shaderHash),paths[p]);
#ifdef ENV_VIS_WG32_TEST
            const auto id=shaderIdentity(bytes);
            require(id.x==8 && id.y==(p==0 && candidate?4u:8u) && id.z==1,"Actual shader LocalSize matches dispatch");
            static u64 referenceTrace=0,referenceResolve=0;
            if(p==0 && !candidate) {if(referenceTrace)require(referenceTrace==id.hash,"Stable reference trace hash");else referenceTrace=id.hash;}
            if(p==0 && candidate)require(referenceTrace && referenceTrace!=id.hash,"Candidate trace differs from reference");
            if(p==1) {if(referenceResolve)require(referenceResolve==id.hash,"Same original resolve in both modes");else referenceResolve=id.hash;}
            std::cout << "[ENV_VIS_WG32_SHADER] mode=" << (candidate?"candidate":"reference") << " stage=" << (p?"resolve":"trace")
                << " path=" << paths[p] << " fnv1a64=" << id.hash << " local_x=" << id.x << " local_y=" << id.y << " local_z=" << id.z << '\n';
#endif
            shaders[p]=d->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"main");require(shaders[p]!=handles::INVALID_SHADER,"Create production shader");ComputePipelineDesc cd{};cd.layout=pl;cd.computeShader=shaders[p];pipes[p]=d->CreateComputePipeline(cd);require(pipes[p]!=handles::INVALID_PIPELINE,"Create production pipeline");sets[p]=d->CreateDescriptorSet({sl});require(sets[p]!=handles::INVALID_DESCRIPTOR_SET,"Create descriptor set");}
    }
    std::vector<u8> readRaw(ResourceHandle t,u32 w,u32 h){
        const u64 bytes=u64(w)*h*8;auto rb=buffer(bytes,BufferType::Raw,GPUMemoryUsage::Readback);CommandBufferHandle handle;auto* c=begin(handle);
        barrier(c,t,ResourceState::ShaderResource,ResourceState::CopySource);c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);
        BufferTextureCopyRegion r{};r.imageSubresource.layerCount=1;r.imageExtent={w,h,1};c->CopyTextureToBuffer(t,rb,&r,1);c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);finish(handle,c);
        auto* p=static_cast<const u8*>(d->MapBuffer(rb,0,bytes));require(p,"Map archived replay readback");std::vector<u8> result(p,p+bytes);d->UnmapBuffer(rb);return result;
    }
};
std::vector<u8> readFile(const std::string& path,size_t bytes){std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f),"open archived source");require(size_t(f.tellg())==bytes,"archived source size");std::vector<u8> result(bytes);f.seekg(0);require(bool(f.read(reinterpret_cast<char*>(result.data()),bytes)),"read archived source");return result;}
int main(){try{
 ray_geometry::SceneBVH scene;require(ray_geometry::LoadSceneBVH(".primal-cache/geometry-bvh/5d07eba5b7554607.tlas",6703585667342222855ull,scene),"load recorded PBVH digest");
 auto cert=ray_geometry::CertifyCompactTraversal(scene);require(cert.valid,"PBVH validates");
 constexpr u32 W=2560,H=1440,HW=1280,HH=720;
 for(const char* view:{"floor","atrium"}) {
  const std::string dir="Docs/reports/primal-bvh-payload-reuse-20261101/capture-"+std::string(view)+"-ref/raw/frame-240/";
  auto normal=readFile(dir+"pass-72-resource-0-v0.raw",size_t(W)*H*4),orm=readFile(dir+"pass-73-resource-0-v0.raw",size_t(W)*H*4),depth=readFile(dir+"pass-74-resource-0-v0.raw",size_t(W)*H*4);
  auto traceCB=readFile(dir+"pass-58-resource-0-v0.raw",128),resolveCB=readFile(dir+"pass-61-resource-0-v0.raw",128);
  auto expectedHalf=readFile(dir+"pass-57-resource-0-v0.raw",size_t(HW)*HH*8),expectedFull=readFile(dir+"pass-60-resource-0-v0.raw",size_t(W)*H*8);
  u32 dims[4];std::memcpy(dims,traceCB.data()+80,sizeof(dims));require(dims[0]==W&&dims[1]==H&&dims[2]==HW&&dims[3]==HH,"archived CB dimensions");
  auto run=[&](int mode){GPU g;g.initializePipelines(mode);auto depthTex=g.texture(W,H,DataFormat::D32_Float,depth.data(),depth.size());
  auto normalTex=g.texture(W,H,DataFormat::BGRA8_UNorm,normal.data(),normal.size()),ormTex=g.texture(W,H,DataFormat::BGRA8_UNorm,orm.data(),orm.size());
  auto halfTex=g.texture(HW,HH,DataFormat::RGBA16_Float,nullptr,0),fullTex=g.texture(W,H,DataFormat::RGBA16_Float,nullptr,0);
  ResourceHandle cb[2]={g.buffer(128,BufferType::Constant,GPUMemoryUsage::Dynamic,traceCB.data()),g.buffer(128,BufferType::Constant,GPUMemoryUsage::Dynamic,resolveCB.data())};
  const void* data[]={scene.top.data(),scene.bottom.data(),scene.triangles.data(),scene.instances.data()};const u64 sizes[]={scene.top.size()*sizeof(ray_geometry::Node),scene.bottom.size()*sizeof(ray_geometry::Node),scene.triangles.size()*sizeof(ray_geometry::Triangle),scene.instances.size()*sizeof(ray_geometry::Instance)};
  ResourceHandle geo[4];for(u32 i=0;i<4;++i)geo[i]=g.buffer(sizes[i],BufferType::Raw,GPUMemoryUsage::Dynamic,data[i]);
  std::vector<u32> sentinel(584*(64+16+16+32+8),0xffffffffu);auto auditBuffer=g.buffer(sentinel.size()*sizeof(u32),BufferType::Raw,GPUMemoryUsage::Dynamic,sentinel.data());
  auto auditReadback=g.buffer(sentinel.size()*sizeof(u32),BufferType::Raw,GPUMemoryUsage::Readback);
  for(u32 phase=0;phase<2;++phase){DescriptorImageInfo images[5]{};DescriptorBufferInfo buffers[6]{};WriteDescriptorSet writes[11]{};
   const ResourceHandle tex[]={depthTex,normalTex,ormTex,phase?fullTex:halfTex,phase?halfTex:ormTex};const u32 bindings[]={0,1,2,3,5};
   for(u32 i=0;i<5;++i){images[i]={handles::INVALID_SAMPLER,tex[i],i==3?ResourceState::UnorderedAccess:ResourceState::ShaderResource};writes[i]={g.sets[phase],bindings[i],0,1,i==3?DescriptorType::StorageImage:(i==0?DescriptorType::SampledDepthImage:DescriptorType::SampledImage),&images[i],nullptr};}
   buffers[0]={cb[phase],0,128};writes[5]={g.sets[phase],4,0,1,DescriptorType::UniformBuffer,nullptr,&buffers[0]};
   for(u32 i=0;i<4;++i){buffers[i+1]={geo[i],0,sizes[i]};writes[i+6]={g.sets[phase],13+i,0,1,DescriptorType::StorageBuffer,nullptr,&buffers[i+1]};}
   buffers[5]={auditBuffer,0,sentinel.size()*sizeof(u32)};writes[10]={g.sets[phase],28,0,1,DescriptorType::StorageBuffer,nullptr,&buffers[5]};g.d->UpdateDescriptorSets(11,writes);
  }
  CommandBufferHandle h;auto* c=g.begin(h);c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::UniformRead);
  c->BindComputePipeline(g.pipes[0]);c->BindDescriptorSets(PipelineBindPoint::Compute,g.pl,0,1,&g.sets[0],0,nullptr);c->Dispatch((HW+7)/8,(HH+7)/8,1);
  c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead);g.barrier(c,halfTex,ResourceState::UnorderedAccess,ResourceState::ShaderResource);
  c->BindComputePipeline(g.pipes[1]);c->BindDescriptorSets(PipelineBindPoint::Compute,g.pl,0,1,&g.sets[1],0,nullptr);c->Dispatch((W+7)/8,(H+7)/8,1);g.barrier(c,fullTex,ResourceState::UnorderedAccess,ResourceState::ShaderResource);
  c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);c->CopyBuffer(auditBuffer,auditReadback,0,0,sentinel.size()*sizeof(u32));c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);g.finish(h,c);
  auto* mapped=static_cast<const u32*>(g.d->MapBuffer(auditReadback,0,sentinel.size()*sizeof(u32)));require(mapped,"map diagnostic records");std::vector<u32> records(mapped,mapped+sentinel.size());g.d->UnmapBuffer(auditReadback);
  auto actualHalf=g.readRaw(halfTex,HW,HH),actualFull=g.readRaw(fullTex,W,H);
  u64 halfDifferences=0,fullDifferences=0;for(size_t i=0;i<actualHalf.size();++i)halfDifferences+=actualHalf[i]!=expectedHalf[i];for(size_t i=0;i<actualFull.size();++i)fullDifferences+=actualFull[i]!=expectedFull[i];
  std::fprintf(stderr,"[ENV_CROSS_RAY_AUDIT_IMAGE] view=%s mode=%d half_bytes=%zu full_bytes=%zu half_byte_mismatch=%llu full_byte_mismatch=%llu\n",view,mode,actualHalf.size(),actualFull.size(),static_cast<unsigned long long>(halfDifferences),static_cast<unsigned long long>(fullDifferences));
  require(!halfDifferences&&!fullDifferences,"archived native239 shadow full output mismatch");return records;};
  auto oldReference=run(0),original=run(1),candidate=run(2);
  auto classify=[&](const char* name,const std::vector<u32>& rows){u64 active=0,mirror=0,four=0;for(u32 n=0;n<584;++n){u32 off=n*64;require(rows[off]==0x454e5650u,"activity sample stamp");float rough;std::memcpy(&rough,&rows[off+15],sizeof(rough));for(u32 lane=0;lane<4;++lane){require(rows[off+16+lane*12]<=1u,"activity lane stamp");if(rows[off+16+lane*12]){++active;if(rough<=.06f)++mirror;else ++four;}}}std::fprintf(stderr,"[ENV_REFERENCE_ACTIVITY] view=%s mode=%s active=%llu mirror=%llu four=%llu\n",view,name,(unsigned long long)active,(unsigned long long)mirror,(unsigned long long)four);};
  classify("old",oldReference);classify("new",original);classify("candidate",candidate);
  u64 privateWordDifferences=0;for(u32 n=0;n<584;++n){u32 off=n*64;for(u32 f=0;f<16;++f)privateWordDifferences+=oldReference[584u*64u+n*16u+f]!=original[584u*64u+n*16u+f];for(u32 f=0;f<64;++f)if(oldReference[off+f]!=original[off+f]){std::fprintf(stderr,"[ENV_REFERENCE_BRIDGE_MISMATCH] view=%s sample=%u region=main field=%u old=%08x new=%08x\n",view,n,f,oldReference[off+f],original[off+f]);throw std::runtime_error("old/new reference main record mismatch");}
   u32 direct=584u*80u+n*16u;for(u32 f=0;f<16;++f)if(oldReference[direct+f]!=original[direct+f]){std::fprintf(stderr,"[ENV_REFERENCE_BRIDGE_MISMATCH] view=%s sample=%u region=direct field=%u old=%08x new=%08x\n",view,n,f,oldReference[direct+f],original[direct+f]);throw std::runtime_error("old/new reference direct input mismatch");}}
  std::fprintf(stderr,"[ENV_REFERENCE_BRIDGE] view=%s old_new_main64=exact old_new_direct16=exact old_private16_differing_words=%llu old_query_tail=unavailable\n",view,(unsigned long long)privateWordDifferences);u64 baseNodes=0,pairNodes=0,baseInstances=0,pairInstances=0,baseTriangles=0,pairTriangles=0,paired=0,overflow=0,used=0,valid=0,preDifferentRays=0,preDifferentComponents=0,preMaxUlp=0,preNonfinite=0,internalMismatch=0,actualDifferentRays=0,actualDifferentComponents=0,actualMaxUlp=0,actualNonfinite=0,actualQueries=0;
  for(u32 index=0;index<584;++index){const u32 off=index*64;require(original[off]==0x454e5650u && candidate[off]==0x454e5650u,"every selected receiver stamped");
   require(original[off+1]==candidate[off+1]&&original[off+2]==candidate[off+2]&&original[off+3]==candidate[off+3],"sample identity and receiver validity match");
   valid+=original[off+3];baseNodes+=original[off+5];pairNodes+=candidate[off+5];baseInstances+=original[off+6];pairInstances+=candidate[off+6];baseTriangles+=original[off+7];pairTriangles+=candidate[off+7];paired+=candidate[off+8];overflow+=candidate[off+9];
   for(u32 field=10;field<16;++field)require(original[off+field]==candidate[off+field],"receiver origin/policy mismatch");
   for(u32 ray=0;ray<4;++ray){u32 at=off+16+ray*12,pre=584u*64u+index*16u+ray*4u;
    const u32 direct=584u*80u+index*16u+ray*4u;
    {const u32 q=584u*96u+index*32u+ray*8u;const u32 rs=original[q+7u],cs=candidate[q+7u];
     const bool referenceTraversed=original[at+2u]!=0u || original[at+3u]!=0u;
     const bool candidateTraversed=candidate[at+2u]!=0u || candidate[at+3u]!=0u;
     const bool referenceStamped=rs!=0xffffffffu,candidateStamped=cs!=0xffffffffu;
     require(referenceStamped==referenceTraversed && candidateStamped==candidateTraversed,"per-ray query stamp/traversal mismatch");
     require((!referenceTraversed && !candidateTraversed) || (original[at] && candidate[at]),"inactive ray traversed");
     require(referenceStamped==candidateStamped,"actual query presence mismatch");
     if(referenceStamped)++actualQueries;
     if(rs!=0xffffffffu){require(rs==0xe5710003u && (cs==0xe5710002u || cs==0xe5710003u),"actual query source stamp");for(u32 f=0;f<7;++f)if(original[q+f]!=candidate[q+f]){std::fprintf(stderr,"[ENV_CROSS_RAY_QUERY_MISMATCH] view=%s sample=%u ray=%u field=%u ref=%08x candidate=%08x\n",view,index,ray,f,original[q+f],candidate[q+f]);throw std::runtime_error("actual query origin/direction/far mismatch");}}}
    if(original[at]){require(original[direct+3u]==0xe5700003u,"reference direct scalar direction stamp");require(candidate[direct+3u]>=0xe5700001u && candidate[direct+3u]<=0xe5700003u,"candidate direct direction stamp");bool differs=false;for(u32 axis=0;axis<3;++axis){u32 a=original[direct+axis],b=candidate[direct+axis];if(a==b)continue;differs=true;++actualDifferentComponents;
      if((a&0x7f800000u)==0x7f800000u||(b&0x7f800000u)==0x7f800000u){++actualNonfinite;continue;}auto ordered=[](u32 x)->u32{return (x&0x80000000u)?~x:(x|0x80000000u);};u32 x=ordered(a),y=ordered(b);u64 ulp=x>y?u64(x-y):u64(y-x);if(ulp>actualMaxUlp)actualMaxUlp=ulp;}
     if(differs){if(actualDifferentRays==0)std::fprintf(stderr,"[ENV_CROSS_RAY_DIRECT_DIRECTION] view=%s sample=%u ray=%u reference=(%08x,%08x,%08x) candidate=(%08x,%08x,%08x) reference_stamp=%08x candidate_stamp=%08x\n",view,index,ray,original[direct],original[direct+1],original[direct+2],candidate[direct],candidate[direct+1],candidate[direct+2],original[direct+3],candidate[direct+3]);++actualDifferentRays;}}
    if(original[at]&&candidate[pre+3u]){bool differs=false;for(u32 axis=0;axis<3;++axis){u32 a=original[at+7u+axis],b=candidate[pre+axis];if(a==b)continue;differs=true;++preDifferentComponents;
      if((a&0x7f800000u)==0x7f800000u||(b&0x7f800000u)==0x7f800000u){++preNonfinite;continue;}
      auto ordered=[](u32 x)->u32{return (x&0x80000000u)?~x:(x|0x80000000u);};u32 x=ordered(a),y=ordered(b);u64 ulp=x>y?u64(x-y):u64(y-x);if(ulp>preMaxUlp)preMaxUlp=ulp;}
     if(differs)++preDifferentRays;}
    for(u32 field=0;field<12;++field)if(original[at+field]!=candidate[at+field]){
    if(field==11 && internalMismatch++>0)continue;
    static constexpr const char* fields[]={"used","outcome","top","bottom","instances","triangles","order","direction_x","direction_y","direction_z","weight","pre_direction_mismatch"};
    auto asFloat=[](u32 bits){float value;std::memcpy(&value,&bits,sizeof(value));return value;};
    std::fprintf(stderr,"[ENV_CROSS_RAY_AUDIT_MISMATCH] view=%s sample=%u x=%u y=%u ray=%u field=%s ref=0x%08x candidate=0x%08x origin=(%.9g,%.9g,%.9g) ref_used=%u ref_status=%u candidate_used=%u candidate_status=%u ref_dir=(%.9g,%.9g,%.9g) candidate_dir=(%.9g,%.9g,%.9g) ref_counts=(%u,%u,%u,%u) candidate_counts=(%u,%u,%u,%u) ref_order=0x%08x candidate_order=0x%08x ref_weight=0x%08x candidate_weight=0x%08x candidate_pre_direction_mismatch=%u\n",view,index,original[off+1],original[off+2],ray,fields[field],original[at+field],candidate[at+field],asFloat(original[off+10]),asFloat(original[off+11]),asFloat(original[off+12]),original[at],original[at+1],candidate[at],candidate[at+1],asFloat(original[at+7]),asFloat(original[at+8]),asFloat(original[at+9]),asFloat(candidate[at+7]),asFloat(candidate[at+8]),asFloat(candidate[at+9]),original[at+2],original[at+3],original[at+4],original[at+5],candidate[at+2],candidate[at+3],candidate[at+4],candidate[at+5],original[at+6],candidate[at+6],original[at+10],candidate[at+10],candidate[at+11]);
    std::fprintf(stderr,"[ENV_CROSS_RAY_PRE_DIRECTION] reference_world_bits=(%08x,%08x,%08x) candidate_pre_bits=(%08x,%08x,%08x) candidate_pre=(%.9g,%.9g,%.9g)\n",original[at+7],original[at+8],original[at+9],candidate[pre],candidate[pre+1],candidate[pre+2],asFloat(candidate[pre]),asFloat(candidate[pre+1]),asFloat(candidate[pre+2]));
    if(field!=11)throw std::runtime_error("VNDF activity/outcome/count/order/direction/weight mismatch");
   }used+=original[at];}}
  std::fprintf(stderr,"[ENV_CROSS_RAY_PRE_SUMMARY] view=%s differing_rays=%llu differing_components=%llu max_ulp=%llu nonfinite_components=%llu internal_mismatch_rays=%llu\n",view,static_cast<unsigned long long>(preDifferentRays),static_cast<unsigned long long>(preDifferentComponents),static_cast<unsigned long long>(preMaxUlp),static_cast<unsigned long long>(preNonfinite),static_cast<unsigned long long>(internalMismatch));
  std::fprintf(stderr,"[ENV_CROSS_RAY_DIRECT_SUMMARY] view=%s differing_rays=%llu differing_components=%llu max_ulp=%llu nonfinite_components=%llu\n",view,static_cast<unsigned long long>(actualDifferentRays),static_cast<unsigned long long>(actualDifferentComponents),static_cast<unsigned long long>(actualMaxUlp),static_cast<unsigned long long>(actualNonfinite));
  require(actualDifferentRays==0,"actual query direction differs from reference");
  u64 freq[8]{};for(u32 sample=0;sample<584;++sample)for(u32 k=0;k<8;++k){u32 v=candidate[584u*128u+sample*8u+k];require(v!=0xffffffffu,"cost frequency stamp");freq[k]+=v;}
  std::fprintf(stderr,"[ENV_COST_LANES] view=%s zero=%llu one=%llu two=%llu prepare=%llu early=%llu need_one=%llu need_two=%llu fallback=%llu\n",view,(unsigned long long)freq[0],(unsigned long long)freq[1],(unsigned long long)freq[2],(unsigned long long)freq[3],(unsigned long long)freq[4],(unsigned long long)freq[5],(unsigned long long)freq[6],(unsigned long long)freq[7]);
#ifdef ENV_COST_REQUIRE_PAIR
  require(paired>0,"E candidate must actually pair queries");
#endif
  require(valid>0 && used>0 && baseNodes>0 && actualQueries>0,"real measured samples, stamped queries and positive paired work");
  std::fprintf(stderr,"[ENV_CROSS_RAY_QUERY_COVERAGE] view=%s actual_queries=%llu\n",view,static_cast<unsigned long long>(actualQueries));
  std::fprintf(stderr,"[ENV_CROSS_RAY_AUDIT] view=%s samples=584 valid=%llu rays=%llu reference_nodes=%llu pair_nodes=%llu reference_instances=%llu pair_instances=%llu reference_triangles=%llu pair_triangles=%llu paired=%llu fallback=%llu full_raw_equal=1\n",view,static_cast<unsigned long long>(valid),static_cast<unsigned long long>(used),static_cast<unsigned long long>(baseNodes),static_cast<unsigned long long>(pairNodes),static_cast<unsigned long long>(baseInstances),static_cast<unsigned long long>(pairInstances),static_cast<unsigned long long>(baseTriangles),static_cast<unsigned long long>(pairTriangles),static_cast<unsigned long long>(paired),static_cast<unsigned long long>(overflow));

 }return 0;
 }catch(const std::exception& e){std::cerr<<"[ENV_CROSS_RAY_AUDIT_IMAGE] FAIL "<<e.what()<<"\n";return 1;}}
