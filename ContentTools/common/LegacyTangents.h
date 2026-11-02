#pragma once
// Legacy mesh tangent generation. Inputs are post-normal/UV vertices and
// triangle-corner indices; imported tangents, when present, are corner-aligned.
#include "../vendor/mikktspace/mikktspace.h"
#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
#include <algorithm>
#include <type_traits>

namespace primal::tools::geometry_detail {
struct TangentStats { size_t fallback_corners=0, rejected_faces=0; bool imported=false, import_rejected=false, mikk_failed=false; };
inline bool finite_bits(float f) { uint32_t u; std::memcpy(&u,&f,4); return (u & 0x7f800000u)!=0x7f800000u; }
struct Tangent4 { float x,y,z,w; };
inline uint8_t tangent_sign_bits(float z, float handedness) { return uint8_t((handedness < 0.f ? 1 : 0) | (z > 0.f ? 4 : 0)); }
template<class V> Tangent4 fallback_tangent(const V& v) {
    double x=v.normal.x,y=v.normal.y,z=v.normal.z;
    if (!finite_bits(v.normal.x)||!finite_bits(v.normal.y)||!finite_bits(v.normal.z)||x*x+y*y+z*z==0) { x=0;y=1;z=0; }
    double l=std::sqrt(x*x+y*y+z*z); x/=l;y/=l;z/=l;
    // Cross the normal with the least aligned axis for a stable tangent.
    double a=0,b=0,c=0;
    if (std::abs(x)<=std::abs(y)&&std::abs(x)<=std::abs(z)) { b=z;c=-y; }
    else if (std::abs(y)<=std::abs(z)) { a=-z;c=x; }
    else { a=y;b=-x; }
    l=std::sqrt(a*a+b*b+c*c); return {(float)(a/l),(float)(b/l),(float)(c/l),1.f};
}
template<class V> struct MikkData {
    const std::vector<V>* verts; const std::vector<uint32_t>* indices;
    std::vector<size_t> faces; std::vector<Tangent4>* corners;
    static MikkData& data(const SMikkTSpaceContext* c) { return *static_cast<MikkData*>(c->m_pUserData); }
    static int count(const SMikkTSpaceContext* c) { return (int)data(c).faces.size(); }
    static int triangle(const SMikkTSpaceContext*,int) { return 3; }
    static const V& vert(const SMikkTSpaceContext* c,int f,int v) { auto& d=data(c); return (*d.verts)[(*d.indices)[d.faces[f]*3+v]]; }
    static void pos(const SMikkTSpaceContext* c,float* o,int f,int i) { const auto& v=vert(c,f,i); o[0]=v.position.x;o[1]=v.position.y;o[2]=v.position.z; }
    static void normal(const SMikkTSpaceContext* c,float* o,int f,int i) { const auto& v=vert(c,f,i); o[0]=v.normal.x;o[1]=v.normal.y;o[2]=v.normal.z; }
    static void uv(const SMikkTSpaceContext* c,float* o,int f,int i) { const auto& v=vert(c,f,i); o[0]=v.uv.x;o[1]=v.uv.y; }
    static void set(const SMikkTSpaceContext* c,const float* t,float s,int f,int i) { auto& d=data(c); (*d.corners)[d.faces[f]*3+i]={t[0],t[1],t[2],s}; }
};
inline bool valid_tangent(const Tangent4& t) { return finite_bits(t.x)&&finite_bits(t.y)&&finite_bits(t.z)&&finite_bits(t.w)&& (double(t.x)*t.x+double(t.y)*t.y+double(t.z)*t.z)>0 && t.w!=0; }
// Returns false for malformed indices; no mutation occurs in that case.
template<class V, class I, class T> bool build_legacy_tangents(V& vertices,I& indices,const T& imported,bool calculate,bool has_uv,TangentStats& stats,bool imported_trusted=false) {
    if (indices.size()%3 || indices.size()/3>size_t(std::numeric_limits<int>::max())) return false;
    using VertexType = std::remove_cv_t<std::remove_reference_t<decltype(vertices[0])>>;
    std::vector<VertexType> old; old.reserve(vertices.size()); for(size_t k=0;k<vertices.size();++k) old.push_back(vertices[k]);
    std::vector<uint32_t> idx; idx.reserve(indices.size()); for(size_t k=0;k<indices.size();++k) idx.push_back(indices[k]);
    for (auto i:idx) if (i>=old.size()) return false;
    std::vector<Tangent4> corners(idx.size(),{0,0,0,0});
    if (!calculate && imported_trusted && imported.size()==idx.size()) {
        bool valid=true; for(size_t k=0;k<idx.size();++k) { const auto& t=imported[k]; const auto& n=old[idx[k]].normal; if (!valid_tangent({t.x,t.y,t.z,t.w}) || !finite_bits(n.x)||!finite_bits(n.y)||!finite_bits(n.z)) { valid=false;break; } double tl=double(t.x)*t.x+double(t.y)*t.y+double(t.z)*t.z, nl=double(n.x)*n.x+double(n.y)*n.y+double(n.z)*n.z, dot=double(t.x)*n.x+double(t.y)*n.y+double(t.z)*n.z; if(tl<0.998 || tl>1.002 || nl<0.998 || nl>1.002 || std::abs(dot)>0.001) {valid=false;break;} }
        if (valid) { for(size_t i=0;i<idx.size();++i) { const auto& t=imported[i]; corners[i]={t.x,t.y,t.z,t.w<0?-1.f:1.f}; } stats.imported=true; }
        else stats.import_rejected=true;
    } else if (!calculate) stats.import_rejected=true;
    if (!stats.imported && has_uv) {
        MikkData<VertexType> d{&old,&idx,{},&corners};
        for(size_t f=0;f<idx.size()/3;++f) {
            const auto& a=old[idx[3*f]]; const auto& b=old[idx[3*f+1]]; const auto& c=old[idx[3*f+2]];
            bool ok=true; for(int k=0;k<3;++k) {const auto& v=old[idx[3*f+k]]; ok &= finite_bits(v.position.x)&&finite_bits(v.position.y)&&finite_bits(v.position.z)&&finite_bits(v.normal.x)&&finite_bits(v.normal.y)&&finite_bits(v.normal.z)&&finite_bits(v.uv.x)&&finite_bits(v.uv.y); double n=double(v.normal.x)*v.normal.x+double(v.normal.y)*v.normal.y+double(v.normal.z)*v.normal.z; ok &= n>0; }
            if (ok) { double x1=double(b.position.x)-a.position.x,y1=double(b.position.y)-a.position.y,z1=double(b.position.z)-a.position.z,x2=double(c.position.x)-a.position.x,y2=double(c.position.y)-a.position.y,z2=double(c.position.z)-a.position.z; double x=y1*z2-z1*y2,y=z1*x2-x1*z2,z=x1*y2-y1*x2; double du1=double(b.uv.x)-a.uv.x,dv1=double(b.uv.y)-a.uv.y,du2=double(c.uv.x)-a.uv.x,dv2=double(c.uv.y)-a.uv.y; ok=(x*x+y*y+z*z)>0 && (du1*dv2-dv1*du2)!=0; }
            if(ok) d.faces.push_back(f); else ++stats.rejected_faces;
        }
        if(!d.faces.empty()) { SMikkTSpaceInterface api{}; api.m_getNumFaces=&decltype(d)::count; api.m_getNumVerticesOfFace=&decltype(d)::triangle; api.m_getPosition=&decltype(d)::pos; api.m_getNormal=&decltype(d)::normal; api.m_getTexCoord=&decltype(d)::uv; api.m_setTSpaceBasic=&decltype(d)::set; SMikkTSpaceContext ctx{&api,&d}; if(!genTangSpaceDefault(&ctx)) {stats.mikk_failed=true; std::fill(corners.begin(),corners.end(),Tangent4{0,0,0,0});} }
    }
    for(size_t i=0;i<idx.size();++i) if(!valid_tangent(corners[i])) {corners[i]=fallback_tangent(old[idx[i]]); ++stats.fallback_corners;}
    // Exact equality keeps every distinct Mikk corner direction and handedness.
    std::vector<std::vector<uint32_t>> groups(old.size()); V out; I outidx; outidx.resize(idx.size());
    for(size_t i=0;i<idx.size();++i) { auto& group=groups[idx[i]]; const auto t=corners[i]; uint32_t found=std::numeric_limits<uint32_t>::max(); for(auto j:group) {const auto& q=out[j].tangent; if(q.x==t.x&&q.y==t.y&&q.z==t.z&&q.w==t.w){found=j;break;}} if(found==std::numeric_limits<uint32_t>::max()) { if(out.size()>=std::numeric_limits<uint32_t>::max()) return false; found=(uint32_t)out.size(); out.emplace_back(old[idx[i]]); out.back().tangent={t.x,t.y,t.z,t.w}; group.push_back(found); } outidx[i]=found; }
    vertices.swap(out); indices.swap(outidx); return true;
}
}
