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
using namespace primal; using namespace primal::graphics;using namespace primal::graphics::rhi;
void require(bool v,const char* s){if(!v)throw std::runtime_error(s);}
struct Query {u32 group,probe;float a[3],b[3];};struct Result {int status;u32 top,bottom,instances,triangles,loads,hits,visits;};static_assert(sizeof(Query)==32&&sizeof(Result)==32);
struct GPU {
 RHIDeviceBase* d=nullptr;VulkanDevice* vk=nullptr;std::vector<ResourceHandle> buffers;ShaderHandle shader=handles::INVALID_SHADER;DescriptorSetLayoutHandle sl=handles::INVALID_DESCRIPTOR_SET_LAYOUT;PipelineLayoutHandle pl=handles::INVALID_PIPELINE_LAYOUT;DescriptorSetHandle ds=handles::INVALID_DESCRIPTOR_SET;PipelineHandle pipe=handles::INVALID_PIPELINE;
 GPU(){DeviceDesc x{};x.platform=RHIPlatform::Vulkan;x.enableValidation=true;x.enableDebug=true;d=CreateRHIDevice(x);require(d,"Vulkan device");vk=static_cast<VulkanDevice*>(d);}
 ~GPU(){if(!d)return;if(ds!=handles::INVALID_DESCRIPTOR_SET)d->DestroyDescriptorSet(ds);if(pipe!=handles::INVALID_PIPELINE)d->DestroyPipeline(pipe);if(pl!=handles::INVALID_PIPELINE_LAYOUT)d->DestroyPipelineLayout(pl);if(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT)d->DestroyDescriptorSetLayout(sl);if(shader!=handles::INVALID_SHADER)d->DestroyShader(shader);for(auto b:buffers)d->DestroyBuffer(b);d->Shutdown();}
 ResourceHandle buffer(size_t n,GPUMemoryUsage usage,const void* data=nullptr){BufferDesc b{};b.size=n;b.type=BufferType::Raw;b.memoryUsage=usage;b.name="EqualBoxReplay";auto h=d->CreateBuffer(b);require(h!=handles::INVALID_RESOURCE,"buffer creation");buffers.push_back(h);if(data)require(d->UpdateBufferData(h,data,n,0),"buffer upload");return h;}
 std::vector<Result> run(const char* path,const ray_geometry::SceneBVH& scene,const std::vector<Query>& queries){
  std::ifstream f(path,std::ios::binary|std::ios::ate);require(bool(f),"open replay SPV");auto sz=f.tellg();require(sz>0,"nonempty replay SPV");std::vector<u8> bytes((size_t)sz);f.seekg(0);require(bool(f.read((char*)bytes.data(),sz)),"read replay SPV");u64 shaderHash=14695981039346656037ull;for(u8 byte:bytes){shaderHash^=byte;shaderHash*=1099511628211ull;}
  std::fprintf(stderr,"[DDGI_INSTANCE_PAYLOAD_REPLAY_SHADER] path=%s fnv1a64=%llu\n",path,static_cast<unsigned long long>(shaderHash));shader=d->CreateShader(bytes.data(),bytes.size(),ShaderStage::Compute,"main");require(shader!=handles::INVALID_SHADER,"shader");
  const u32 indices[]={0,1,13,14,15,16};std::array<DescriptorSetLayoutBinding,6> bindings{};for(u32 i=0;i<6;++i){bindings[i]={indices[i],DescriptorType::StorageBuffer,1,ShaderStage::Compute,nullptr};bindings[i].readonly=i!=1;}
  sl=d->CreateDescriptorSetLayout({u32(bindings.size()),bindings.data()});require(sl!=handles::INVALID_DESCRIPTOR_SET_LAYOUT,"set layout");pl=d->CreatePipelineLayout({1,&sl});require(pl!=handles::INVALID_PIPELINE_LAYOUT,"pipeline layout");ds=d->CreateDescriptorSet({sl});require(ds!=handles::INVALID_DESCRIPTOR_SET,"set");ComputePipelineDesc pd{};pd.layout=pl;pd.computeShader=shader;pipe=d->CreateComputePipeline(pd);require(pipe!=handles::INVALID_PIPELINE,"pipeline");
  std::vector<u8> sentinel(queries.size()*sizeof(Result),0xcd);const void* data[]={queries.data(),sentinel.data(),scene.top.data(),scene.bottom.data(),scene.triangles.data(),scene.instances.data()};const size_t sizes[]={queries.size()*sizeof(Query),sentinel.size(),scene.top.size()*sizeof(ray_geometry::Node),scene.bottom.size()*sizeof(ray_geometry::Node),scene.triangles.size()*sizeof(ray_geometry::Triangle),scene.instances.size()*sizeof(ray_geometry::Instance)};
  std::array<DescriptorBufferInfo,6> infos{};std::array<WriteDescriptorSet,6> writes{};for(u32 i=0;i<6;++i){auto h=buffer(sizes[i],GPUMemoryUsage::Dynamic,data[i]);infos[i]={h,0,sizes[i]};writes[i]={ds,indices[i],0,1,DescriptorType::StorageBuffer,nullptr,&infos[i]};}d->UpdateDescriptorSets(writes.size(),writes.data());auto rb=buffer(sentinel.size(),GPUMemoryUsage::Readback);
  auto cmd=d->CreateCommandBuffer(CommandQueueType::Compute);auto* c=vk->GetCommandBuffer(cmd);require(c&&c->Reset()&&c->Begin(),"begin");c->MemoryBarrier(PipelineStage::Host,PipelineStage::ComputeShader,AccessFlag::HostWrite,AccessFlag::ShaderRead|AccessFlag::ShaderWrite);c->BindComputePipeline(pipe);c->BindDescriptorSets(PipelineBindPoint::Compute,pl,0,1,&ds,0,nullptr);c->Dispatch(((queries.size()+7)/8+63)/64,1,1);c->MemoryBarrier(PipelineStage::ComputeShader,PipelineStage::Transfer,AccessFlag::ShaderWrite,AccessFlag::TransferRead);c->CopyBuffer(infos[1].buffer,rb,0,0,sentinel.size());c->MemoryBarrier(PipelineStage::Transfer,PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);// A failed End has not been submitted. Submit returning false is not
  // accepted as proof that no work reached the queue; after a successful
  // Submit, failed completion is likewise unknown. Exit this standalone test
  // without unwinding or destroying possibly in-flight resources.
  const auto failWithoutCleanup=[](const char* operation) {
   std::fprintf(stderr,"[DDGI_INSTANCE_PAYLOAD_REPLAY] FAIL %s; completion unavailable, exiting without releasing potentially pending resources\n",operation);
   std::fflush(stderr);std::_Exit(7);
  };
  if(!c->End())failWithoutCleanup("command end before submit");
  if(!c->Submit(0))failWithoutCleanup("command submit outcome");
  if(!c->WaitForCompletion())failWithoutCleanup("command completion after submit");
  d->DestroyCommandBuffer(cmd);auto* p=d->MapBuffer(rb,0,sentinel.size());require(p,"map results");std::vector<Result> r(queries.size());std::memcpy(r.data(),p,sentinel.size());d->UnmapBuffer(rb);return r;
 }
};
int main(){try{ray_geometry::SceneBVH scene;require(ray_geometry::LoadSceneBVH(".primal-cache/geometry-bvh/5d07eba5b7554607.tlas",6703585667342222855ull,scene),"original scene");auto cert=ray_geometry::CertifyCompactTraversal(scene);require(cert.valid&&cert.top_max_pending<=64&&cert.bottom_max_pending<=64,"original scene validated");
 for(const char* view:{"floor","atrium"}){std::string base="Docs/reports/primal-ddgi-query-kernel-20261030/tlas-screen/";std::ifstream in(base+view+".bin",std::ios::binary);require(bool(in),"recorded binary");std::vector<Query> q;Query v;while(in.read((char*)&v,sizeof(v)))q.push_back(v);require(in.eof()&&q.size()==4672,"exact query count");std::string gpath="Docs/reports/primal-ddgi-gpu-traversal-20261030/capture-reference-"+std::string(view)+"/queries.csv";std::ifstream gold(gpath);require(bool(gold),"recorded GPU CSV");std::string line;std::getline(gold,line);std::vector<std::array<int,5>> expected;while(std::getline(gold,line)){std::stringstream ss(line);std::string c;std::vector<std::string> fields;while(std::getline(ss,c,','))fields.push_back(c);require(fields.size()==18,"CSV format");const auto index=expected.size();require(index<q.size(),"CSV has no excess rows");require(q[index].group==u32(std::stoul(fields[0])) && q[index].probe==u32(std::stoul(fields[4])) && u32(std::stoul(fields[3]))==index%8 && q[index].group==index/8,"query IDs match GPU source");
  for(u32 k=0;k<6;++k){const float value=k<3?q[index].a[k]:q[index].b[k-3];require(std::bit_cast<u32>(value)==u32(std::stoul(fields[12+k],nullptr,16)),"query endpoint bits match GPU source");}
  require(std::stoi(fields[11])==0,"GPU source flags clear");int reason=std::stoi(fields[6]);require(reason==3||reason==4||reason==5,"known GPU result reason");require(std::stoi(fields[5])==(reason==4?1:0),"GPU result reason consistent");expected.push_back({reason==4?1:reason==5?-1:0,std::stoi(fields[7]),std::stoi(fields[8]),std::stoi(fields[9]),std::stoi(fields[10])});}require(expected.size()==q.size(),"expected rows");
  std::vector<Result> a,b;{GPU gpu;a=gpu.run(REPLAY_REFERENCE_SPV,scene,q);}{GPU gpu;b=gpu.run(REPLAY_CANDIDATE_SPV,scene,q);}u64 visits=0,loads=0,hits=0;for(size_t i=0;i<q.size();++i){auto x=a[i],y=b[i];require(x.status==expected[i][0]&&int(x.top)==expected[i][1]&&int(x.bottom)==expected[i][2]&&int(x.instances)==expected[i][3]&&int(x.triangles)==expected[i][4],"GPU reference mismatch recorded source");require(x.status==y.status&&x.top==y.top&&x.bottom==y.bottom&&x.instances==y.instances&&x.triangles==y.triangles,"candidate logical work or status differs");require(x.visits==x.instances && x.loads==x.visits && !x.hits && y.visits==y.instances && y.loads+y.hits==y.visits,"per-ray counters consistent");visits+=y.visits;loads+=y.loads;hits+=y.hits;}
  require(hits>0&&hits*100>=visits*15,"positive >=15 percent instance payload reuse");std::cout<<"[DDGI_INSTANCE_PAYLOAD_REPLAY] view="<<view<<" pixels="<<q.size()/8<<" queries="<<q.size()<<" instance_visits="<<visits<<" instance_loads="<<loads<<" instance_hits="<<hits<<" gpu_source_matches=1\n";
 }return 0;}catch(const std::exception& e){std::cerr<<"[DDGI_INSTANCE_PAYLOAD_REPLAY] FAIL "<<e.what()<<"\n";return 1;}}
