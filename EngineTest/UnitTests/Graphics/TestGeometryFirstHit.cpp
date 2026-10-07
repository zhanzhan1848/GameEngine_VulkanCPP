#include "Graphics/Utilities/GeometryFirstHit.h"
#include "Graphics/Utilities/GeometryBVHStore.h"
#include <fstream>
#include <chrono>
#include <iostream>
#include <bit>
using namespace primal;
using namespace primal::graphics;
int main() {
    int tests=0,fail=0;
    auto check=[&](bool ok,const char* name){++tests;if(!ok){++fail;std::cerr<<"FAIL "<<name<<'\n';}};
    rhi::RHIMeshAsset mesh{};mesh.num_vertices=3;mesh.num_indices=3;mesh.index_size=4;
    float v[][3]={{-2,-2,0},{2,-2,0},{0,2,0}};u32 indices[]={0,1,2};
    mesh.position_buffer.resize(sizeof(v));std::memcpy(mesh.position_buffer.data(),v,sizeof(v));
    mesh.index_buffer.resize(sizeof(indices));std::memcpy(mesh.index_buffer.data(),indices,sizeof(indices));
    auto blas=ray_geometry::BuildMeshBLAS(mesh);check(bool(blas),"valid BLAS");
    auto matrix=[](float x,float y,float z){math::m4x4 m{};m.columns[0]={x,0,0,0};m.columns[1]={0,y,0,0};m.columns[2]={0,0,z,0};m.columns[3]={0,0,0,1};return m;};
    auto world=matrix(1,1,1),nearWorld=matrix(-2,3,.25);world.columns[3].z=5;nearWorld.columns[3].z=2;
    std::vector<ray_geometry::InstanceInput> in{{blas,world,10},{blas,nearWorld,22}};
    ray_geometry::SceneBVH scene;check(scene.Build(in),"scene build");
    check(scene.triangles.size()==1 && scene.bottom.size()==blas->nodes.size(),"shared BLAS");
    float o[]={0,0,0},d[]={0,0,1};ray_geometry::Hit hit;
    check(ray_geometry::Trace(scene,o,d,.05,50,hit)==1 && hit.instance_id==22 && std::abs(hit.t-2)<1e-6 && hit.triangle_id==0,"nearest transformed instance");
    check(ray_geometry::Trace(scene,o,d,3,50,hit)==1 && hit.instance_id==10 && std::abs(hit.t-5)<1e-6,"segment clipping");
    check(ray_geometry::Trace(scene,o,d,.05,1,hit)==0,"proven segment miss");
    float boundary[]={-2,-2,0};check(ray_geometry::Trace(scene,boundary,d,.05,50,hit)==1,"axis-parallel boundary ray");
    check(hit.geometry_digest==blas->geometry_digest,"hit retains SC geometry identity");
    rhi::RHICardBakeData bake{};rhi::ComputeCardBakeDigests(mesh);
    check(mesh.card_bake.geometry_digest==blas->geometry_digest,"BVH and SC share geometry digest");
    auto dir=std::filesystem::temp_directory_path()/ ("primal-bvh-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto blasFile=dir/"mesh.blas",sceneFile=dir/"scene.tlas";
    check(ray_geometry::SaveMeshBLAS(blasFile,*blas),"BLAS saved");
    auto restored=ray_geometry::LoadMeshBLAS(blasFile,blas->geometry_digest);
    check(restored && restored->triangles.size()==1 && std::memcmp(restored->triangles.data(),blas->triangles.data(),48)==0,"BLAS wire roundtrip");
    check(!ray_geometry::LoadMeshBLAS(blasFile,blas->geometry_digest+1),"wrong mesh identity rejected");
    auto corrupt=[&](const std::filesystem::path& path,size_t offset){std::fstream f(path,std::ios::binary|std::ios::in|std::ios::out);f.seekg(offset);char byte=0;f.read(&byte,1);byte^=1;f.seekp(offset);f.write(&byte,1);};
    corrupt(blasFile,80);check(!ray_geometry::LoadMeshBLAS(blasFile,blas->geometry_digest),"BLAS payload corruption rejected");
    check(ray_geometry::SaveMeshBLAS(blasFile,*blas),"atomic replacement repairs corruption");
    corrupt(blasFile,7);check(!ray_geometry::LoadMeshBLAS(blasFile,blas->geometry_digest),"unknown file version rejected");
    ray_geometry::SaveMeshBLAS(blasFile,*blas);std::filesystem::resize_file(blasFile,70);
    check(!ray_geometry::LoadMeshBLAS(blasFile,blas->geometry_digest),"truncation rejected");
    bool loaded=false;
    auto cached=ray_geometry::LoadOrBuildMeshBLAS(mesh,dir,&loaded);check(cached&&!loaded,"cold mesh cache builds");
    cached=ray_geometry::LoadOrBuildMeshBLAS(mesh,dir,&loaded);check(cached&&loaded,"warm mesh cache loads");
    auto materialGeometry=mesh;materialGeometry.element_buffer.resize(4);materialGeometry.element_buffer[0]=1;
    auto modified=ray_geometry::LoadOrBuildMeshBLAS(materialGeometry,dir,&loaded);
    check(modified&&!loaded&&modified->geometry_digest!=blas->geometry_digest,"normal/UV source changes invalidate shared SC/BVH identity");
    bool persisted=true;auto memoryOnly=ray_geometry::LoadOrBuildMeshBLAS(mesh,"/dev/null/unwritable",nullptr,&persisted);
    check(memoryOnly&&!persisted,"write failure keeps valid geometry and reports not persisted");
    u64 source=ray_geometry::SceneSourceDigest(in);
    check(ray_geometry::SaveSceneBVH(sceneFile,scene,source),"two-level scene saved");
    ray_geometry::SceneBVH diskScene;
    check(ray_geometry::LoadSceneBVH(sceneFile,source,diskScene) && diskScene.triangles.size()==1 && diskScene.instances.size()==2,"two-level scene roundtrip preserves sharing");
    check(ray_geometry::Trace(diskScene,o,d,.05,50,hit)==1 && hit.instance_id==22 && hit.geometry_digest==blas->geometry_digest,"loaded two-level nearest and identity");
    check(ray_geometry::LoadOrBuildSceneBVH(in,dir,diskScene,&loaded)&&!loaded,"cold scene cache builds");
    check(ray_geometry::LoadOrBuildSceneBVH(in,dir,diskScene,&loaded)&&loaded,"warm scene cache loads");
    auto changed=in;changed[0].world.columns[3].z=6;
    check(ray_geometry::SceneSourceDigest(changed)!=source && !ray_geometry::LoadSceneBVH(sceneFile,ray_geometry::SceneSourceDigest(changed),diskScene),"transform invalidates stored TLAS");
    changed=in;changed[0].instance_id=11;
    check(ray_geometry::SceneSourceDigest(changed)!=source,"instance reassignment invalidates TLAS");
    auto invalid=*blas;invalid.nodes[0].links[2]=99;
    check(!ray_geometry::SaveMeshBLAS(dir/"invalid.blas",invalid),"bad leaf range rejected");
    invalid=*blas;invalid.triangles[0].a[3]=std::bit_cast<float>(99u);
    check(!ray_geometry::SaveMeshBLAS(dir/"bad-triangle.blas",invalid),"invalid original triangle identity rejected");
    invalid=*blas;invalid.nodes[0].links[3]=0;invalid.nodes[0].links[0]=0;invalid.nodes[0].links[1]=0;
    check(!ray_geometry::SaveMeshBLAS(dir/"cycle.blas",invalid),"cycle rejected");
    std::filesystem::remove_all(dir);
    // Rotate local Z onto world X, add shear and non-uniform scale.
    auto rotated=matrix(1,1,1);rotated.columns[0]={0,0,-2,0};rotated.columns[1]={.3f,3,0,0};rotated.columns[2]={.25f,0,0,0};rotated.columns[3]={4,0,0,1};
    in={{blas,rotated,7}};check(scene.Build(in),"rotated sheared scene");
    float dx[]={1,0,0};check(ray_geometry::Trace(scene,o,dx,.05,50,hit)==1 && std::abs(hit.t-4)<1e-6,"inverse ray preserves t");
    auto singular=matrix(0,1,1);in.push_back({blas,singular,8});
    check(!scene.Build(in) && scene.top.empty(),"singular scene fails closed");
    in={{blas,world,1},{nullptr,world,2}};check(!scene.Build(in)&&scene.top.empty(),"missing mesh cannot certify sky");
    indices[2]=3;std::memcpy(mesh.index_buffer.data(),indices,sizeof(indices));check(!ray_geometry::BuildMeshBLAS(mesh),"bad index rejected");
    indices[2]=2;std::memcpy(mesh.index_buffer.data(),indices,sizeof(indices));v[0][0]=std::bit_cast<float>(0x7fc00000u);std::memcpy(mesh.position_buffer.data(),v,sizeof(v));check(!ray_geometry::BuildMeshBLAS(mesh),"nonfinite mesh rejected under fast math");
    check(scene.Build(std::span<const ray_geometry::InstanceInput>(&in[0],1)),"valid scene restores");
    float nanRay[]={std::bit_cast<float>(0x7fc00000u),0,1};check(ray_geometry::Trace(scene,o,nanRay,.05,50,hit)==-1,"invalid ray unknown");
    std::cout<<tests-fail<<'/'<<tests<<" checks passed\n";return fail?1:0;
}
