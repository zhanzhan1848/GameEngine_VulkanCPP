#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>
// Explicit experimental main-raster legacy qualification only. Never mutates source assets or SC.
namespace qualified_normal_signs {
struct Vec {float x,y,z;};
struct XY {uint16_t x,y;};
struct Key {std::array<uint32_t,3> v;bool operator==(const Key&)const=default;};
struct Hash {size_t operator()(const Key& k)const {size_t h=1469598103934665603ull;for(auto v:k.v){h^=v;h*=1099511628211ull;}return h;}};
struct Result {bool qualified=false;uint32_t maxXYDelta=0;size_t mismatch=0,positive=0;std::vector<uint8_t> signs;};
inline Result Recover(std::span<const Vec> p,std::span<const XY> xy,std::span<const uint32_t> index,std::span<const uint8_t> sourceSigns){
 Result r;if(p.empty()||xy.size()!=p.size()||sourceSigns.size()!=p.size()||index.empty()||index.size()%3)return r;
 // Explicit legacy candidate only: never reinterpret a mesh carrying any authored sign bits.
 if(std::any_of(sourceSigns.begin(),sourceSigns.end(),[](auto v){return v!=0;}))return r;
 for(auto a:p)if(!std::isfinite(a.x)||!std::isfinite(a.y)||!std::isfinite(a.z))return r;
 for(auto i:index)if(i>=p.size())return r;
 std::vector<Vec> faces(index.size()/3),acc(p.size(),Vec{});
 for(size_t t=0;t<faces.size();++t){auto a=p[index[3*t]],b=p[index[3*t+1]],c=p[index[3*t+2]];
  Vec e{b.x-a.x,b.y-a.y,b.z-a.z},f{c.x-a.x,c.y-a.y,c.z-a.z};Vec n{e.y*f.z-e.z*f.y,e.z*f.x-e.x*f.z,e.x*f.y-e.y*f.x};
  float l=n.x*n.x+n.y*n.y+n.z*n.z;if(!std::isfinite(l))return r;
  if(l>1e-5f){float len=std::sqrt(l);n={n.x/len,n.y/len,n.z/len};}else n={};faces[t]=n;
 }
 // Fixed float32 order, then exact-position weld restricted to this mesh.
 for(size_t corner=0;corner<3;++corner)for(size_t t=0;t<faces.size();++t){auto& a=acc[index[t*3+corner]];auto b=faces[t];a.x+=b.x;a.y+=b.y;a.z+=b.z;}
 std::unordered_map<Key,size_t,Hash> groups;std::vector<size_t> ids(p.size());std::vector<Vec> welded;
 for(size_t i=0;i<p.size();++i){auto a=p[i];Key k{{std::bit_cast<uint32_t>(a.x),std::bit_cast<uint32_t>(a.y),std::bit_cast<uint32_t>(a.z)}};auto [it,newGroup]=groups.emplace(k,welded.size());if(newGroup)welded.push_back({});ids[i]=it->second;auto& w=welded[it->second];w.x+=acc[i].x;w.y+=acc[i].y;w.z+=acc[i].z;}
 r.signs.resize(p.size());
 for(size_t i=0;i<p.size();++i){auto n=welded[ids[i]];float l=n.x*n.x+n.y*n.y+n.z*n.z;if(!std::isfinite(l)){r.signs.clear();return r;}if(l>1e-5f){float len=std::sqrt(l);n={n.x/len,n.y/len,n.z/len};}else n={0,1,0};
  auto pack=[](float v){return int(std::floor((std::clamp(v,-1.f,1.f)+1.f)*.5f*65535.f+.5f));};
  auto delta=uint32_t(std::max(std::abs(pack(n.x)-int(xy[i].x)),std::abs(pack(n.y)-int(xy[i].y))));r.maxXYDelta=std::max(r.maxXYDelta,delta);r.mismatch+=delta>1;r.signs[i]=n.z>0?2:0;r.positive+=n.z>0;
 }
 // All-or-nothing; tolerance is packed-code qualification, not an opaque/ray numerical gate.
 r.qualified=r.mismatch==0;if(!r.qualified){r.signs.clear();r.positive=0;}return r;
}
}
