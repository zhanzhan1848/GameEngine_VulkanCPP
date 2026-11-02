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
struct Query {u32 group,probe;float a[3],b[3];};struct Result {int status;u32 top,bottom,instances,triangles,order;};struct PairStats {u32 nodes,instances,triangles,kind,attemptNodes,attemptInstances,attemptTriangles,pad;};struct Outputs{std::vector<Result> rays;std::vector<PairStats> pairs;};static_assert(sizeof(Query)==32&&sizeof(Result)==24&&sizeof(PairStats)==32);
struct GPU {
 RHIDeviceBase* d=nullptr;VulkanDevice* vk=nullptr;std::vector<ResourceHandle> buffers;ShaderHandle shader=handles::INVALID_SHADER;DescriptorSetLayoutHandle sl=handles::INVALID_DESCRIPTOR_SET_LAYOUT;PipelineLayoutHandle pl=handles::INVALID_PIPELINE_LAYOUT;DescriptorSetHandle ds=handles::INVALID_DESCRIPTOR_SET;PipelineHandle pipe=handles::INVALID_PIPELINE;
 GPU(){DeviceDesc x{};x.platform=RHIPlatform::Vulkan;x.enableValidation=true;x.enableDebug=true;d=CreateRHIDevice(x);require(d,"Vulkan device");vk=static_cast<VulkanDevice*>(d);}
 ~GPU(){if(!d)return;if(ds!=handles::INVALID_DESCRIPTOR_SET)d->DestroyDescriptorSet(ds);if(pipe!=handles::INVALID_PIPELINE)d->DestroyPipeline(pipe);if(pl!=handles::INVALID_PIPELINE_LAYOUT)d->DestroyPipelineLayout(pl);if(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)d->DestroyDescriptorSetLayout(sl);if(shader!=handles::INVALID_SHADER)d->DestroyShader(shader);for(auto b:buffers)d->DestroyBuffer(b);d->Shutdown();}
 ResourceHandle buffer(size_t n,GPUMemoryUsage usage,const void* data=nullptr){BufferDesc b{};b.size=n;b.type=BufferType::Raw;b.memoryUsage=usage;b.name="EqualBoxReplay";auto h=d->CreateBuffer(b);require(h!=handles::INVALID_RESOURCE,"buffer creation");buffers.push_back(h);if(data)require(d->UpdateBufferData(h,data,n,0),"buffer upload");return h;}
 Outputs run(const char* path,u64 expectedShaderHash,const ray_geometry::SceneBVH& scene,const std::vector<Query>& queries){
  std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f),"open replay SPV");auto sz=f.tellg();require(sz>0,"nonempty replay SPV");std::vector<u8> bytes((size_t)sz);f.seekg(0);require(bool(f.read((char*)bytes.data(),sz)),"read replay SPV");u64 shaderHash=14695981039346656037ull;for(u8 byte:bytes){shaderHash^=byte;shaderHash*=1099511628211ull;}
  std::fprintf(stderr,"[DDGI_CROSS_RAY_REPLAY_SHADER] path=%s fnv1a64=%llu\n",path,static_cast<unsigned long long>(shaderHash));require(shaderHash==expectedShaderHash,"loaded SPV differs from frozen expected FNV");shader=d->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"main");require(shader!=handles::INVALID_SHADER,"shader");
  const u32 indices[]={0,1,2,13,14,15,16};std::array<DescriptorSetLayoutBinding,7> bindings{};for(u32 i=0;i<7;++i){bindings[i]={indices[i],DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};bindings[i].readonly=i!=1 && i!=2;}
  sl=d->CreateDescriptorSetLayout({u32(bindings.size()),bindings.data()});require(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT,"set layout");pl=d->CreatePipelineLayout({1,&sl});require(pl!=handles::INVALID_PIPELINE_LAYOUT,"pipeline layout");ds=d->CreateDescriptorSet({sl});require(ds!=handles::INVALID_DESCRIPTOR_SET,"set");ComputePipelineDesc pd{};pd.layout=pl;pd.computeShader=shader;pipe=d->CreateComputePipeline(pd);require(pipe!=handles::INVALID_PIPELINE,"pipeline");
  std::vector<u8> sentinel(queries.size()*sizeof(Result),0xcd),statsSentinel((queries.size()/2)*sizeof(PairStats),0xcd);const void* data[]={queries.data(),sentinel.data(),statsSentinel.data(),scene.top.data(),scene.bottom.data(),scene.triangles.data(),scene.instances.data()};const size_t sizes[]={queries.size()*sizeof(Query),sentinel.size(),statsSentinel.size(),scene.top.size()*sizeof(ray_geometry::Node),scene.bottom.size()*sizeof(ray_geometry::Node),scene.triangles.size()*sizeof(ray_geometry::Triangle),scene.instances.size()*sizeof(ray_geometry::Instance)};
  std::array<DescriptorBufferInfo,7> infos{};std::array<WriteDescriptorSet,7> writes{};for(u32 i=0;i<7;++i){auto h=buffer(sizes[i],GPUMemoryUsage::Dynamic,data[i]);infos[i]={h,0,sizes[i]};writes[i]={ds,indices[i],0,1,DescriptorType::StorageBuffer,nullptr,&infos[i]};}d->UpdateDescriptorSets(writes.size(),writes.data());auto rb=buffer(sentinel.size(),GPUMemoryUsage::Readback);auto statsRb=buffer(statsSentinel.size(),GPUMemoryUsage::Readback);
  auto cmd=d->CreateCommandBuffer(CommandQueueType::Compute);auto* c=vk->GetCommandBuffer(cmd);require(c&&c->Reset()&&c->Begin(),"begin");c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);c->BindComputePipeline(pipe);c->BindDescriptorSets(PipelineBindPoint::Compute,pl,0,1,&ds,0,nullptr);c->Dispatch(((queries.size()+1)/2+63)/64,1,1);c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);c->CopyBuffer(infos[1].buffer,rb,0,0,sentinel.size());c->CopyBuffer(infos[2].buffer,statsRb,0,0,statsSentinel.size());c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);// A failed End has not been submitted. Submit returning false is not
  // accepted as proof that no work reached the queue; after a successful
  // Submit, failed completion is likewise unknown. Exit this standalone test
  // without unwinding or destroying possibly in-flight resources.
  const auto failWithoutCleanup=[](const char* operation) {
   std::fprintf(stderr,"[DDGI_CROSS_RAY_REPLAY] FAIL %s; completion unavailable, exiting without releasing potentially pending resources\n",operation);
   std::fflush(stderr);std::_Exit(7);
  };
  if(!c->End())failWithoutCleanup("command end before submit");
  if(!c->Submit(0))failWithoutCleanup("command submit outcome");
  if(!c->WaitForCompletion())failWithoutCleanup("command completion after submit");
  d->DestroyCommandBuffer(cmd);auto* p=d->MapBuffer(rb,0,sentinel.size());require(p,"map results");Outputs r;r.rays.resize(queries.size());std::memcpy(r.rays.data(),p,sentinel.size());d->UnmapBuffer(rb);p=d->MapBuffer(statsRb,0,statsSentinel.size());require(p,"map pair statistics");r.pairs.resize(queries.size()/2);std::memcpy(r.pairs.data(),p,statsSentinel.size());d->UnmapBuffer(statsRb);return r;
 }
};
#ifdef PAIR_SYNTHETIC
int main(){try{
 ray_geometry::SceneBVH scene;ray_geometry::Node top{},bottom{};
 for(u32 k=0;k<3;++k){top.lo[k]=bottom.lo[k]=-10.f;top.hi[k]=bottom.hi[k]=10.f;}
 top.links[2]=0;top.links[3]=1;bottom.links[2]=0;bottom.links[3]=1;scene.top.push_back(top);scene.bottom.push_back(bottom);
 ray_geometry::Triangle tri{};tri.a[0]=-1.f;tri.a[1]=-1.f;tri.a[2]=.75f;tri.e1[0]=2.f;tri.e2[0]=1.f;tri.e2[1]=2.f;scene.triangles.push_back(tri);
 ray_geometry::Instance inst{};for(u32 i=0;i<3;++i)inst.inverse_rows[i][i]=1.f;scene.instances.push_back(inst);
 const float nan=std::numeric_limits<float>::quiet_NaN();
 Query hit{0,0,{0,0,.5f},{0,0,1.f}},unknown{0,1,{-.5f,0,.75f},{.5f,0,.75f}},invalid{0,0,{nan,0,.5f},{0,0,1.f}},open{0,0,{0,0,2.f},{0,0,3.f}};
 std::vector<Query> q{hit,unknown,unknown,hit,invalid,hit,invalid,invalid,open,open};
 Outputs a,b;{GPU gpu;a=gpu.run(REPLAY_REFERENCE_SPV,11935473351958342571ull,scene,q);}{GPU gpu;b=gpu.run(REPLAY_CANDIDATE_SPV,6915313621516803618ull,scene,q);}
 const int status[]={-1,0,0,-1,0,-1,0,0,1,1};
 for(u32 i=0;i<q.size();++i){const auto x=a.rays[i],y=b.rays[i];require(x.status==status[i] && x.status==y.status && x.top==y.top && x.bottom==y.bottom && x.instances==y.instances && x.triangles==y.triangles && x.order==y.order,"synthetic status, counts and order digest");}
 require(b.pairs[0].kind==1 && b.pairs[1].kind==1 && b.pairs[2].kind==2 && b.pairs[3].kind==2 && b.pairs[4].kind==1,"two, one, zero eligible pairs classified");
 require(a.rays[1].triangles>0 && a.rays[0].triangles>0 && !a.rays[4].top && !a.rays[6].top && !a.rays[7].top,"mixed nearparallel UNKNOWN and invalid endpoints trace qualification");
 // The long right-first comb gives ray 0 all pending siblings. Ray 1
 // misses the right subtree, then reaches the left sibling and hits. At
 // 64 leaves ray 0 completes; at 65 its next virtual push must be UNKNOWN.
 auto comb=[&](u32 leaves){ray_geometry::SceneBVH tree;tree.bottom=scene.bottom;tree.triangles=scene.triangles;
  auto add=[&](auto&& self,u32 count)->u32 {u32 at=u32(tree.top.size());auto node=top;tree.top.push_back(node);
   if(count==1){auto instance=inst;instance.instance_id=u32(tree.instances.size());tree.top[at].links[2]=u32(tree.instances.size());tree.top[at].links[3]=1;tree.instances.push_back(instance);}
   else {u32 deep=self(self,count-1),sibling=self(self,1);tree.top[at].links[0]=sibling;tree.top[at].links[1]=deep;tree.top[at].links[2]=tree.top[at].links[3]=0;}
   return at;};add(add,leaves);
  for(u32 k=1;k<tree.top.size();++k)tree.top[k].hi[0]=0.f;
  const u32 left=tree.top[0].links[0];tree.top[left].lo[0]=0.f;tree.top[left].hi[0]=10.f;
  return tree;};
 for(u32 leaves:{64u,65u}){auto tree=comb(leaves);Query longRay{0,0,{-.25f,0,2.f},{-.25f,0,3.f}},shortRay{0,1,{.25f,0,.5f},{.25f,0,1.f}};std::vector<Query> input{longRay,shortRay};
  Outputs original,paired;{GPU gpu;original=gpu.run(REPLAY_REFERENCE_SPV,11935473351958342571ull,tree,input);}{GPU gpu;paired=gpu.run(REPLAY_CANDIDATE_SPV,6915313621516803618ull,tree,input);}
  for(u32 r=0;r<2;++r){auto x=original.rays[r],y=paired.rays[r];require(x.status==y.status&&x.top==y.top&&x.bottom==y.bottom&&x.instances==y.instances&&x.triangles==y.triangles&&x.order==y.order,"virtual capacity query state counts order");}
  const auto slow=original.rays[0],fast=original.rays[1];
  require(fast.status==-1&&fast.top==3&&fast.instances==1&&fast.triangles==1,"short ray reaches independent left hit");
  if(leaves==64)require(slow.status==1&&slow.top==127&&slow.instances==63&&slow.triangles==63,"virtual capacity64 completes original traversal");
  else require(slow.status==0&&slow.top==64&&slow.instances==0&&slow.triangles==0,"virtual 64-to-65 push yields original UNKNOWN");
  require(paired.pairs[0].kind==1,"virtual capacity uses actual pair path");
  std::cout<<"[DDGI_CROSS_RAY_VIRTUAL_STACK] leaves="<<leaves<<" slow_status="<<slow.status<<" slow_top="<<slow.top<<" slow_instances="<<slow.instances<<" slow_triangles="<<slow.triangles<<" fast_status="<<fast.status<<" fast_top="<<fast.top<<" pair_used=1\n";
 }
 std::cout<<"[DDGI_CROSS_RAY_SYNTHETIC] cases=7 mixed_hit_unknown=3 eligible_two=5 eligible_one=1 eligible_zero=1 compared=14\n";return 0;
 }catch(const std::exception& e){std::cerr<<"[DDGI_CROSS_RAY_SYNTHETIC] FAIL "<<e.what()<<"\n";return 1;}}
#else
int main(){try{ray_geometry::SceneBVH scene;require(ray_geometry::LoadSceneBVH(".primal-cache/geometry-bvh/5d07eba5b7554607.tlas",6703585667342222855ull,scene),"original scene");auto cert=ray_geometry::CertifyCompactTraversal(scene);require(cert.valid&&cert.top_max_pending<=64&&cert.bottom_max_pending<=64,"original scene validated");
 for(const char* view:{"floor","atrium"}){std::string base="Docs/reports/primal-ddgi-query-kernel-20261030/tlas-screen/";std::ifstream in(base+view+".bin",std::ios::binary);require(bool(in),"recorded binary");std::vector<Query> q;Query v;while(in.read((char*)&v,sizeof(v)))q.push_back(v);require(in.eof()&&q.size()==4672,"exact query count");std::string gpath="Docs/reports/primal-ddgi-gpu-traversal-20261030/capture-reference-"+std::string(view)+"/queries.csv";std::ifstream gold(gpath);require(bool(gold),"recorded GPU CSV");std::string line;std::getline(gold,line);std::vector<std::array<int,5>> expected;while(std::getline(gold,line)){std::stringstream ss(line);std::string c;std::vector<std::string> fields;while(std::getline(ss,c,','))fields.push_back(c);require(fields.size()==18,"CSV format");const auto index=expected.size();require(index<q.size(),"CSV has no excess rows");require(q[index].group==u32(std::stoul(fields[0])) && q[index].probe==u32(std::stoul(fields[4])) && u32(std::stoul(fields[3]))==index%8 && q[index].group==index/8,"query IDs match GPU source");
  for(u32 k=0;k<6;++k){const float value=k<3?q[index].a[k]:q[index].b[k-3];require(std::bit_cast<u32>(value)==u32(std::stoul(fields[12+k],nullptr,16)),"query endpoint bits match GPU source");}
  require(std::stoi(fields[11])==0,"GPU source flags clear");int reason=std::stoi(fields[6]);require(reason==3||reason==4||reason==5,"known GPU result reason");require(std::stoi(fields[5])==(reason==4?1:0),"GPU result reason consistent");expected.push_back({reason==4?1:reason==5?-1:0,std::stoi(fields[7]),std::stoi(fields[8]),std::stoi(fields[9]),std::stoi(fields[10])});}require(expected.size()==q.size(),"expected rows");
  // FNV checks bind the bytes actually read by each GPU instance to these
  // frozen modules; also compare the two complete files before dispatch.
  constexpr u64 refHash=11935473351958342571ull;
#ifdef PAIR_FORCE_OVERFLOW
  constexpr u64 candidateHash=7618862715215647654ull;
#else
  constexpr u64 candidateHash=6915313621516803618ull;
#endif
  auto shaderBytes=[](const char* path){std::ifstream f(path,std::ios::binary);require(bool(f),"open frozen shader");return std::vector<char>(std::istreambuf_iterator<char>(f),{});};
  const auto refBytes=shaderBytes(REPLAY_REFERENCE_SPV),candidateBytes=shaderBytes(REPLAY_CANDIDATE_SPV);
  require(!refBytes.empty() && !candidateBytes.empty() && refBytes!=candidateBytes && refHash!=candidateHash,"reference and candidate SPV bytes must differ");
  Outputs a,b;{GPU gpu;a=gpu.run(REPLAY_REFERENCE_SPV,refHash,scene,q);}{GPU gpu;b=gpu.run(REPLAY_CANDIDATE_SPV,candidateHash,scene,q);}u64 logical=0,loads=0,paired=0,preflight=0,overflow=0,attempt=0,instances=0,triangles=0;for(size_t i=0;i<q.size();++i){auto x=a.rays[i],y=b.rays[i];require(x.status==expected[i][0]&&int(x.top)==expected[i][1]&&int(x.bottom)==expected[i][2]&&int(x.instances)==expected[i][3]&&int(x.triangles)==expected[i][4],"GPU reference mismatch recorded source");require(x.status==y.status&&x.top==y.top&&x.bottom==y.bottom&&x.instances==y.instances&&x.triangles==y.triangles&&x.order==y.order,"candidate per-ray state counts or traversal-order digest differ");logical+=x.top+x.bottom;}
  for(auto p:a.pairs)require(p.kind==0 && !p.attemptNodes && !p.attemptInstances && !p.attemptTriangles,"reference used only scalar path");
  for(size_t i=0;i<b.pairs.size();++i){const auto p=b.pairs[i],ref=a.pairs[i];require(p.kind>=1 && p.kind<=3,"candidate output category written");
   if(p.kind==1){++paired;require(p.nodes==p.attemptNodes && p.instances==p.attemptInstances && p.triangles==p.attemptTriangles,"successful pair accounts its whole work");}
   else if(p.kind==2){++preflight;require(!p.attemptNodes && !p.attemptInstances && !p.attemptTriangles && p.nodes==ref.nodes && p.instances==ref.instances && p.triangles==ref.triangles,"preflight scalar accounts both reference queries");}
   else {++overflow;require(p.attemptNodes && p.nodes==p.attemptNodes+ref.nodes && p.instances==p.attemptInstances+ref.instances && p.triangles==p.attemptTriangles+ref.triangles,"overflow accounts attempt plus both reference queries");}
   loads+=p.nodes;attempt+=p.attemptNodes;instances+=p.instances;triangles+=p.triangles;}
#ifdef PAIR_FORCE_OVERFLOW
  require(paired+preflight+overflow==q.size()/2 && overflow>0,"forced bounded physical stack triggers scalar rerun");
#else
  require(paired+preflight+overflow==q.size()/2 && paired>0,"every candidate pair classified and paired work present");
  require(loads<logical && (logical-loads)*100>=logical*15,"positive >=15 percent node payload reuse including fallback work");
#endif
  std::cout<<"[DDGI_CROSS_RAY_REPLAY] view="<<view<<" pairs="<<q.size()/2<<" queries="<<q.size()<<" eligible_pairs="<<paired+overflow<<" paired_pairs="<<paired<<" preflight_scalar_pairs="<<preflight<<" physical_overflow_pairs="<<overflow<<" fallback_pairs="<<preflight+overflow<<" logical_nodes="<<logical<<" node_loads="<<loads<<" instance_loads="<<instances<<" triangle_loads="<<triangles<<" attempted_pair_nodes="<<attempt<<" gpu_source_matches=1\n";
 }return 0;}catch(const std::exception& e){std::cerr<<"[DDGI_CROSS_RAY_REPLAY] FAIL "<<e.what()<<"\n";return 1;}}

#endif
