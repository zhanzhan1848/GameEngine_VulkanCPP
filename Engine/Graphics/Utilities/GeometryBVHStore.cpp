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
