// Production ScreenProbePlace SPIR-V with controlled full-resolution inputs.
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace primal;
using namespace primal::graphics;
using namespace primal::graphics::rhi;
namespace {
void check(bool value,const char* msg){if(!value)throw std::runtime_error(msg);}
constexpr u32 W=13,H=11,D=8,GW=2,GH=2;
using Vec4=std::array<float,4>;
struct GPU {
 RHIDeviceBase* d=nullptr; VulkanDevice* vk=nullptr;
 std::vector<ResourceHandle> buffers,textures; ShaderHandle shader=handles::INVALID_SHADER;
 DescriptorSetLayoutHandle sl=handles::INVALID_DESCRIPTOR_SET_LAYOUT;
 PipelineLayoutHandle pl=handles::INVALID_PIPELINE_LAYOUT;PipelineHandle pipe=handles::INVALID_PIPELINE;
 DescriptorSetHandle set=handles::INVALID_DESCRIPTOR_SET;
 GPU(){DeviceDesc desc{};desc.platform=RHIPlatform::Vulkan;desc.enableValidation=true;desc.enableDebug=true;d=CreateRHIDevice(desc);check(d,"Vulkan device");vk=static_cast<VulkanDevice*>(d);}
 ~GPU(){if(!d)return;if(set!=handles::INVALID_DESCRIPTOR_SET)d->DestroyDescriptorSet(set);if(pipe!=handles::INVALID_PIPELINE)d->DestroyPipeline(pipe);if(pl!=handles::INVALID_PIPELINE_LAYOUT)d->DestroyPipelineLayout(pl);if(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)d->DestroyDescriptorSetLayout(sl);if(shader!=handles::INVALID_SHADER)d->DestroyShader(shader);for(auto t:textures)d->DestroyTexture(t);for(auto b:buffers)d->DestroyBuffer(b);d->Shutdown();}
 ResourceHandle buffer(u64 bytes,BufferType type,GPUMemoryUsage mem,const void* data=nullptr){BufferDesc b{};b.size=bytes;b.type=type;b.memoryUsage=mem;b.name="SPGIPlaceCenterFixture";auto h=d->CreateBuffer(b);check(h!=handles::INVALID_RESOURCE,"buffer");buffers.push_back(h);if(data)check(d->UpdateBufferData(h,data,bytes,0),"buffer upload");return h;}
 VulkanCommandBuffer* begin(CommandBufferHandle& h){h=d->CreateCommandBuffer(CommandQueueType::Compute);auto* c=vk->GetCommandBuffer(h);check(c&&c->Reset()&&c->Begin(),"begin");return c;}
 void finish(CommandBufferHandle h,VulkanCommandBuffer* c){check(c->End()&&c->Submit(0)&&c->WaitForCompletion(),"submit");d->DestroyCommandBuffer(h);}
 ResourceHandle texture(DataFormat format,const void* data,u64 bytes){TextureDesc t{};t.type=TextureType::Texture2D;t.size={W,H,1};t.format=format;t.mipLevels=1;t.usage=TextureUsage::ShaderResource|TextureUsage::CopyDest;auto h=d->CreateTexture(t);check(h!=handles::INVALID_RESOURCE,"texture");textures.push_back(h);auto staging=buffer(bytes,BufferType::Raw,GPUMemoryUsage::Dynamic,data);CommandBufferHandle cmd;auto* c=begin(cmd);BufferTextureCopyRegion r{};r.imageSubresource.layerCount=1;r.imageExtent={W,H,1};c->CopyBufferToTexture(staging,h,&r,1);ResourceBarrier b{};b.resource=h;b.beforeState=ResourceState::CopyDest;b.afterState=ResourceState::ShaderResource;b.subresource=0xffffffffu;b.queueFamily=0xffffffffu;c->InsertBarrier(&b,1);finish(cmd,c);return h;}
 void init(){DescriptorSetLayoutBinding b[]={{0,DescriptorType::SampledImage,1,ShaderStage::Compute,nullptr},{1,DescriptorType::SampledImage,1,ShaderStage::Compute,nullptr},{2,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr},{3,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr},{4,DescriptorType::UniformBuffer,1,ShaderStage::Compute,nullptr},{5,DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr}};b[5].readonly=true;sl=d->CreateDescriptorSetLayout({6,b});check(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT,"layout");pl=d->CreatePipelineLayout({1,&sl});check(pl!=handles::INVALID_PIPELINE_LAYOUT,"pipeline layout");const char* overridePath=std::getenv("SPGI_PLACE_TEST_SPV_OVERRIDE");std::ifstream f(overridePath?overridePath:SPGI_PLACE_TEST_SPV,std::ios::binary|std::ios::ate);check(bool(f),"open private SPV");auto n=f.tellg();check(n>0,"nonempty private SPV");std::vector<u8> code(size_t(n),0);f.seekg(0);f.read(reinterpret_cast<char*>(code.data()),n);check(bool(f),"read private SPV");shader=d->CreateShader(code.data(),code.size(),ShaderStage::Compute,"screen_probe_place");check(shader!=handles::INVALID_SHADER,"production shader");ComputePipelineDesc p{};p.layout=pl;p.computeShader=shader;p.threadGroupSize={64,1,1};pipe=d->CreateComputePipeline(p);check(pipe!=handles::INVALID_PIPELINE,"pipeline");set=d->CreateDescriptorSet({sl});check(set!=handles::INVALID_DESCRIPTOR_SET,"descriptor set");}
};
struct Case {const char* name;bool repr,tier;float seed,maxWorld;};
std::array<std::vector<Vec4>,2> noJitterBaseline;
void run(const Case& test) {
 GPU g;g.init();std::vector<float> depths(W*H);std::vector<Vec4> normals(W*H,{.5f,.5f,1.f,1.f});
 for(u32 y=0;y<H;++y)for(u32 x=0;x<W;++x)depths[y*W+x]=.25f+.001f*float(y*W+x);
 auto dep=g.texture(DataFormat::R32_Float,depths.data(),depths.size()*4),norm=g.texture(DataFormat::RGBA32_Float,normals.data(),normals.size()*sizeof(Vec4));
 const u32 count=GW*GH*(test.tier?4:1);const u64 size=u64(count)*sizeof(Vec4);std::vector<Vec4> initial(count,{-9,-9,-9,-9});
 auto positions=g.buffer(size,BufferType::Raw,GPUMemoryUsage::Dynamic,initial.data()),outputNormals=g.buffer(size,BufferType::Raw,GPUMemoryUsage::Dynamic,initial.data());
 u32 mask[GW*GH]={1,1,1,1};auto var=g.buffer(sizeof(mask),BufferType::Raw,GPUMemoryUsage::Dynamic,mask);
 lumen::ScreenProbeGlobalData cb{};cb.inv_view_projection.columns[0]={1,0,0,0};cb.inv_view_projection.columns[1]={0,1,0,0};cb.inv_view_projection.columns[2]={0,0,1,0};cb.inv_view_projection.columns[3]={0,0,0,1};cb.camera_position.w=test.seed;cb.grid_params={float(GW),float(GH),float(D),48};cb.trace_params={50,2,float(W),float(H)};cb.surface_cache_params.w=test.maxWorld;cb.cache_params.z=.00001f;cb.repr_params={test.repr?1.f:0.f,1.f,.5f,0.f};cb.tier_params={test.tier?1.f:0.f,0.f,test.tier?1.f:0.f,0.f};
 auto constants=g.buffer(sizeof(cb),BufferType::Constant,GPUMemoryUsage::Dynamic,&cb),readback=g.buffer(size,BufferType::Raw,GPUMemoryUsage::Readback);
 DescriptorImageInfo images[2]={{handles::INVALID_SAMPLER,dep,ResourceState::ShaderResource},{handles::INVALID_SAMPLER,norm,ResourceState::ShaderResource}};
 DescriptorBufferInfo buf[4]={{positions,0,size},{outputNormals,0,size},{constants,0,sizeof(cb)},{var,0,sizeof(mask)}};WriteDescriptorSet writes[6]{};
 for(u32 i=0;i<2;++i)writes[i]={g.set,i,0,1,DescriptorType::SampledImage,&images[i],nullptr};
 for(u32 i=0;i<4;++i)writes[i+2]={g.set,i+2,0,1,i==2?DescriptorType::UniformBuffer:DescriptorType::StorageBuffer,nullptr,&buf[i]};g.d->UpdateDescriptorSets(6,writes);
 CommandBufferHandle h;auto* c=g.begin(h);c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::UniformRead);c->BindComputePipeline(g.pipe);c->BindDescriptorSets(PipelineBindPoint::Compute,g.pl,0,1,&g.set,0,nullptr);c->Dispatch((count+63)/64,1,1);c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);c->CopyBuffer(positions,readback,0,0,size);c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);g.finish(h,c);
 auto* p=static_cast<const Vec4*>(g.d->MapBuffer(readback,0,size));check(p,"read probe positions");u32 active=0,edge=0,quadrant=0,changed=0;
 for(u32 i=0;i<count;++i){if(p[i][3]<=0)continue;++active;if(test.tier && (i%4))++quadrant;
   const float sx=(p[i][0]+1)*.5f*W-.5f,sy=(1-p[i][1])*.5f*H-.5f;
   const int x=int(std::lround(sx)),y=int(std::lround(sy));
   check(x>=0 && x<int(W) && y>=0 && y<int(H),"world point inside sampled texel extent");
   check(std::abs(sx-x)<.0002f && std::abs(sy-y)<.0002f,"world position uses exact sampled texel center");
   check(std::abs(p[i][2]-depths[u32(y)*W+u32(x)])<1e-6f,"world Z matches actual point-fetched depth");
   if(x==int(W)-1 || y==int(H)-1 || x==0 || y==0)++edge;
   if(!test.tier && test.seed>=0 && test.maxWorld>1 && !noJitterBaseline[test.repr].empty() &&
      (p[i][0]!=noJitterBaseline[test.repr][i][0] || p[i][1]!=noJitterBaseline[test.repr][i][1]))++changed;
 }
 if(!test.tier && test.seed<0)noJitterBaseline[test.repr]=std::vector<Vec4>(p,p+count);
 if(!test.tier && test.repr && test.seed==8 && test.maxWorld<.01f) {
   // For slot 0 the seed's unconstrained texel is (3,6), but with this
   // world-distance threshold its resample returns representative (4,4).
   check(p[0][3]>0 && std::abs(p[0][0]-(2.f*4.5f/W-1.f))<1e-6f &&
         std::abs(p[0][1]-(1.f-2.f*4.5f/H))<1e-6f,"actual world jitter-clamp resample");
 }
 g.d->UnmapBuffer(readback);check(active>0,"active probes");check(edge>0,"edge clamped sample exercised");
 if(test.tier)check(quadrant>0,"actual quadrant dispatch active");
 if(!test.tier && test.seed>=0 && test.maxWorld>1 && !noJitterBaseline[test.repr].empty())
     check(changed>0,"nonzero jitter changes actual selected texel");
 std::cout<<"{\"case\":\""<<test.name<<"\",\"active\":"<<active<<",\"edge\":"<<edge<<",\"quadrant\":"<<quadrant<<",\"jitter_changed\":"<<changed<<"}\n";
}
}
int main(){try{const Case cases[]={{"repr_off_no_jitter",false,false,-1,0},{"repr_on_no_jitter",true,false,-1,0},{"repr_off_jitter",false,false,4,10},{"repr_on_jitter",true,false,5,10},{"repr_jitter_world_clamp",true,false,8,.001f},{"tier3_quadrants_jitter",true,true,7,10}};
 const char* selected=std::getenv("SPGI_PLACE_TEST_CASE");bool found=false;
 for(const auto& t:cases){if(selected && std::strcmp(selected,t.name)!=0)continue;found=true;run(t);}
 if(!found)throw std::runtime_error("unknown SPGI_PLACE_TEST_CASE");
 return 0;}catch(const std::exception& e){std::cerr<<"SPGI place fixture failed: "<<e.what()<<'\n';return 1;}}
