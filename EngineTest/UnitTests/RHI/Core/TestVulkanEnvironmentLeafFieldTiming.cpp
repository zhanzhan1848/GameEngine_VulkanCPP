// Exact archived native239 EnvironmentVisibility Trace/Resolve reference replay.
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/Utilities/GeometryBVHStore.h"
#include <array>
#include <chrono>
#include <cmath>
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
    ShaderHandle shaders[5]{handles::INVALID_SHADER,handles::INVALID_SHADER,handles::INVALID_SHADER,handles::INVALID_SHADER,handles::INVALID_SHADER};
    DescriptorSetLayoutHandle sl=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    PipelineLayoutHandle pl=handles::INVALID_PIPELINE_LAYOUT;
    PipelineHandle pipes[5]{handles::INVALID_PIPELINE,handles::INVALID_PIPELINE,handles::INVALID_PIPELINE,handles::INVALID_PIPELINE,handles::INVALID_PIPELINE};
    DescriptorSetHandle sets[2]{handles::INVALID_DESCRIPTOR_SET,handles::INVALID_DESCRIPTOR_SET};
    GPU(){DeviceDesc desc{};desc.platform=RHIPlatform::Vulkan;desc.enableValidation=true;desc.enableDebug=true;d=CreateRHIDevice(desc);require(d!=nullptr,"Create Vulkan device");vk=static_cast<VulkanDevice*>(d);}
    ~GPU(){if(!d)return;for(auto s:sets)if(s!=handles::INVALID_DESCRIPTOR_SET)d->DestroyDescriptorSet(s);for(auto p:pipes)if(p!=handles::INVALID_PIPELINE)d->DestroyPipeline(p);if(pl!=handles::INVALID_PIPELINE_LAYOUT)d->DestroyPipelineLayout(pl);if(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)d->DestroyDescriptorSetLayout(sl);for(auto s:shaders)if(s!=handles::INVALID_SHADER)d->DestroyShader(s);for(auto t:textures)d->DestroyTexture(t);for(auto b:buffers)d->DestroyBuffer(b);d->Shutdown();}
    ResourceHandle buffer(u64 bytes,BufferType type,GPUMemoryUsage mem,const void* data=nullptr){
        BufferDesc bd{};bd.size=bytes;bd.type=type;bd.memoryUsage=mem;bd.name="EnvironmentVisibilityTest";
        auto h=d->CreateBuffer(bd);require(h!=handles::INVALID_RESOURCE,"Create buffer");buffers.push_back(h);if(data)require(d->UpdateBufferData(h,data,bytes,0),"Upload buffer");return h;
    }
    VulkanCommandBuffer* begin(CommandBufferHandle& handle){handle=d->CreateCommandBuffer(CommandQueueType::Graphics);auto* c=vk->GetCommandBuffer(handle);require(c&&c->Reset()&&c->Begin(),"Begin commands");return c;}
    void finish(CommandBufferHandle handle,VulkanCommandBuffer* c){auto fail=[](const char* op){std::fprintf(stderr,"[ENV_ARCHIVED_REPLAY] %s failed; pending resources retained until process exit\n",op);std::fflush(stderr);std::_Exit(7);};if(!c->End())fail("End");if(!c->Submit(0))fail("Submit");if(!c->WaitForCompletion())fail("Wait");d->DestroyCommandBuffer(handle);}
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
    void initializePipelines(bool candidate=false){
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
            {17,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr}};
        for(u32 i=6;i<11;++i)bindings[i].readonly=true;
        sl=d->CreateDescriptorSetLayout({11,bindings});require(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT,"Create matching descriptor layout");
        pl=d->CreatePipelineLayout({1,&sl});require(pl!=handles::INVALID_PIPELINE_LAYOUT,"Create pipeline layout");
        const char* paths[]={ENV_VIS_TEST_TRACE_SPV,ENV_FIELD_PROD_SPV,ENV_FIELD_PROD_SPV,ENV_VIS_TEST_TRACE_SPV,ENV_VIS_TEST_RESOLVE_SPV};
        const u64 hashes[]={10730699119571479063ull,11665097322767879862ull,11665097322767879862ull,10730699119571479063ull,7235070505685952031ull};
        require(!std::getenv("ENV_VIS_TEST_SHADER_DIRECTORY"),"shader override forbidden");
        for(u32 p=0;p<5;++p){std::ifstream f(paths[p],std::ios::binary|std::ios::ate);require(bool(f),"open shader");auto n=f.tellg();require(n>0,"nonempty shader");std::vector<u8> bytes(size_t(n),0);f.seekg(0);require(bool(f.read(reinterpret_cast<char*>(bytes.data()),n)),"read shader");u64 hash=14695981039346656037ull;for(u8 byte:bytes){hash^=byte;hash*=1099511628211ull;}require(hash==hashes[p],"actual production shader hash");std::fprintf(stderr,"[ENV_COST_SHADER] arm=%c fnv1a64=%llu path=%s\n",p<4?(p==0||p==3?'A':'H'):'R',(unsigned long long)hash,paths[p]);shaders[p]=d->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"main");require(shaders[p]!=handles::INVALID_SHADER,"shader");ComputePipelineDesc cd{};cd.layout=pl;cd.computeShader=shaders[p];pipes[p]=d->CreateComputePipeline(cd);require(pipes[p]!=handles::INVALID_PIPELINE,"pipeline");}
        for(u32 p=0;p<2;++p){sets[p]=d->CreateDescriptorSet({sl});require(sets[p]!=handles::INVALID_DESCRIPTOR_SET,"set");}
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
  GPU g;g.initializePipelines();auto depthTex=g.texture(W,H,DataFormat::D32_Float,depth.data(),depth.size());
  auto normalTex=g.texture(W,H,DataFormat::BGRA8_UNorm,normal.data(),normal.size()),ormTex=g.texture(W,H,DataFormat::BGRA8_UNorm,orm.data(),orm.size());
  auto halfTex=g.texture(HW,HH,DataFormat::RGBA16_Float,nullptr,0),fullTex=g.texture(W,H,DataFormat::RGBA16_Float,nullptr,0);
  ResourceHandle cb[2]={g.buffer(128,BufferType::Constant,GPUMemoryUsage::Dynamic,traceCB.data()),g.buffer(128,BufferType::Constant,GPUMemoryUsage::Dynamic,resolveCB.data())};
  const void* data[]={scene.top.data(),scene.bottom.data(),scene.triangles.data(),scene.instances.data()};const u64 sizes[]={scene.top.size()*sizeof(ray_geometry::Node),scene.bottom.size()*sizeof(ray_geometry::Node),scene.triangles.size()*sizeof(ray_geometry::Triangle),scene.instances.size()*sizeof(ray_geometry::Instance)};
  ResourceHandle geo[4];for(u32 i=0;i<4;++i)geo[i]=g.buffer(sizes[i],BufferType::Raw,GPUMemoryUsage::Dynamic,data[i]);
  auto fieldBytes=readFile("Docs/reports/primal-environment-sdf-hybrid-20261102/leaf-field-v2.bin",2646348);u64 fieldHash=14695981039346656037ull;for(u8 v:fieldBytes){fieldHash^=v;fieldHash*=1099511628211ull;}require(fieldHash==11017672305127041697ull,"frozen field identity");auto fieldBuffer=g.buffer(fieldBytes.size(),BufferType::Raw,GPUMemoryUsage::Dynamic,fieldBytes.data());
  for(u32 phase=0;phase<2;++phase){DescriptorImageInfo images[5]{};DescriptorBufferInfo buffers[6]{};WriteDescriptorSet writes[11]{};
   const ResourceHandle tex[]={depthTex,normalTex,ormTex,phase?fullTex:halfTex,phase?halfTex:ormTex};const u32 bindings[]={0,1,2,3,5};
   for(u32 i=0;i<5;++i){images[i]={handles::INVALID_SAMPLER,tex[i],i==3?ResourceState::UnorderedAccess:ResourceState::ShaderResource};writes[i]={g.sets[phase],bindings[i],0,1,i==3?DescriptorType::StorageImage:(i==0?DescriptorType::SampledDepthImage:DescriptorType::SampledImage),&images[i],nullptr};}
   buffers[0]={cb[phase],0,128};writes[5]={g.sets[phase],4,0,1,DescriptorType::UniformBuffer,nullptr,&buffers[0]};
   for(u32 i=0;i<4;++i){buffers[i+1]={geo[i],0,sizes[i]};writes[i+6]={g.sets[phase],13+i,0,1,DescriptorType::StorageBuffer,nullptr,&buffers[i+1]};}buffers[5]={fieldBuffer,0,fieldBytes.size()};writes[10]={g.sets[phase],17,0,1,DescriptorType::StorageBuffer,nullptr,&buffers[5]};g.d->UpdateDescriptorSets(11,writes);
  }
  // Validate uninstrumented A/B/C/D complete Trace+Resolve output first.
  for(u32 arm=0;arm<4;++arm){CommandBufferHandle h;auto* c=g.begin(h);c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::UniformRead);
   c->BindComputePipeline(g.pipes[arm]);c->BindDescriptorSets(PipelineBindPoint::Compute,g.pl,0,1,&g.sets[0],0,nullptr);c->Dispatch((HW+7)/8,(HH+7)/8,1);
   c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderRead);g.barrier(c,halfTex,ResourceState::UnorderedAccess,ResourceState::ShaderResource);
   c->BindComputePipeline(g.pipes[4]);c->BindDescriptorSets(PipelineBindPoint::Compute,g.pl,0,1,&g.sets[1],0,nullptr);c->Dispatch((W+7)/8,(H+7)/8,1);g.barrier(c,fullTex,ResourceState::UnorderedAccess,ResourceState::ShaderResource);g.finish(h,c);
   auto actualHalf=g.readRaw(halfTex,HW,HH),actualFull=g.readRaw(fullTex,W,H);u64 hd=0,fd=0;for(size_t i=0;i<actualHalf.size();++i)hd+=actualHalf[i]!=expectedHalf[i];for(size_t i=0;i<actualFull.size();++i)fd+=actualFull[i]!=expectedFull[i];
   std::fprintf(stderr,"[ENV_COST_IMAGE] view=%s arm=%c half_diff=%llu full_diff=%llu\n",view,(arm==0||arm==3?'A':'H'),(unsigned long long)hd,(unsigned long long)fd);require(!hd&&!fd,"production full image mismatch");
   auto* reset=g.begin(h);g.barrier(reset,halfTex,ResourceState::CopySource,ResourceState::UnorderedAccess);g.barrier(reset,fullTex,ResourceState::CopySource,ResourceState::UnorderedAccess);g.finish(h,reset);
  }
  VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(g.vk->GetNativePhysicalDevice(),&props);u32 n=0;vkGetPhysicalDeviceQueueFamilyProperties(g.vk->GetNativePhysicalDevice(),&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(g.vk->GetNativePhysicalDevice(),&n,families.data());u32 family=g.vk->GetGraphicsQueueFamily();require(family<n&&families[family].timestampValidBits>0&&props.limits.timestampPeriod>0.f,"timestamp unsupported");const u32 validBits=families[family].timestampValidBits;VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qci.queryType=VK_QUERY_TYPE_TIMESTAMP;qci.queryCount=2;VkQueryPool q=VK_NULL_HANDLE;require(vkCreateQueryPool(g.vk->GetNativeDevice(),&qci,nullptr,&q)==VK_SUCCESS,"query pool");struct QueryGuard{VkDevice device;VkQueryPool pool;~QueryGuard(){if(pool!=VK_NULL_HANDLE)vkDestroyQueryPool(device,pool,nullptr);}} queryGuard{g.vk->GetNativeDevice(),q};
  const u32 order[]={0,1,2,3,3,2,1,0};for(u32 run=0;run<20;++run){u32 arm=run<4?run:order[(run-4)%8];CommandBufferHandle h;auto* c=g.begin(h);auto native=c->GetNativeCmdBuffer();require(native!=VK_NULL_HANDLE,"native command");vkCmdResetQueryPool(native,q,0,2);c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::UniformRead);c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::ComputeShader,AccessFlag::ShaderWrite,AccessFlag::ShaderWrite);vkCmdWriteTimestamp(native,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,q,0);
   c->BindComputePipeline(g.pipes[arm]);c->BindDescriptorSets(PipelineBindPoint::Compute,g.pl,0,1,&g.sets[0],0,nullptr);c->Dispatch((HW+7)/8,(HH+7)/8,1);vkCmdWriteTimestamp(native,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,q,1);
   auto t0=std::chrono::steady_clock::now();g.finish(h,c);auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count();struct Q{u64 tick,available;}v[2]{};VkResult vr=vkGetQueryPoolResults(g.vk->GetNativeDevice(),q,0,2,sizeof(v),v,sizeof(Q),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);require(vr==VK_SUCCESS&&v[0].available&&v[1].available,"timestamp unavailable after completion");require(v[1].tick>=v[0].tick,"timestamp wrap rejected");long double cycle=std::ldexp((long double)props.limits.timestampPeriod,validBits);require((long double)ns<cycle,"timestamp multi-wrap possible");
   std::fprintf(stderr,"[ENV_COST_TIMESTAMP] view=%s run=%u warm=%u arm=%c valid_bits=%u period_ns=%.9g start=%llu end=%llu host_bound_ns=%lld duration_ms=%.9f\n",view,run,run<4?1u:0u,(arm==0||arm==3?'A':'H'),validBits,props.limits.timestampPeriod,(unsigned long long)v[0].tick,(unsigned long long)v[1].tick,(long long)ns,double(v[1].tick-v[0].tick)*double(props.limits.timestampPeriod)/1e6);
  }

 }return 0;
 }catch(const std::exception& e){std::cerr<<"[ENV_ARCHIVED_REPLAY] FAIL "<<e.what()<<"\n";return 1;}}
