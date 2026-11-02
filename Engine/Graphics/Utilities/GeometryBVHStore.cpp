#include "GeometryBVHStore.h"
#include "Graphics/RHI/Core/CardSurfaceIdentity.h"
#include <atomic>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <cmath>
#include <cstring>

namespace primal::graphics::ray_geometry {
using rhi::card_surface_identity::Hash;
struct Header {
    char magic[8]{'P','B','V','H','0','0','0','1'};
    u32 endian{0x01020304},kind{};
    u64 source{},payload{};
    u32 counts[4]{};
    u64 reserved[2]{};
};
static_assert(sizeof(Header)==64);
static constexpr u64 MaxBytes=512ull*1024*1024;
static bool finite(float value) {volatile u32 bits=std::bit_cast<u32>(value);return (bits&0x7f800000u)!=0x7f800000u;}
static bool contains(const Node& parent,const Node& child) {
    for(u32 k=0;k<3;++k) if(parent.lo[k]>child.lo[k]||parent.hi[k]<child.hi[k])return false;
    return true;
}
// Detect out-of-range links, cycles, repeated nodes/leaves, unreachable payload,
// invalid bounds, and depths beyond the actual fixed traversal stack contract.
static bool validateTree(std::span<const Node> nodes,std::span<const u32> roots,u32 primitives) {
    if(nodes.empty()||roots.empty()||!primitives)return false;
    std::vector<u8> visited(nodes.size()),used(primitives);
    std::vector<std::pair<u32,u32>> stack;
    for(u32 root:roots)stack.emplace_back(root,0);
    while(!stack.empty()) {
        auto [at,depth]=stack.back();stack.pop_back();
        if(at>=nodes.size()||visited[at]||depth>=63)return false;visited[at]=1;
        const auto& n=nodes[at];
        for(u32 k=0;k<3;++k)if(!finite(n.lo[k])||!finite(n.hi[k])||n.lo[k]>n.hi[k])return false;
        if(n.links[3]) {
            const u32 first=n.links[2],count=n.links[3];
            if(first>primitives||count>primitives-first)return false;
            for(u32 i=first;i<first+count;++i){if(used[i])return false;used[i]=1;}
        } else {
            for(u32 k=0;k<2;++k){u32 child=n.links[k];if(child>=nodes.size()||!contains(n,nodes[child]))return false;stack.emplace_back(child,depth+1);}
        }
    }
    return std::all_of(visited.begin(),visited.end(),[](u8 v){return v!=0;})&&
        std::all_of(used.begin(),used.end(),[](u8 v){return v!=0;});
}
static bool validateTriangles(std::span<const Node> nodes,std::span<const Triangle> triangles) {
    for(const auto& tri:triangles) for(u32 k=0;k<3;++k)
        if(!finite(tri.a[k])||!finite(tri.e1[k])||!finite(tri.e2[k]))return false;
    for(const auto& n:nodes) if(n.links[3])for(u32 i=0;i<n.links[3];++i) {
        const auto& tri=triangles[n.links[2]+i];
        for(u32 k=0;k<3;++k)for(float p:{tri.a[k],tri.a[k]+tri.e1[k],tri.a[k]+tri.e2[k]})
            if(!finite(p)||p<n.lo[k]||p>n.hi[k])return false;
    }
    return true;
}
static bool validate(const SceneBVH& scene,bool mesh) {
    if(scene.bottom.empty()||scene.triangles.empty())return false;
    std::vector<u32> roots;
    if(mesh) {
        roots.push_back(0);std::vector<u8> ids(scene.triangles.size());
        for(const auto& t:scene.triangles){u32 id=std::bit_cast<u32>(t.a[3]);if(id>=ids.size()||ids[id])return false;ids[id]=1;}
    }
    else {
        if(scene.top.empty()||scene.instances.empty())return false;
        std::unordered_set<u32> distinct,ids;
        for(const auto& instance:scene.instances){
            if(!ids.insert(instance.instance_id).second||instance.instance_id>=0x1fffffffu)return false;
            for(const auto& row:instance.inverse_rows)for(float v:row)if(!finite(v))return false;
            if(distinct.insert(instance.root).second)roots.push_back(instance.root);
        }
        const u32 root=0;
        if(!validateTree(scene.top,std::span<const u32>(&root,1),u32(scene.instances.size())))return false;
    }
    return validateTree(scene.bottom,roots,u32(scene.triangles.size()))&&validateTriangles(scene.bottom,scene.triangles);
}
CompactTraversalCertificate CertifyCompactTraversal(const SceneBVH& scene) {
    CompactTraversalCertificate result{};
    if(!validate(scene,false)) return result;
    auto maximum=[](std::span<const Node> nodes,u32 root) {
        std::vector<u32> pending{root}; u32 peak=1;
        while(!pending.empty()) {
            const u32 at=pending.back(); pending.pop_back();
            const auto& n=nodes[at];
            if(!n.links[3]) {
                pending.push_back(n.links[0]); pending.push_back(n.links[1]);
                peak=std::max(peak,u32(pending.size()));
            }
        }
        return peak;
    };
    result.top_max_pending=maximum(scene.top,0);
    std::unordered_set<u32> roots;
    for(const auto& instance:scene.instances) if(roots.insert(instance.root).second)
        result.bottom_max_pending=std::max(result.bottom_max_pending,maximum(scene.bottom,instance.root));
    result.valid=true; return result;
}
InstanceNormalOwnerCertificate CertifyInstanceNormalOwners(const SceneBVH& scene,
    u64 maxBufferBytes,u64 totalBudgetBytes) {
    InstanceNormalOwnerCertificate result{};
    const auto count=scene.triangles.size();
    if(!count || count>UINT32_MAX || scene.instances.empty() || scene.instances.size()>UINT32_MAX ||
       scene.top.size()>UINT32_MAX || scene.bottom.size()>UINT32_MAX) {
        result.failure=InstanceNormalOwnerFailure::Size;return result;
    }
    // count is at most UINT32_MAX, so these products and the sum times 3
    // are representable in u64; check limits before allocating anything.
    const u64 ownerBytes=u64(count)*sizeof(u32), normalBytes=u64(count)*16;
    if(ownerBytes>maxBufferBytes || normalBytes>maxBufferBytes ||
       3*(ownerBytes+normalBytes)>totalBudgetBytes) {
        result.failure=InstanceNormalOwnerFailure::Budget;return result;
    }
    try {
        std::unordered_set<u32> roots;
        for(const auto& instance:scene.instances) if(!roots.insert(instance.root).second) {
            result.failure=InstanceNormalOwnerFailure::SharedRoot;return result;
        }
        const auto cert=CertifyCompactTraversal(scene);
        if(!cert.valid || cert.top_max_pending>64 || cert.bottom_max_pending>64) {
            result.failure=InstanceNormalOwnerFailure::InvalidScene;return result;
        }
        std::vector<u32> owners(count,UINT32_MAX),pending;
        for(u32 owner=0;owner<scene.instances.size();++owner) {
            pending.push_back(scene.instances[owner].root);
            while(!pending.empty()) {
                const u32 at=pending.back();pending.pop_back();
                if(at>=scene.bottom.size()) {result.failure=InstanceNormalOwnerFailure::InvalidScene;return result;}
                const auto& n=scene.bottom[at];
                if(n.links[3]) {
                    const u32 first=n.links[2],length=n.links[3];
                    if(first>count || length>count-first) {result.failure=InstanceNormalOwnerFailure::InvalidScene;return result;}
                    for(u32 j=0;j<length;++j) {
                        auto& value=owners[first+j];
                        if(value!=UINT32_MAX) {result.failure=InstanceNormalOwnerFailure::InvalidScene;return result;}
                        value=owner;
                    }
                } else {pending.push_back(n.links[0]);pending.push_back(n.links[1]);}
            }
        }
        if(std::find(owners.begin(),owners.end(),UINT32_MAX)!=owners.end()) {
            result.failure=InstanceNormalOwnerFailure::InvalidScene;return result;
        }
        u64 h=0xcbf29ce484222325ull;
        h=Hash(scene.top.data(),scene.top.size()*sizeof(Node),h);
        h=Hash(scene.bottom.data(),scene.bottom.size()*sizeof(Node),h);
        h=Hash(scene.triangles.data(),scene.triangles.size()*sizeof(Triangle),h);
        h=Hash(scene.instances.data(),scene.instances.size()*sizeof(Instance),h);
        result.source_payload_hash=h;
        result.owner_hash=Hash(owners.data(),ownerBytes,0xcbf29ce484222325ull);
        result.owner_bytes=ownerBytes;result.normal_bytes=normalBytes;
        result.owner=std::move(owners);result.failure=InstanceNormalOwnerFailure::None;
    } catch(const std::bad_alloc&) {result={};result.failure=InstanceNormalOwnerFailure::Allocation;}
    return result;
}
std::shared_ptr<const InstanceNormalOwnerCertificate> TryCertifyInstanceNormalOwners(
    const SceneBVH& scene,u64 maxBufferBytes,u64 totalBudgetBytes) {
    try { return std::make_shared<InstanceNormalOwnerCertificate>(CertifyInstanceNormalOwners(scene,maxBufferBytes,totalBudgetBytes)); }
    catch(const std::bad_alloc&) { return {}; }
}
static double nodeArea(const Node& n) {
    const double x=double(n.hi[0])-n.lo[0],y=double(n.hi[1])-n.lo[1],z=double(n.hi[2])-n.lo[2];
    return 2.0*(x*y+y*z+z*x);
}
static u64 scenePayloadHash(const SceneBVH& s) {
    u64 h=0xcbf29ce484222325ull;
    h=Hash(s.top.data(),s.top.size()*sizeof(Node),h);
    h=Hash(s.bottom.data(),s.bottom.size()*sizeof(Node),h);
    h=Hash(s.triangles.data(),s.triangles.size()*sizeof(Triangle),h);
    return Hash(s.instances.data(),s.instances.size()*sizeof(Instance),h);
}
bool EqualBoxSidecarIntact(const EqualBoxSidecar& sidecar) {
    return sidecar.valid && sidecar.payload_hash && sidecar.flags_hash &&
        sidecar.flags.size()==u64(sidecar.top_count)+sidecar.bottom_count &&
        Hash(sidecar.flags.data(),sidecar.flags.size()*sizeof(u32),0xcbf29ce484222325ull)==sidecar.flags_hash;
}
std::shared_ptr<const EqualBoxSidecar> TryBuildEqualBoxSidecar(const SceneBVH& scene,u64 maxBytes) {
    try {
        const auto cert=CertifyCompactTraversal(scene);
        if(!cert.valid || cert.top_max_pending>64 || cert.bottom_max_pending>64 ||
           scene.top.size()>UINT32_MAX || scene.bottom.size()>UINT32_MAX ||
           scene.top.size()+scene.bottom.size()>UINT32_MAX ||
           (scene.top.size()+scene.bottom.size())*sizeof(u32)>maxBytes) return {};
        auto result=std::make_shared<EqualBoxSidecar>();
        result->top_count=u32(scene.top.size());result->bottom_count=u32(scene.bottom.size());
        result->flags.resize(scene.top.size()+scene.bottom.size());
        auto fill=[&](const auto& nodes,size_t offset) {
            for(size_t i=0;i<nodes.size();++i) {
                const auto& parent=nodes[i];if(parent.links[3])continue;
                for(u32 child=0;child<2;++child) {
                    const u32 index=parent.links[child];if(index>=nodes.size())return false;
                    const auto& n=nodes[index];bool equal=true;
                    for(u32 k=0;k<3;++k) {
                        const u32 values[]={std::bit_cast<u32>(parent.lo[k]),std::bit_cast<u32>(parent.hi[k]),std::bit_cast<u32>(n.lo[k]),std::bit_cast<u32>(n.hi[k])};
                        for(u32 bits:values)if((bits&0x7f800000u)==0x7f800000u)return false;
                        equal &= values[0]==values[2] && values[1]==values[3];
                    }
                    if(equal)result->flags[offset+i]|=1u<<child;
                }
            }return true;
        };
        if(!fill(scene.top,0)||!fill(scene.bottom,scene.top.size()))return {};
        result->payload_hash=scenePayloadHash(scene);
        result->flags_hash=Hash(result->flags.data(),result->flags.size()*sizeof(u32),0xcbf29ce484222325ull);
        result->valid=result->payload_hash && result->flags_hash;
        return result->valid ? result : std::shared_ptr<const EqualBoxSidecar>{};
    } catch(const std::bad_alloc&) {return {};}
}
bool BuildUpperSAH16(const SceneBVH& source,SceneBVH& out,UpperBVHStats& stats) {
    const auto start=std::chrono::steady_clock::now(); stats={};out={};
    auto finish=[&](bool ok) {stats.build_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); if(!ok)out={}; return ok;};
    const auto originalCert=CertifyCompactTraversal(source);
    if(!originalCert.valid || originalCert.top_max_pending>64 || originalCert.bottom_max_pending>64) return finish(false);
    stats.original_max_pending=std::max(originalCert.top_max_pending,originalCert.bottom_max_pending);
    stats.original_payload_hash=scenePayloadHash(source);
    SceneBVH candidate=source;
    auto maxDepth=[](std::span<const Node> nodes,u32 root) {
        std::vector<std::pair<u32,u32>> todo{{root,0}}; u32 maximum=0;
        while(!todo.empty()){auto [i,d]=todo.back();todo.pop_back();maximum=std::max(maximum,d);
            if(!nodes[i].links[3]){todo.emplace_back(nodes[i].links[0],d+1);todo.emplace_back(nodes[i].links[1],d+1);}}
        return maximum;
    };
    stats.original_max_depth=maxDepth(source.top,0);
    std::unordered_set<u32> roots;
    for(const auto& inst:source.instances) roots.insert(inst.root);
    for(u32 root:roots) {
        stats.original_max_depth=std::max(stats.original_max_depth,maxDepth(source.bottom,root));
        std::vector<u32> pending{root},leaves,internal;
        while(!pending.empty()) {const u32 i=pending.back();pending.pop_back();const auto& n=source.bottom[i];
            if(n.links[3])leaves.push_back(i);else {internal.push_back(i);pending.push_back(n.links[0]);pending.push_back(n.links[1]);}}
        if(leaves.size()==1)continue;
        if(internal.size()+1!=leaves.size() || internal.empty() || internal[0]!=root)return finish(false);
        double oldCost=0; for(u32 i:internal)oldCost+=nodeArea(source.bottom[i]);
        std::vector<u32> pool(internal.begin()+1,internal.end());size_t cursor=0;
        auto rec=[&](auto&& self,size_t first,size_t last,u32 node,u32 depth)->bool {
            if(depth>=63 || last-first<2)return false;
            double lo[3]={INFINITY,INFINITY,INFINITY},hi[3]={-INFINITY,-INFINITY,-INFINITY};
            for(size_t i=first;i<last;++i)for(u32 k=0;k<3;++k){const auto& n=source.bottom[leaves[i]];const double c=(double(n.lo[k])+double(n.hi[k]))*.5;lo[k]=std::min(lo[k],c);hi[k]=std::max(hi[k],c);}
            double best=INFINITY;u32 bestAxis=0,bestSplit=0; bool found=false;
            for(u32 axis=0;axis<3;++axis){const double extent=hi[axis]-lo[axis];if(!(extent>0) || !std::isfinite(extent))continue;
                struct Bin {double lo[3]={INFINITY,INFINITY,INFINITY},hi[3]={-INFINITY,-INFINITY,-INFINITY};u32 n=0;}; Bin bins[16]{};
                auto binOf=[&](u32 id){const auto& n=source.bottom[id];double c=(double(n.lo[axis])+n.hi[axis])*.5;return std::min(15u,u32((c-lo[axis])/extent*16.0));};
                for(size_t i=first;i<last;++i){auto& b=bins[binOf(leaves[i])];const auto& n=source.bottom[leaves[i]];++b.n;for(u32 k=0;k<3;++k){b.lo[k]=std::min(b.lo[k],double(n.lo[k]));b.hi[k]=std::max(b.hi[k],double(n.hi[k]));}}
                auto merge=[](Bin a,const Bin& b){if(!b.n)return a; for(u32 k=0;k<3;++k){a.lo[k]=std::min(a.lo[k],b.lo[k]);a.hi[k]=std::max(a.hi[k],b.hi[k]);}a.n+=b.n;return a;};
                auto area=[](const Bin& b){double x=b.hi[0]-b.lo[0],y=b.hi[1]-b.lo[1],z=b.hi[2]-b.lo[2];return 2*(x*y+y*z+z*x);};
                Bin prefix[16],suffix[16],acc{};for(u32 j=0;j<16;++j){acc=merge(acc,bins[j]);prefix[j]=acc;}acc=Bin{};for(int j=15;j>=0;--j){acc=merge(acc,bins[j]);suffix[j]=acc;}
                for(u32 j=0;j<15;++j)if(prefix[j].n&&suffix[j+1].n){const double cost=area(prefix[j])*prefix[j].n+area(suffix[j+1])*suffix[j+1].n;if(std::isfinite(cost)&&cost<best){best=cost;bestAxis=axis;bestSplit=j;found=true;}}
            }
            size_t middle;
            if(!found) {
                // Only exactly coincident, finite centroids get the stable
                // fallback; other failed binning retains the original root.
                for(u32 k=0;k<3;++k) {
                    const double extent=hi[k]-lo[k];
                    if(!std::isfinite(extent) || extent!=0.0)return false;
                }
                std::sort(leaves.begin()+first,leaves.begin()+last);
                middle=first+(last-first)/2;
            } else {
                const double range=hi[bestAxis]-lo[bestAxis];
                auto mid=std::partition(leaves.begin()+first,leaves.begin()+last,[&](u32 id){const auto& n=source.bottom[id];double c=(double(n.lo[bestAxis])+n.hi[bestAxis])*.5;return std::min(15u,u32((c-lo[bestAxis])/range*16.0))<=bestSplit;});
                middle=size_t(mid-leaves.begin());
            }
            if(middle==first||middle==last)return false;
            auto child=[&](size_t begin,size_t end,u32& id){if(end-begin==1){id=leaves[begin];return true;}if(cursor>=pool.size())return false;id=pool[cursor++];return self(self,begin,end,id,depth+1);};
            u32 left,right;if(!child(first,middle,left)||!child(middle,last,right))return false;
            auto& n=candidate.bottom[node]; const auto& a=candidate.bottom[left];const auto& b=candidate.bottom[right];
            for(u32 k=0;k<3;++k){n.lo[k]=std::min(a.lo[k],b.lo[k]);n.hi[k]=std::max(a.hi[k],b.hi[k]);}
            n.links[0]=left;n.links[1]=right;return true;
        };
        const bool rebuilt=rec(rec,0,leaves.size(),root,0) && cursor==pool.size();
        double newCost=0;bool changed=false;for(u32 i:internal){newCost+=nodeArea(candidate.bottom[i]);changed |= std::memcmp(&source.bottom[i],&candidate.bottom[i],sizeof(Node))!=0;}
        if(!rebuilt || !changed || !(newCost<oldCost)) {
            for(u32 i:internal)candidate.bottom[i]=source.bottom[i];
            newCost=oldCost;changed=false;
        }
        stats.original_cost+=oldCost;stats.candidate_cost+=newCost;if(changed)++stats.roots_changed;
    }
    if(!stats.roots_changed || !(stats.candidate_cost<stats.original_cost))return finish(false);
    const auto newCert=CertifyCompactTraversal(candidate);
    if(!newCert.valid || newCert.top_max_pending>64 || newCert.bottom_max_pending>64)return finish(false);
    if(source.top.size()!=candidate.top.size() || source.bottom.size()!=candidate.bottom.size() ||
       source.triangles.size()!=candidate.triangles.size() || source.instances.size()!=candidate.instances.size() ||
       std::memcmp(source.top.data(),candidate.top.data(),source.top.size()*sizeof(Node))!=0 ||
       std::memcmp(source.triangles.data(),candidate.triangles.data(),source.triangles.size()*sizeof(Triangle))!=0 ||
       std::memcmp(source.instances.data(),candidate.instances.data(),source.instances.size()*sizeof(Instance))!=0) return finish(false);
    for(size_t i=0;i<source.bottom.size();++i) {
        const bool oldLeaf=source.bottom[i].links[3]!=0, newLeaf=candidate.bottom[i].links[3]!=0;
        if(oldLeaf!=newLeaf || (oldLeaf && std::memcmp(&source.bottom[i],&candidate.bottom[i],sizeof(Node))!=0))return finish(false);
    }
    stats.candidate_max_pending=std::max(newCert.top_max_pending,newCert.bottom_max_pending);
    stats.candidate_max_depth=maxDepth(candidate.top,0);
    for(u32 root:roots)stats.candidate_max_depth=std::max(stats.candidate_max_depth,maxDepth(candidate.bottom,root));
    stats.candidate_payload_hash=scenePayloadHash(candidate);
    stats.extra_gpu_bytes=candidate.bottom.size()*sizeof(Node);
    out=std::move(candidate);return finish(true);
}
static bool write(const std::filesystem::path& path,const SceneBVH& scene,u64 digest,u32 kind) {
    if(std::endian::native!=std::endian::little||!digest||!validate(scene,kind==1))return false;
    Header h;h.kind=kind;h.source=digest;
    h.counts[0]=u32(scene.top.size());h.counts[1]=u32(scene.bottom.size());
    h.counts[2]=u32(scene.triangles.size());h.counts[3]=u32(scene.instances.size());
    const void* data[]={scene.top.data(),scene.bottom.data(),scene.triangles.data(),scene.instances.data()};
    const u64 sizes[]={scene.top.size()*48,scene.bottom.size()*48,scene.triangles.size()*48,scene.instances.size()*64};
    u64 total=64;h.payload=0xcbf29ce484222325ull;
    for(u32 i=0;i<4;++i){total+=sizes[i];h.payload=Hash(data[i],sizes[i],h.payload);}if(total>MaxBytes)return false;
    std::error_code ec;if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path(),ec);if(ec)return false;
    static std::atomic<u64> serial{0};auto temp=path;
    temp += ".tmp."+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"."+std::to_string(serial++);
    std::ofstream file(temp,std::ios::binary|std::ios::trunc);if(!file)return false;
    file.write(reinterpret_cast<const char*>(&h),64);
    for(u32 i=0;i<4;++i)if(sizes[i])file.write(static_cast<const char*>(data[i]),sizes[i]);
    file.flush();bool ok=bool(file);file.close();ok&=!file.fail();
    if(ok)std::filesystem::rename(temp,path,ec);if(!ok||ec){std::filesystem::remove(temp,ec);return false;}return true;
}
static bool read(const std::filesystem::path& path,u64 digest,u32 kind,SceneBVH& out) {
    out={};if(std::endian::native!=std::endian::little||!digest)return false;
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)return false;
    const auto length=file.tellg();if(length<64||length>std::streamoff(MaxBytes))return false;file.seekg(0);
    Header h;file.read(reinterpret_cast<char*>(&h),64);
    if(!file||std::memcmp(h.magic,"PBVH0001",8)||h.endian!=0x01020304||h.kind!=kind||h.source!=digest||h.reserved[0]||h.reserved[1])return false;
    const u64 sizes[]={u64(h.counts[0])*48,u64(h.counts[1])*48,u64(h.counts[2])*48,u64(h.counts[3])*64};
    u64 total=64;for(u64 size:sizes)total+=size;if(total!=u64(length))return false;
    if(kind==1 && (h.counts[0]||h.counts[3]))return false;
    SceneBVH s;s.top.resize(h.counts[0]);s.bottom.resize(h.counts[1]);s.triangles.resize(h.counts[2]);s.instances.resize(h.counts[3]);
    void* data[]={s.top.data(),s.bottom.data(),s.triangles.data(),s.instances.data()};u64 hash=0xcbf29ce484222325ull;
    for(u32 i=0;i<4;++i){if(sizes[i])file.read(static_cast<char*>(data[i]),sizes[i]);hash=Hash(data[i],sizes[i],hash);}
    if(!file||hash!=h.payload||!validate(s,kind==1))return false;out=std::move(s);return true;
}
std::filesystem::path GeometryCacheDirectory() {
    const char* directory=std::getenv("PRIMAL_SC_GEOMETRY_CACHE_DIR");
    return directory && directory[0]?std::filesystem::path(directory):std::filesystem::path(".primal-cache/geometry-bvh");
}
static std::filesystem::path name(const std::filesystem::path& dir,u64 digest,const char* extension){
    std::ostringstream text;text<<std::hex<<std::setw(16)<<std::setfill('0')<<digest<<extension;return dir/text.str();
}
bool SaveMeshBLAS(const std::filesystem::path& path,const MeshBLAS& mesh){
    SceneBVH scene;for(auto node:mesh.nodes)scene.bottom.push_back(node);for(auto triangle:mesh.triangles)scene.triangles.push_back(triangle);
    return write(path,scene,mesh.geometry_digest,1);
}
std::shared_ptr<const MeshBLAS> LoadMeshBLAS(const std::filesystem::path& path,u64 digest){
    SceneBVH scene;if(!read(path,digest,1,scene))return {};
    auto mesh=std::make_shared<MeshBLAS>();mesh->geometry_digest=digest;
    std::vector<u8> ids(scene.triangles.size());
    for(const auto& t:scene.triangles){u32 id=std::bit_cast<u32>(t.a[3]);if(id>=ids.size()||ids[id])return {};ids[id]=1;mesh->triangles.push_back(t);}
    for(auto n:scene.bottom)mesh->nodes.push_back(n);return mesh;
}
std::shared_ptr<const MeshBLAS> LoadOrBuildMeshBLAS(const rhi::RHIMeshAsset& source,const std::filesystem::path& dir,bool* loaded,bool* persisted){
    if(loaded)*loaded=false;if(persisted)*persisted=false;const u64 digest=rhi::ComputeGeometryDigest(source);auto path=name(dir,digest,".blas");
    if(auto mesh=LoadMeshBLAS(path,digest)){if(loaded)*loaded=true;if(persisted)*persisted=true;return mesh;}
    auto mesh=BuildMeshBLAS(source);if(mesh){bool saved=SaveMeshBLAS(path,*mesh);if(persisted)*persisted=saved;}return mesh;
}
u64 SceneSourceDigest(std::span<const InstanceInput> inputs){
    if(inputs.empty())return 0;u64 count=inputs.size(),digest=Hash(&count,8);
    for(const auto& input:inputs){if(!input.mesh||!input.mesh->geometry_digest)return 0;
        digest=Hash(&input.mesh->geometry_digest,8,digest);digest=Hash(&input.instance_id,4,digest);
        digest=Hash(&input.world,sizeof(input.world),digest);}
    return digest;
}
bool SaveSceneBVH(const std::filesystem::path& path,const SceneBVH& scene,u64 digest){return write(path,scene,digest,2);}
bool LoadSceneBVH(const std::filesystem::path& path,u64 digest,SceneBVH& scene){return read(path,digest,2,scene);}
bool LoadOrBuildSceneBVH(std::span<const InstanceInput> inputs,const std::filesystem::path& dir,SceneBVH& scene,bool* loaded,bool* persisted){
    if(loaded)*loaded=false;if(persisted)*persisted=false;u64 digest=SceneSourceDigest(inputs);auto path=name(dir,digest,".tlas");
    if(digest && LoadSceneBVH(path,digest,scene)){if(loaded)*loaded=true;if(persisted)*persisted=true;return true;}
    if(!scene.Build(inputs))return false;if(digest){bool saved=SaveSceneBVH(path,scene,digest);if(persisted)*persisted=saved;}return true;
}
}
