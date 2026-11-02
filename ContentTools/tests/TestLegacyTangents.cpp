#include "../common/LegacyTangents.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
using namespace primal::tools::geometry_detail;
struct V2 {float x,y;}; struct V3 {float x,y,z;}; struct V4 {float x,y,z,w;};
struct Vertex {V3 position,normal; V2 uv; V4 tangent; int marker;};
using Vertices=std::vector<Vertex>; using Indices=std::vector<uint32_t>; using Tangents=std::vector<V4>;
static Vertex vertex(float x,float y,float z,float u,float v,V3 n={0,0,1}) {return {{x,y,z},n,{u,v},{},42};}
static bool close(float a,float b){return std::abs(a-b)<1e-5f;}
static void verify_finite(const Vertices& v) {for(auto& x:v) {assert(finite_bits(x.tangent.x)&&finite_bits(x.tangent.y)&&finite_bits(x.tangent.z)&&finite_bits(x.tangent.w)); assert(close(x.tangent.x*x.tangent.x+x.tangent.y*x.tangent.y+x.tangent.z*x.tangent.z,1));assert(x.marker==42);}}
int main(){
 {Vertices v={vertex(0,0,0,0,0),vertex(1,0,0,1,0),vertex(0,1,0,0,1)}; Indices i={0,1,2}; TangentStats s; assert(build_legacy_tangents(v,i,Tangents{},true,true,s));verify_finite(v); assert(s.fallback_corners==0);for(auto& x:v)assert(close(x.tangent.x,1)&&close(x.tangent.y,0)&&x.tangent.w>0);}
 {Vertices v={vertex(0,0,0,0,0),vertex(1,0,0,1,0),vertex(0,1,0,0,1),vertex(0,-1,0,0,1)}; Indices i={0,1,2,0,3,1}; TangentStats s; assert(build_legacy_tangents(v,i,Tangents{},true,true,s)); assert(i[0]!=i[3]&&i[1]!=i[5]); assert(v[i[0]].tangent.w==-v[i[3]].tangent.w);verify_finite(v);}
 for(float sign:{-1.f,1.f}) {float q=std::sqrt(.5f); Vertices v={vertex(0,0,0,0,0,{-sign*q,0,q}),vertex(q,0,sign*q,1,0,{-sign*q,0,q}),vertex(0,1,0,0,1,{-sign*q,0,q})}; Indices i={0,1,2}; TangentStats s; assert(build_legacy_tangents(v,i,Tangents{},true,true,s)); verify_finite(v); assert(v[0].tangent.z*sign>.7f); unsigned packed=tangent_sign_bits(v[0].tangent.z,v[0].tangent.w); assert(((packed&4)!=0)==(sign>0)); assert(((packed&1)!=0)==(v[0].tangent.w<0)); }
 {Vertices v={vertex(0,0,0,0,0),vertex(1,0,0,0,0),vertex(0,1,0,0,0)}; Indices i={0,1,2}; TangentStats s; assert(build_legacy_tangents(v,i,Tangents{},true,true,s)); assert(s.rejected_faces==1&&s.fallback_corners==3);verify_finite(v);}
 {Vertices v={vertex(0,0,0,0,0),vertex(1,0,0,1,0),vertex(0,0,0,0,1)}; uint32_t infbits=0x7f800000u; std::memcpy(&v[1].uv.x,&infbits,4); Indices i={0,1,2}; TangentStats s; assert(build_legacy_tangents(v,i,Tangents{},true,true,s)); assert(s.rejected_faces==1&&s.fallback_corners==3); verify_finite(v);}
 {Vertices v={vertex(0,0,0,0,0),vertex(1,0,0,1,0),vertex(0,1,0,0,1)}; Indices i={0,1,2}; TangentStats s; assert(build_legacy_tangents(v,i,Tangents{},true,false,s)); assert(s.fallback_corners==3);verify_finite(v);}
 {Vertices v={vertex(0,0,0,0,0),vertex(1,0,0,1,0),vertex(0,1,0,0,1)}; Indices i={0,1,2,0,2,1}; Tangents t={{0,1,0,-1},{0,1,0,-1},{0,1,0,-1},{1,0,0,1},{1,0,0,1},{1,0,0,1}}; TangentStats s; assert(build_legacy_tangents(v,i,t,false,true,s,true));assert(s.imported&&!s.import_rejected);assert(i[0]!=i[3]&&v[i[0]].tangent.w==-1&&v[i[3]].tangent.x==1);verify_finite(v);}
 {Vertices v={vertex(0,0,0,0,0),vertex(1,0,0,1,0),vertex(0,1,0,0,1)}; Indices i={0,1,2}; TangentStats s; assert(build_legacy_tangents(v,i,Tangents{{0,0,0,0}},false,true,s)); assert(s.import_rejected&&!s.imported&&s.fallback_corners==0);}
 {Vertices v={vertex(0,0,0,0,0),vertex(1,0,0,1,0),vertex(0,1,0,0,1)}; uint32_t nanbits=0x7fc00000u; std::memcpy(&v[0].normal.x,&nanbits,4); Indices i={0,1,2}; TangentStats s; assert(build_legacy_tangents(v,i,Tangents{},true,true,s)); assert(s.rejected_faces==1 && s.fallback_corners==3); verify_finite(v);}
 std::puts("Legacy tangent tests passed");
}
