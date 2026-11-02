#pragma once
// Fixed-asset, opt-in CPU proof for the private Sponza field experiment.
// Does not certify device floating point or authorize arbitrary GPU writers.
#include "GeometryBVHStore.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <span>
#include <vector>
namespace primal::graphics::environment_hybrid {
inline constexpr u64 kFieldFNV=11017672305127041697ull;
inline constexpr u64 kFieldBytes=2646348ull;
inline constexpr u64 kSourceDigest=6703585667342222855ull;
inline constexpr const char* kFieldPath="Docs/reports/primal-environment-sdf-hybrid-20261102/leaf-field-v2.bin";
inline constexpr const char* kScenePath=".primal-cache/geometry-bvh/5d07eba5b7554607.tlas";
inline constexpr const char* kShaderPath="Docs/reports/primal-environment-sdf-hybrid-20261102/leaf-field-production.spv";
inline constexpr u64 kShaderFNV=11665097322767879862ull;
inline u64 Hash(std::span<const u8> data){u64 h=14695981039346656037ull;for(u8 v:data){h^=v;h*=1099511628211ull;}return h;}
inline bool ReadExact(const char* path,u64 bytes,std::vector<u8>& out){std::ifstream f(path,std::ios::binary|std::ios::ate);if(!f||f.tellg()!=std::streampos(bytes))return false;out.resize(bytes);f.seekg(0);return bool(f.read(reinterpret_cast<char*>(out.data()),bytes));}
template<class T> bool Same(const std::vector<T>& a,const std::vector<T>& b){return a.size()==b.size() && (a.empty()||std::memcmp(a.data(),b.data(),a.size()*sizeof(T))==0);}
inline bool SamePayload(const ray_geometry::SceneBVH& scene,const ray_geometry::SceneBVH& expected){return Same(scene.top,expected.top)&&Same(scene.bottom,expected.bottom)&&Same(scene.triangles,expected.triangles)&&Same(scene.instances,expected.instances);}
inline bool VerifyField(const ray_geometry::SceneBVH& scene,const std::vector<u8>& field,bool enforceHash=true){
 auto cert=ray_geometry::CertifyCompactTraversal(scene);if(!cert.valid||cert.top_max_pending>64||cert.bottom_max_pending>64)return false;
 if(field.size()!=kFieldBytes||(enforceHash && Hash(field)!=kFieldFNV)||field.size()%4)return false;
 std::vector<u32> words(field.size()/4);std::memcpy(words.data(),field.data(),field.size());const u64 len=words.size();
 if(len<32||words[0]!=0x464c4546u||words[1]!=1u||words[2]!=len||words[3]!=scene.bottom.size()||words[4]!=32||words[7]!=2)return false;
 const u64 leafBase=words[5],leafLen=words[6];if(leafBase>len||leafLen>len-leafBase||32ull+2ull*scene.bottom.size()>len)return false;
 for(u32 j=0;j<2;++j){const u32 m=8+j*12,root=words[m];if(root!=(j==0?10153u:9707u)||words[m+8]!=16||u64(words[m+1])+4096>len||root>=scene.bottom.size())return false;
  std::vector<u32> todo{root},seen(scene.bottom.size(),0);
  while(!todo.empty()){u32 at=todo.back();todo.pop_back();if(at>=scene.bottom.size()||seen[at]++)return false;const auto& node=scene.bottom[at];
   std::vector<u32> stack{at},leaves;while(!stack.empty()){u32 x=stack.back();stack.pop_back();if(x>=scene.bottom.size())return false;const auto& n=scene.bottom[x];if(n.links[3])leaves.push_back(x);else{stack.push_back(n.links[0]);stack.push_back(n.links[1]);}}
   const u32 start=words[32+2*at],count=words[33+2*at];if(start>leafLen||count>leafLen-start||count!=leaves.size())return false;std::sort(leaves.begin(),leaves.end());std::vector<u32> supplied(words.begin()+leafBase+start,words.begin()+leafBase+start+count);std::sort(supplied.begin(),supplied.end());if(leaves!=supplied)return false;
   if(!node.links[3]){todo.push_back(node.links[0]);todo.push_back(node.links[1]);}
  }
 }
 return true;
}
inline bool Verify(const ray_geometry::SceneBVH& scene,std::vector<u8>& field){ray_geometry::SceneBVH expected;if(!ray_geometry::LoadSceneBVH(kScenePath,kSourceDigest,expected)||!SamePayload(scene,expected))return false;if(!ReadExact(kFieldPath,kFieldBytes,field))return false;return VerifyField(scene,field);}
inline u64 NewProducerId(){static std::atomic<u64> serial{0};u64 old=serial.load();for(;;){if(old==~u64(0))return 0;if(serial.compare_exchange_weak(old,old+1))return old+1;}}
struct Binding {const void* producer=nullptr;u64 producer_id=0,epoch=0,digest=0,generation=0,completion=0;u32 slot=~u32(0);u64 handles[4]{},allocated[4]{},logical[4]{};bool ready=false;};
inline bool SameBinding(const Binding& a,const Binding& b){if(!a.ready||!b.ready||!a.producer||a.producer!=b.producer||!a.producer_id||a.producer_id!=b.producer_id||a.epoch!=b.epoch||!a.digest||a.digest!=b.digest||!a.generation||a.generation!=b.generation||!a.completion||a.completion!=b.completion||a.slot!=b.slot)return false;for(u32 i=0;i<4;++i)if(a.handles[i]!=b.handles[i]||a.allocated[i]!=b.allocated[i]||a.logical[i]!=b.logical[i]||!a.logical[i]||a.allocated[i]<a.logical[i])return false;return true;}
}
