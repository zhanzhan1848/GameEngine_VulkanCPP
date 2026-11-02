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
        const char* paths[]={mode==0?ENV_PAIR_AUDIT_OLD_REF_SPV:(candidate?ENV_PAIR_AUDIT_CAND_SPV:ENV_PAIR_AUDIT_REF_SPV),ENV_VIS_TEST_RESOLVE_SPV};
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
            if(p==0)require(mode==1 && shaderHash==5688962046733576652ull,"frozen reference audit shader hash");
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
  std::vector<u32> sentinel(584*(64+16+16+32),0xffffffffu);auto auditBuffer=g.buffer(sentinel.size()*sizeof(u32),BufferType::Raw,GPUMemoryUsage::Dynamic,sentinel.data());
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
  auto records=run(1);u32 queryCount=0,active=0;for(u32 n=0;n<584;++n){u32 off=n*64;require(records[off]==0x454e5650u,"receiver stamp");for(u32 lane=0;lane<4;++lane){u32 r=off+16+lane*12,q=584u*96u+n*32u+lane*8u;require(records[r]<=1u,"lane activity stamp");active+=records[r];bool traversed=records[r+2u]!=0u||records[r+3u]!=0u;bool present=records[q+7u]!=0xffffffffu;require(present==traversed,"query stamp versus traversal");if(present){require(records[r]==1u && records[q+7u]==0xe5710003u,"real scalar query stamp");++queryCount;}}}require(active&&queryCount,"nonempty real queries");
  const std::string path="Docs/reports/primal-environment-sdf-hybrid-20261102/real-query-"+std::string(view)+".bin";std::ifstream existing(path,std::ios::binary);require(!existing.good(),"refusing overwrite existing evidence");std::ofstream out(path,std::ios::binary|std::ios::out);require(bool(out),"create query capture");out.write(reinterpret_cast<const char*>(records.data()),records.size()*sizeof(u32));out.flush();require(bool(out),"write query capture");out.close();require(!out.fail(),"close query capture");std::fprintf(stderr,"[ENV_REAL_QUERY_CAPTURE] view=%s samples=584 words=%zu active=%u queries=%u path=%s\n",view,records.size(),active,queryCount,path.c_str());
 }return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"[ENV_REAL_QUERY_CAPTURE] FAIL %s\n",e.what());return 1;}}
