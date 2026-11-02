#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/Utilities/GeometryBVHStore.h"
#include <fstream>
#include <bit>
#include <iostream>
#include <sstream>
#include <vector>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <limits>
using namespace primal; using namespace primal::graphics;using namespace primal::graphics::rhi;
void require(bool v,const char* s){if(!v)throw std::runtime_error(s);}
struct Query {u32 group,probe;float a[3],b[3];};struct Result {int status;u32 top,bottom,instances,triangles,order;};struct PairStats {u32 nodes,instances,triangles,kind,attemptNodes,attemptInstances,attemptTriangles,pad;u32 attempt0[2][4],attempt1[2][4],retry0[2][4],retry1[2][4];};struct Outputs{std::vector<Result> rays;std::vector<PairStats> pairs;};static_assert(sizeof(Query)==32&&sizeof(Result)==24&&sizeof(PairStats)==160);
struct GPU {
 RHIDeviceBase* d=nullptr;VulkanDevice* vk=nullptr;std::vector<ResourceHandle> buffers;ShaderHandle shader=handles::INVALID_SHADER;DescriptorSetLayoutHandle sl=handles::INVALID_DESCRIPTOR_SET_LAYOUT;PipelineLayoutHandle pl=handles::INVALID_PIPELINE_LAYOUT;DescriptorSetHandle ds=handles::INVALID_DESCRIPTOR_SET;PipelineHandle pipe=handles::INVALID_PIPELINE;
 GPU(){DeviceDesc x{};x.platform=RHIPlatform::Vulkan;x.enableValidation=true;x.enableDebug=true;d=CreateRHIDevice(x);require(d,"Vulkan device");vk=static_cast<VulkanDevice*>(d);}
 ~GPU(){if(!d)return;if(ds!=handles::INVALID_DESCRIPTOR_SET)d->DestroyDescriptorSet(ds);if(pipe!=handles::INVALID_PIPELINE)d->DestroyPipeline(pipe);if(pl!=handles::INVALID_PIPELINE_LAYOUT)d->DestroyPipelineLayout(pl);if(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)d->DestroyDescriptorSetLayout(sl);if(shader!=handles::INVALID_SHADER)d->DestroyShader(shader);for(auto b:buffers)d->DestroyBuffer(b);d->Shutdown();}
 ResourceHandle buffer(size_t n,GPUMemoryUsage usage,const void* data=nullptr){BufferDesc b{};b.size=n;b.type=BufferType::Raw;b.memoryUsage=usage;b.name="EnvironmentPairBoundary";auto h=d->CreateBuffer(b);require(h!=handles::INVALID_RESOURCE,"buffer creation");buffers.push_back(h);if(data)require(d->UpdateBufferData(h,data,n,0),"buffer upload");return h;}
 Outputs run(const char* path,u64 expectedShaderHash,const ray_geometry::SceneBVH& scene,const std::vector<Query>& queries){
  std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f),"open replay SPV");auto sz=f.tellg();require(sz>0,"nonempty replay SPV");std::vector<u8> bytes((size_t)sz);f.seekg(0);require(bool(f.read((char*)bytes.data(),sz)),"read replay SPV");u64 shaderHash=14695981039346656037ull;for(u8 byte:bytes){shaderHash^=byte;shaderHash*=1099511628211ull;}
  std::fprintf(stderr,"[ENV_PAIR_BOUNDARY_SHADER] path=%s fnv1a64=%llu\n",path,static_cast<unsigned long long>(shaderHash));require(shaderHash==expectedShaderHash,"loaded SPV differs from frozen expected FNV");shader=d->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"main");require(shader!=handles::INVALID_SHADER,"shader");
  const u32 indices[]={0,1,2,13,14,15,16};std::array<DescriptorSetLayoutBinding,7> bindings{};for(u32 i=0;i<7;++i){bindings[i]={indices[i],DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};bindings[i].readonly=i!=1 && i!=2;}
  sl=d->CreateDescriptorSetLayout({u32(bindings.size()),bindings.data()});require(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT,"set layout");pl=d->CreatePipelineLayout({1,&sl});require(pl!=handles::INVALID_PIPELINE_LAYOUT,"pipeline layout");ds=d->CreateDescriptorSet({sl});require(ds!=handles::INVALID_DESCRIPTOR_SET,"set");ComputePipelineDesc pd{};pd.layout=pl;pd.computeShader=shader;pipe=d->CreateComputePipeline(pd);require(pipe!=handles::INVALID_PIPELINE,"pipeline");
  std::vector<u8> sentinel(queries.size()*sizeof(Result),0xcd),statsSentinel((queries.size()/2)*sizeof(PairStats),0xcd);const void* data[]={queries.data(),sentinel.data(),statsSentinel.data(),scene.top.data(),scene.bottom.data(),scene.triangles.data(),scene.instances.data()};const size_t sizes[]={queries.size()*sizeof(Query),sentinel.size(),statsSentinel.size(),scene.top.size()*sizeof(ray_geometry::Node),scene.bottom.size()*sizeof(ray_geometry::Node),scene.triangles.size()*sizeof(ray_geometry::Triangle),scene.instances.size()*sizeof(ray_geometry::Instance)};
  std::array<DescriptorBufferInfo,7> infos{};std::array<WriteDescriptorSet,7> writes{};for(u32 i=0;i<7;++i){auto h=buffer(sizes[i],GPUMemoryUsage::Dynamic,data[i]);infos[i]={h,0,sizes[i]};writes[i]={ds,indices[i],0,1,DescriptorType::StorageBuffer,nullptr,&infos[i]};}d->UpdateDescriptorSets(writes.size(),writes.data());auto rb=buffer(sentinel.size(),GPUMemoryUsage::Readback);auto statsRb=buffer(statsSentinel.size(),GPUMemoryUsage::Readback);
  auto cmd=d->CreateCommandBuffer(CommandQueueType::Compute);auto* c=vk->GetCommandBuffer(cmd);require(c&&c->Reset()&&c->Begin(),"begin");c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);c->BindComputePipeline(pipe);c->BindDescriptorSets(PipelineBindPoint::Compute,pl,0,1,&ds,0,nullptr);c->Dispatch(((queries.size()+1)/2+63)/64,1,1);c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);c->CopyBuffer(infos[1].buffer,rb,0,0,sentinel.size());c->CopyBuffer(infos[2].buffer,statsRb,0,0,statsSentinel.size());c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);// A failed End has not been submitted. Submit returning false is not
  // accepted as proof that no work reached the queue; after a successful
  // Submit, failed completion is likewise unknown. Exit this standalone test
  // without unwinding or destroying possibly in-flight resources.
  const auto failWithoutCleanup=[](const char* operation) {
   std::fprintf(stderr,"[ENV_PAIR_BOUNDARY] FAIL %s; completion unavailable, exiting without releasing potentially pending resources\n",operation);
   std::fflush(stderr);std::_Exit(7);
  };
  if(!c->End())failWithoutCleanup("command end before submit");
  if(!c->Submit(0))failWithoutCleanup("command submit outcome");
  if(!c->WaitForCompletion())failWithoutCleanup("command completion after submit");
  d->DestroyCommandBuffer(cmd);auto* p=d->MapBuffer(rb,0,sentinel.size());require(p,"map results");Outputs r;r.rays.resize(queries.size());std::memcpy(r.rays.data(),p,sentinel.size());d->UnmapBuffer(rb);p=d->MapBuffer(statsRb,0,statsSentinel.size());require(p,"map pair statistics");r.pairs.resize(queries.size()/2);std::memcpy(r.pairs.data(),p,statsSentinel.size());d->UnmapBuffer(statsRb);return r;
 }
};
int main(){try{
 ray_geometry::SceneBVH scene;ray_geometry::Node top{},bottom{};
 for(u32 k=0;k<3;++k){top.lo[k]=bottom.lo[k]=-10.f;top.hi[k]=bottom.hi[k]=10.f;}
 top.links[2]=0;top.links[3]=1;bottom.links[2]=0;bottom.links[3]=1;scene.top.push_back(top);scene.bottom.push_back(bottom);
 ray_geometry::Triangle tri{};tri.a[0]=-1.f;tri.a[1]=-1.f;tri.a[2]=.75f;tri.e1[0]=2.f;tri.e2[0]=1.f;tri.e2[1]=2.f;scene.triangles.push_back(tri);
 ray_geometry::Instance inst{};for(u32 i=0;i<3;++i)inst.inverse_rows[i][i]=1.f;scene.instances.push_back(inst);
 auto comb=[&](u32 leaves){ray_geometry::SceneBVH tree;tree.bottom=scene.bottom;tree.triangles=scene.triangles;
  auto add=[&](auto&& self,u32 count)->u32 {u32 at=u32(tree.top.size());auto node=top;tree.top.push_back(node);
   if(count==1){auto instance=inst;instance.instance_id=u32(tree.instances.size());tree.top[at].links[2]=u32(tree.instances.size());tree.top[at].links[3]=1;tree.instances.push_back(instance);}
   else {u32 deep=self(self,count-1),sibling=self(self,1);tree.top[at].links[0]=sibling;tree.top[at].links[1]=deep;tree.top[at].links[2]=tree.top[at].links[3]=0;}return at;};add(add,leaves);
  for(u32 k=1;k<tree.top.size();++k)tree.top[k].hi[0]=0.f;const u32 left=tree.top[0].links[0];tree.top[left].lo[0]=0.f;tree.top[left].hi[0]=10.f;return tree;};
 auto bottomComb=[&](u32 leaves){ray_geometry::SceneBVH tree;tree.top=scene.top;tree.instances=scene.instances;tree.triangles=scene.triangles;
  auto add=[&](auto&& self,u32 count)->u32{u32 at=u32(tree.bottom.size());tree.bottom.push_back(bottom);
   if(count>1){u32 deep=self(self,count-1),sibling=self(self,1);tree.bottom[at].links[0]=sibling;tree.bottom[at].links[1]=deep;tree.bottom[at].links[2]=tree.bottom[at].links[3]=0;}return at;};add(add,leaves);
  for(u32 k=1;k<tree.bottom.size();++k)tree.bottom[k].hi[0]=0.f;u32 left=tree.bottom[0].links[0];tree.bottom[left].lo[0]=0.f;tree.bottom[left].hi[0]=10.f;return tree;};
 for(bool blas:{false,true})for(u32 leaves:{64u,65u}){auto tree=blas?bottomComb(leaves):comb(leaves);Query longRay{0,0,{-.25f,0,2.f},{-.25f,0,3.f}},shortRay{0,1,{.25f,0,.5f},{.25f,0,1.f}};std::vector<Query> input{longRay,shortRay};
  Outputs original,paired,small;{GPU gpu;original=gpu.run(ENV_BOUNDARY_REFERENCE_SPV,10634993464813357995ull,tree,input);}{GPU gpu;paired=gpu.run(ENV_BOUNDARY_PAIR_SPV,9505221594127124166ull,tree,input);}{GPU gpu;small=gpu.run(ENV_BOUNDARY_SMALL_SPV,17299814406941537054ull,tree,input);}
  for(u32 r=0;r<2;++r){auto x=original.rays[r];for(auto y:{paired.rays[r],small.rays[r]})require(x.status==y.status&&x.top==y.top&&x.bottom==y.bottom&&x.instances==y.instances&&x.triangles==y.triangles&&x.order==y.order,"environment scalar/pair/retry ray status, counts and order");}
  const auto slow=original.rays[0],fast=original.rays[1];require(fast.status==1&&fast.top==(blas?1u:3u)&&fast.bottom==(blas?3u:1u)&&fast.instances==1&&fast.triangles==1,"independent fast ray hit");
  if(leaves==64)require(slow.status==0&&slow.top==(blas?1u:127u)&&slow.bottom==(blas?127u:63u)&&slow.instances==(blas?1u:63u)&&slow.triangles==63,"64 pending completes as open");else require(slow.status==-1&&slow.top==(blas?1u:64u)&&slow.bottom==(blas?64u:0u)&&slow.instances==(blas?1u:0u)&&slow.triangles==0,"65 pending returns unknown");
  const auto& st=small.pairs[0];require(paired.pairs[0].kind==1,"actual shared pair");require(st.kind==3&&st.attemptNodes==(blas?2u:1u)&&st.attemptInstances==(blas?1u:0u)&&st.attemptTriangles==0,"forced attempt exact node instance triangle counts");
  require(st.nodes==st.attemptNodes+slow.top+slow.bottom+fast.top+fast.bottom && st.instances==st.attemptInstances+slow.instances+fast.instances && st.triangles==st.attemptTriangles+slow.triangles+fast.triangles,"forced retry includes all attempt and scalar work");
  auto equalInput=[&](u32 lane,const char* mode,const char* component,u32 expected,u32 actual){if(expected!=actual){std::fprintf(stderr,"[ENV_PAIR_BOUNDARY_INPUT_MISMATCH] tree=%s leaves=%u lane=%u mode=%s component=%s expected=%08x actual=%08x\n",blas?"BLAS":"TLAS",leaves,lane,mode,component,expected,actual);throw std::runtime_error("actual query input mismatch");}};
  static constexpr const char* originComponents[]={"origin.x","origin.y","origin.z","near"};static constexpr const char* directionComponents[]={"direction.x","direction.y","direction.z","far"};
  for(u32 lane=0;lane<2;++lane){for(u32 word=0;word<4;++word){equalInput(lane,"attempt_vs_retry",originComponents[word],st.attempt0[lane][word],st.retry0[lane][word]);equalInput(lane,"attempt_vs_retry",directionComponents[word],st.attempt1[lane][word],st.retry1[lane][word]);equalInput(lane,"scalar_vs_retry",originComponents[word],original.pairs[0].retry0[lane][word],st.retry0[lane][word]);equalInput(lane,"scalar_vs_retry",directionComponents[word],original.pairs[0].retry1[lane][word],st.retry1[lane][word]);}equalInput(lane,"expected_distance","far",std::bit_cast<u32>(lane==0?1.f:.5f),st.retry1[lane][3]);}
  std::fprintf(stderr,"[ENV_PAIR_BOUNDARY] tree=%s leaves=%u slow_status=%d slow_top=%u slow_bottom=%u slow_instances=%u slow_triangles=%u fast_status=%d fast_top=%u fast_bottom=%u pair=1 forced_fallback=1 attempt_nodes=%u attempt_instances=%u attempt_triangles=%u total_nodes=%u total_instances=%u total_triangles=%u inputs_exact=1\n",blas?"BLAS":"TLAS",leaves,slow.status,slow.top,slow.bottom,slow.instances,slow.triangles,fast.status,fast.top,fast.bottom,st.attemptNodes,st.attemptInstances,st.attemptTriangles,st.nodes,st.instances,st.triangles);
 }
 return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"[ENV_PAIR_BOUNDARY] FAIL %s\n",e.what());return 1;}}
