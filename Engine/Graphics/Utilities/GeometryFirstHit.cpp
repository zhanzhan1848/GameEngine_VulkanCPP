#include "GeometryFirstHit.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace primal::graphics::ray_geometry {
static bool finite(float f) { volatile u32 bits=std::bit_cast<u32>(f); return (bits&0x7f800000u)!=0x7f800000u; }
std::shared_ptr<const MeshBLAS> BuildMeshBLAS(const rhi::RHIMeshAsset& m) {
    if (!m.num_vertices || !m.num_indices || m.num_indices%3 ||
        (m.index_size!=2 && m.index_size!=4) ||
        m.position_buffer.size()!=u64(m.num_vertices)*12 ||
        m.index_buffer.size()!=u64(m.num_indices)*m.index_size) return {};
    primal::utl::vector<math::v3> positions; positions.resize(m.num_vertices);
    for(u32 i=0;i<m.num_vertices;++i) { float xyz[3]; std::memcpy(xyz,m.position_buffer.data()+u64(i)*12,12); positions[i]={xyz[0],xyz[1],xyz[2]}; }
    for (const auto& p:positions) if (!finite(p.x)||!finite(p.y)||!finite(p.z)) return {};
    primal::utl::vector<u32> indices; indices.resize(m.num_indices);
    for (u32 i=0;i<m.num_indices;++i) {
        u32 v=0; std::memcpy(&v,m.index_buffer.data()+u64(i)*m.index_size,m.index_size);
        if (v>=m.num_vertices) return {}; indices[i]=v;
    }
    utl::BVH bvh; bvh.Build(positions,indices);
    // Fixed GPU stacks are part of the contract, never silently truncate.
    if (bvh.GetStats().max_depth>=63) return {};
    auto result=std::make_shared<MeshBLAS>(); result->geometry_digest=rhi::ComputeGeometryDigest(m); bvh.Export(result->nodes,result->triangles);
    for (const auto& t:result->triangles) for (u32 k=0;k<3;++k)
        if (!finite(t.e1[k])||!finite(t.e2[k])) return {};
    return result;
}
bool SceneBVH::Build(std::span<const InstanceInput> inputs) {
    *this={}; if(inputs.empty()) return false;
    struct Entry { Node bounds; Instance instance; };
    std::vector<Entry> entries;
    std::unordered_map<const MeshBLAS*,u32> roots;
    for(const auto& in:inputs) {
        if(!in.mesh || in.mesh->nodes.empty() || in.mesh->triangles.empty()) { *this={}; return false; }
        double w[3][4];
        for(u32 r=0;r<3;++r) for(u32 c=0;c<4;++c) {
            const float* col=reinterpret_cast<const float*>(&in.world.columns[c]);
            if(!finite(col[r])) { *this={}; return false; } w[r][c]=col[r];
        }
        for(u32 c=0;c<4;++c) {
            const float* col=reinterpret_cast<const float*>(&in.world.columns[c]);
            if(!finite(col[3]) || col[3]!=(c==3?1.f:0.f)) { *this={}; return false; }
        }
        const double det=w[0][0]*(w[1][1]*w[2][2]-w[1][2]*w[2][1])-
            w[0][1]*(w[1][0]*w[2][2]-w[1][2]*w[2][0])+w[0][2]*(w[1][0]*w[2][1]-w[1][1]*w[2][0]);
        if(std::abs(det)<1e-20) { *this={}; return false; }
        Entry e{};
        for(u32 r=0;r<3;++r) {
            const u32 a=(r+1)%3,b=(r+2)%3;
            for(u32 c=0;c<3;++c) {
                const u32 d=(c+1)%3,f=(c+2)%3;
                e.instance.inverse_rows[r][c]=float((w[d][a]*w[f][b]-w[d][b]*w[f][a])/det);
            }
            double translation=0;
            for(u32 c=0;c<3;++c) translation-=e.instance.inverse_rows[r][c]*w[c][3];
            e.instance.inverse_rows[r][3]=float(translation);
            for(float v:e.instance.inverse_rows[r]) if(!finite(v)) { *this={}; return false; }
            double lo=w[r][3],hi=lo;
            const auto& bound=in.mesh->nodes[0];
            for(u32 c=0;c<3;++c) {
                double x=w[r][c]*bound.lo[c],y=w[r][c]*bound.hi[c];
                lo+=std::min(x,y); hi+=std::max(x,y);
            }
            e.bounds.lo[r]=std::nextafter(float(lo),-std::numeric_limits<float>::infinity());
            e.bounds.hi[r]=std::nextafter(float(hi),std::numeric_limits<float>::infinity());
            if(!finite(e.bounds.lo[r])||!finite(e.bounds.hi[r])) { *this={}; return false; }
        }
        auto [it,fresh]=roots.emplace(in.mesh.get(),u32(bottom.size()));
        if(fresh) {
            const u32 base=it->second,triBase=u32(triangles.size());
            for(auto node:in.mesh->nodes) {
                if(node.links[3]) node.links[2]+=triBase;
                else { node.links[0]+=base; node.links[1]+=base; }
                bottom.push_back(node);
            }
            for(auto t:in.mesh->triangles) triangles.push_back(t);
        }
        e.instance.root=it->second; e.instance.instance_id=in.instance_id;
        e.instance.pad[0]=u32(in.mesh->geometry_digest); e.instance.pad[1]=u32(in.mesh->geometry_digest>>32u); entries.push_back(e);
    }
    auto build=[&](auto&& self,u32 begin,u32 end)->u32 {
        u32 at=u32(top.size()); Node n{};
        for(u32 k=0;k<3;++k) {
            n.lo[k]=std::numeric_limits<float>::max(); n.hi[k]=-n.lo[k];
            for(u32 i=begin;i<end;++i) { n.lo[k]=std::min(n.lo[k],entries[i].bounds.lo[k]); n.hi[k]=std::max(n.hi[k],entries[i].bounds.hi[k]); }
        }
        top.push_back(n);
        if(end-begin==1) { top[at].links[2]=u32(instances.size()); top[at].links[3]=1; instances.push_back(entries[begin].instance); }
        else {
            u32 axis=0; for(u32 k=1;k<3;++k) if(n.hi[k]-n.lo[k]>n.hi[axis]-n.lo[axis]) axis=k;
            u32 mid=begin+(end-begin)/2;
            std::nth_element(entries.begin()+begin,entries.begin()+mid,entries.begin()+end,[&](const Entry& a,const Entry& b){
                return double(a.bounds.lo[axis])+a.bounds.hi[axis]<double(b.bounds.lo[axis])+b.bounds.hi[axis]; });
            u32 left=self(self,begin,mid),right=self(self,mid,end);
            top[at].links[0]=left; top[at].links[1]=right;
        } return at;
    }; build(build,0,u32(entries.size())); return true;
}
// R4: 参数名对齐头文件 tmin/tmax——Windows 侧 rpcndr.h 把 near/far 定义为
// 空宏（`near<0` → `<0` 等语法错级联）。
static bool box(const Node& n,const float* o,const float* d,float tmin,float tmax) {
    for(u32 k=0;k<3;++k) {
        if(d[k]==0.f) { if(o[k]<n.lo[k]||o[k]>n.hi[k]) return false; }
        else { float a=(n.lo[k]-o[k])/d[k],b=(n.hi[k]-o[k])/d[k]; tmin=std::max(tmin,std::min(a,b)); tmax=std::min(tmax,std::max(a,b)); if(tmin>tmax) return false; }
    } return true;
}
int Trace(const SceneBVH& scene,const float* o,const float* d,float tmin,float tmax,Hit& hit) {
    hit={}; hit.t=tmax;
    if(!finite(tmin)||!finite(tmax)||tmin<0||tmin>=tmax||scene.top.empty()) return -1;
    double norm=0; for(u32 k=0;k<3;++k) { if(!finite(o[k])||!finite(d[k])) return -1; norm+=double(d[k])*d[k]; }
    if(norm==0) return -1;
    u32 stack[64]{0},sp=1; bool found=false;
    while(sp) {
        const auto& n=scene.top[stack[--sp]]; if(!box(n,o,d,tmin,hit.t)) continue;
        if(!n.links[3]) { if(sp+2>64) return -1; stack[sp++]=n.links[0]; stack[sp++]=n.links[1]; continue; }
        const auto& inst=scene.instances[n.links[2]]; float localO[3],localD[3];
        for(u32 r=0;r<3;++r) { localO[r]=inst.inverse_rows[r][3]; localD[r]=0; for(u32 c=0;c<3;++c) { localO[r]+=inst.inverse_rows[r][c]*o[c]; localD[r]+=inst.inverse_rows[r][c]*d[c]; } }
        // Do not normalize localD: the ray parameter must remain world-space t.
        u32 bs[64]{inst.root},bp=1;
        while(bp) {
            const auto& b=scene.bottom[bs[--bp]]; if(!box(b,localO,localD,tmin,hit.t)) continue;
            if(!b.links[3]) { if(bp+2>64) return -1; bs[bp++]=b.links[0]; bs[bp++]=b.links[1]; continue; }
            for(u32 i=0;i<b.links[3];++i) {
                const auto& tri=scene.triangles[b.links[2]+i];
                double p[3],q[3],v[3];
                for(u32 k=0;k<3;++k) { u32 a=(k+1)%3,c=(k+2)%3; p[k]=double(localD[a])*tri.e2[c]-double(localD[c])*tri.e2[a]; v[k]=double(localO[k])-tri.a[k]; }
                double det=0; for(u32 k=0;k<3;++k) det+=tri.e1[k]*p[k]; if(det==0) continue;
                double u=0; for(u32 k=0;k<3;++k) u+=v[k]*p[k]; u/=det; if(u<0||u>1) continue;
                for(u32 k=0;k<3;++k) {u32 a=(k+1)%3,c=(k+2)%3;q[k]=v[a]*tri.e1[c]-v[c]*tri.e1[a];}
                double bary=0,t=0;for(u32 k=0;k<3;++k) {bary+=localD[k]*q[k];t+=tri.e2[k]*q[k];} bary/=det;t/=det;
                if(bary<0||u+bary>1||t<tmin||t>hit.t) continue;
                u32 id=std::bit_cast<u32>(tri.a[3]);
                if(found && float(t)==hit.t && (inst.instance_id>hit.instance_id || (inst.instance_id==hit.instance_id && id>=hit.triangle_id))) continue;
                hit={float(t),float(u),float(bary),inst.instance_id,id,u64(inst.pad[0])|(u64(inst.pad[1])<<32u)}; found=true;
            }
        }
    } return found?1:0;
}
}
