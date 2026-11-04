#include "Graphics/Utilities/GeometryFirstHit.h"
#include "Graphics/Utilities/GeometryBVHStore.h"
#include "Graphics/RHI/Core/FrameSlotCompletion.h"
#include "Graphics/Utilities/ExperimentalSAHMesh.h"
#include "Graphics/RenderPipeline/Modules/InstanceNormalCachePolicy.h"
#include <fstream>
#include <chrono>
#include <iostream>
#include <bit>
using namespace primal;
using namespace primal::graphics;
int main() {
    int tests=0,fail=0;
    auto check=[&](bool ok,const char* name){++tests;if(!ok){++fail;std::cerr<<"FAIL "<<name<<'\n';}};
    check(!InstanceNormalCacheQueryReady(false,false),"cache request without query opt-in stays on original pipeline");
    check(!InstanceNormalCacheQueryReady(false,true),"created query pipeline alone cannot opt in cache");
    check(!InstanceNormalCacheQueryReady(true,false),"query shader/pipeline failure disables cache and setter re-enable");
    check(InstanceNormalCacheQueryReady(true,true),"explicit ready query permits cache admission");
    rhi::RHIMeshAsset mesh{};mesh.num_vertices=3;mesh.num_indices=3;mesh.index_size=4;
    float v[][3]={{-2,-2,0},{2,-2,0},{0,2,0}};u32 indices[]={0,1,2};
    mesh.position_buffer.resize(sizeof(v));std::memcpy(mesh.position_buffer.data(),v,sizeof(v));
    mesh.index_buffer.resize(sizeof(indices));std::memcpy(mesh.index_buffer.data(),indices,sizeof(indices));
    auto blas=ray_geometry::BuildMeshBLAS(mesh);check(bool(blas),"valid BLAS");
    auto sah=ray_geometry::BuildExperimentalSAHMesh(*blas);
    check(sah && sah->geometry_digest==blas->geometry_digest && sah->triangles.size()==1 &&
          std::memcmp(sah->triangles.data(),blas->triangles.data(),sizeof(ray_geometry::Triangle))==0,
          "experimental SAH preserves triangle payload and digest");
    auto badExperimental=*blas; badExperimental.nodes[0].links[2]=99;
    check(!ray_geometry::BuildExperimentalSAHMesh(badExperimental),"experimental SAH rejects invalid leaf");
    auto matrix=[](float x,float y,float z){math::m4x4 m{};m.columns[0]={x,0,0,0};m.columns[1]={0,y,0,0};m.columns[2]={0,0,z,0};m.columns[3]={0,0,0,1};return m;};
    auto world=matrix(1,1,1),nearWorld=matrix(-2,3,.25);world.columns[3].z=5;nearWorld.columns[3].z=2;
    std::vector<ray_geometry::InstanceInput> in{{blas,world,10},{blas,nearWorld,22}};
    ray_geometry::SceneBVH scene;check(scene.Build(in),"scene build");
    check(scene.triangles.size()==1 && scene.bottom.size()==blas->nodes.size(),"shared BLAS");
    {
        // A skewed full binary tree whose pending bound is exactly leaf count:
        // x is a leaf and y is the recursively visited child.
        auto appendComb=[](std::vector<ray_geometry::Node>& nodes, ray_geometry::Node bounds,
                           u32 count,u32 first, auto&& self)->u32 {
            const u32 root=u32(nodes.size()); bounds.links[0]=bounds.links[1]=0;
            bounds.links[2]=first; bounds.links[3]=count==1?1:0; nodes.push_back(bounds);
            if(count>1) {
                const u32 left=self(nodes,bounds,1,first,self);
                const u32 right=self(nodes,bounds,count-1,first+1,self);
                nodes[root].links[0]=left; nodes[root].links[1]=right;
            }
            return root;
        };
        auto make=[&](u32 topLeaves,u32 bottomLeaves) {
            ray_geometry::SceneBVH x;
            appendComb(x.top,scene.top[0],topLeaves,0,appendComb);
            appendComb(x.bottom,scene.bottom[0],bottomLeaves,0,appendComb);
            for(u32 k=0;k<topLeaves;++k) {auto inst=scene.instances[0];inst.root=0;inst.instance_id=k;x.instances.push_back(inst);}
            for(u32 k=0;k<bottomLeaves;++k)x.triangles.push_back(scene.triangles[0]);
            return x;
        };
        auto equalScene=make(2,2);
        auto equal=ray_geometry::TryBuildEqualBoxSidecar(equalScene,1u<<20);
        check(equal && equal->valid && equal->flags[0]==3 && equal->flags[equal->top_count]==3 && equal->payload_hash && equal->flags_hash,
              "equal-box certificate preserves both child bits for TLAS and BLAS");
        check(!ray_geometry::TryBuildEqualBoxSidecar(equalScene,4),"equal-box certificate rejects insufficient storage budget");
        if(equal){auto damaged=*equal;damaged.flags[0]^=1;check(!ray_geometry::EqualBoxSidecarIntact(damaged),"equal-box upload rejects CPU-tampered flags");
            damaged=*equal;damaged.flags.pop_back();check(!ray_geometry::EqualBoxSidecarIntact(damaged),"equal-box upload rejects truncated flags");}
        const u64 sourceHandles[4]={10,11,12,13},sourceSizes[4]={32,48,64,80};
        auto binding=[&](u64 epoch,u64 digest,u32 slot,u64 side,u64 bytes,const u64* handles,const u64* sizes){
            return ray_geometry::EqualBoxBindingMatches(4,9,1,17,23,31,44,sourceHandles,sourceSizes,epoch,digest,slot,17,23,side,bytes,handles,sizes);};
        check(binding(4,9,1,31,44,sourceHandles,sourceSizes),"equal-box slot identity valid");
        check(!binding(5,9,1,31,44,sourceHandles,sourceSizes) && !binding(4,9,2,31,44,sourceHandles,sourceSizes) &&
              !binding(4,9,1,32,44,sourceHandles,sourceSizes) && !binding(4,9,1,31,40,sourceHandles,sourceSizes),"equal-box rejects epoch slot private handle and length changes");
        const u64 changedHandles[4]={10,77,12,13},changedSizes[4]={32,48,64,81};
        check(!binding(4,9,1,31,44,changedHandles,sourceSizes) && !binding(4,9,1,31,44,sourceHandles,changedSizes),"equal-box rejects source changes");
        rhi::FrameSlotCompletion completion;
        check(!completion.CanReuse(1,0),"equal-box upload rejects unknown completion");
        check(completion.Publish(1) && completion.CanReuse(1,0),"equal-box upload permits new completed slot");
        const auto consumed=completion.Serial(1);
        check(!completion.CanReuse(1,consumed),"equal-box upload cannot reuse same completion serial");
        completion.Invalidate();check(!completion.CanReuse(1,0),"equal-box upload rejects invalidated completion");
        check(completion.Publish(2) && !completion.CanReuse(1,consumed),"equal-box upload rejects other completed slot");
        check(completion.Publish(1) && completion.CanReuse(1,consumed),"equal-box upload accepts new matching completion");
        auto signedZero=equalScene;
        for(auto& node:signedZero.top){node.lo[0]=-0.0f;node.hi[0]=1.0f;}
        signedZero.top[signedZero.top[0].links[0]].lo[0]=0.0f;
        auto zeros=ray_geometry::TryBuildEqualBoxSidecar(signedZero,1u<<20);
        check(zeros && !(zeros->flags[0]&1) && (zeros->flags[0]&2),"equal-box certificate distinguishes signed zero");
        auto invalidEqual=equalScene;invalidEqual.top[0].links[0]=u32(invalidEqual.top.size());
        check(!ray_geometry::TryBuildEqualBoxSidecar(invalidEqual,1u<<20),"equal-box certificate rejects invalid source");
        auto mismatchEqual=equalScene;mismatchEqual.bottom[mismatchEqual.bottom[0].links[0]].hi[0]-=.001f;
        check(!ray_geometry::TryBuildEqualBoxSidecar(mismatchEqual,1u<<20),"equal-box certificate rejects undersized triangle leaf");
        auto fits=make(16,32); auto c=ray_geometry::CertifyCompactTraversal(fits);
        check(c.Fits() && c.top_max_pending==16 && c.bottom_max_pending==32,"compact exact stack capacity");
        auto topOver=make(17,1); c=ray_geometry::CertifyCompactTraversal(topOver);
        check(c.valid && !c.Fits() && c.top_max_pending==17,"compact TLAS over capacity");
        auto bottomOver=make(1,33); c=ray_geometry::CertifyCompactTraversal(bottomOver);
        check(c.valid && !c.Fits() && c.bottom_max_pending==33,"compact BLAS over capacity");
        auto roots=make(2,3); const u32 other=appendComb(roots.bottom,scene.bottom[0],4,3,appendComb);
        for(int k=0;k<4;++k)roots.triangles.push_back(scene.triangles[0]); roots.instances[1].root=other;
        c=ray_geometry::CertifyCompactTraversal(roots);
        check(c.Fits() && c.top_max_pending==2 && c.bottom_max_pending==4,"compact independent BLAS roots");
        using OF=ray_geometry::InstanceNormalOwnerFailure;
        constexpr u64 ownerLimit=1ull<<24;
        auto owned=ray_geometry::CertifyInstanceNormalOwners(roots,ownerLimit,ownerLimit);
        check(owned.Valid() && owned.owner.size()==7 && owned.owner_bytes==28 && owned.normal_bytes==112 &&
              owned.owner[0]==0 && owned.owner[2]==0 && owned.owner[3]==1 && owned.owner[6]==1 &&
              owned.source_payload_hash && owned.owner_hash,"owner certificate maps actual multi-root instance indices");
        auto reversed=roots;std::swap(reversed.instances[0],reversed.instances[1]);
        auto swapped=ray_geometry::CertifyInstanceNormalOwners(reversed,ownerLimit,ownerLimit);
        check(swapped.Valid() && swapped.owner[0]==1 && swapped.owner[6]==0 &&
              swapped.source_payload_hash!=owned.source_payload_hash && swapped.owner_hash!=owned.owner_hash,
              "owner indices follow actual reordered instance buffer and identity");
        auto shared=roots;shared.instances[1].root=shared.instances[0].root;
        auto r=ray_geometry::CertifyInstanceNormalOwners(shared,ownerLimit,ownerLimit);
        check(r.failure==OF::SharedRoot && r.owner.empty(),"shared root rejected and publishes no map");
        r=ray_geometry::CertifyInstanceNormalOwners(roots,111,ownerLimit);
        check(r.failure==OF::Budget && r.owner.empty(),"per-buffer capacity rejects oversized normals");
        r=ray_geometry::CertifyInstanceNormalOwners(roots,ownerLimit,419);
        check(r.failure==OF::Budget && r.owner.empty(),"three-slot total budget rejects allocation");
        auto broken=roots;broken.bottom[0].links[0]=0;
        r=ray_geometry::CertifyInstanceNormalOwners(broken,ownerLimit,ownerLimit);
        check(r.failure==OF::InvalidScene && r.owner.empty(),"cycle rejected without published map");
        broken=roots;broken.bottom.back().links[2]=0;
        r=ray_geometry::CertifyInstanceNormalOwners(broken,ownerLimit,ownerLimit);
        check(r.failure==OF::InvalidScene && r.owner.empty(),"overlap and hole rejected without published map");
        broken=roots;broken.bottom.back().links[2]=u32(broken.triangles.size());
        r=ray_geometry::CertifyInstanceNormalOwners(broken,ownerLimit,ownerLimit);
        check(r.failure==OF::InvalidScene && r.owner.empty(),"out-of-range triangle rejected without published map");
        broken=roots;broken.triangles.push_back(broken.triangles[0]);
        r=ray_geometry::CertifyInstanceNormalOwners(broken,ownerLimit,ownerLimit);
        check(r.failure==OF::InvalidScene && r.owner.empty(),"unreferenced triangle rejected without published map");
        auto single=make(1,1);r=ray_geometry::CertifyInstanceNormalOwners(single,ownerLimit,ownerLimit);
        check(r.Valid() && r.owner.size()==1 && r.owner[0]==0,"single-instance owner map");

        const u64 handles[4]={11,12,13,14}, sizes[4]={64,128,192,256};
        const auto cert=ray_geometry::CertifyCompactTraversal(fits);
        auto match=[&](u64 epoch,u64 digest,u32 slot,const u64* h,const u64* n) {
            return ray_geometry::CompactBindingMatches(cert,7,99,1,handles,sizes,epoch,digest,slot,h,n);
        };
        check(match(7,99,1,handles,sizes),"compact slot binding valid");
        check(!match(8,99,1,handles,sizes),"compact same digest scene rebuild stale");
        check(!match(7,99,0,handles,sizes) && !match(7,99,2,handles,sizes),"compact triple slot mismatch");
        check(!match(7,100,1,handles,sizes),"compact digest mismatch");
        u64 altered[4]={11,12,33,14};check(!match(7,99,1,altered,sizes),"compact changed handle mismatch");
        u64 resized[4]={64,128,191,256};check(!match(7,99,1,handles,resized),"compact changed size mismatch");
        auto absent=cert; absent.valid=false;
        check(!ray_geometry::CompactBindingMatches(absent,7,99,1,handles,sizes,7,99,1,handles,sizes),"compact failed upload has no certificate");
        auto upperSource=make(1,8);
        const float offsets[8]={0,100,1,101,2,102,3,103};
        for(auto& n:upperSource.bottom) if(n.links[3]) {
            const auto j=n.links[2]; const float dx=offsets[j];
            n.lo[0]+=dx;n.hi[0]+=dx;upperSource.triangles[j].a[0]+=dx;
        }
        auto rebind=[&](auto&& self,u32 id)->void {auto& n=upperSource.bottom[id]; if(n.links[3])return;
            self(self,n.links[0]);self(self,n.links[1]); const auto& a=upperSource.bottom[n.links[0]];const auto& b=upperSource.bottom[n.links[1]];
            for(u32 k=0;k<3;++k){n.lo[k]=std::min(a.lo[k],b.lo[k]);n.hi[k]=std::max(a.hi[k],b.hi[k]);}};
        rebind(rebind,0);
        ray_geometry::SceneBVH upper;ray_geometry::UpperBVHStats us{};
        const bool built=ray_geometry::BuildUpperSAH16(upperSource,upper,us);
        bool same=built && upper.bottom.size()==upperSource.bottom.size() && upper.instances[0].root==upperSource.instances[0].root &&
            std::memcmp(upper.top.data(),upperSource.top.data(),upper.top.size()*sizeof(ray_geometry::Node))==0 &&
            std::memcmp(upper.triangles.data(),upperSource.triangles.data(),upper.triangles.size()*sizeof(ray_geometry::Triangle))==0 &&
            std::memcmp(upper.instances.data(),upperSource.instances.data(),upper.instances.size()*sizeof(ray_geometry::Instance))==0;
        for(size_t k=0;k<upperSource.bottom.size() && same;++k)if(upperSource.bottom[k].links[3])
            same=std::memcmp(&upper.bottom[k],&upperSource.bottom[k],sizeof(ray_geometry::Node))==0;
        check(same && us.roots_changed>0 && us.candidate_cost<us.original_cost && us.original_payload_hash!=us.candidate_payload_hash,
            "upper SAH preserves root leaves and payload with lower cost");
        const auto oldOwners=ray_geometry::CertifyInstanceNormalOwners(upperSource,ownerLimit,ownerLimit);
        const auto upperOwners=ray_geometry::CertifyInstanceNormalOwners(upper,ownerLimit,ownerLimit);
        check(oldOwners.Valid() && upperOwners.Valid() && oldOwners.owner==upperOwners.owner &&
              oldOwners.owner_hash==upperOwners.owner_hash && oldOwners.source_payload_hash!=upperOwners.source_payload_hash,
              "upper retains complete triangle ownership while topology witness changes");
        ray_geometry::SceneBVH rejected;ray_geometry::UpperBVHStats badStats{};
        auto noSplit=make(1,8);check(!ray_geometry::BuildUpperSAH16(noSplit,rejected,badStats) && rejected.bottom.empty(),"upper degenerate fallback");
        check(!ray_geometry::BuildUpperSAH16(noSplit,rejected,badStats) && rejected.bottom.empty(),"identical bounds cannot fake improvement repeatedly");
        auto coincident=make(1,8);
        const float scales[8]={1,2,3,4,5,6,7,8};
        for(auto& n:coincident.bottom) if(n.links[3]) {
            float scale=scales[n.links[2]];
            n.lo[0]=-2*scale;n.hi[0]=2*scale;n.lo[1]=-2*scale;n.hi[1]=2*scale;
        }
        auto fitParents=[](ray_geometry::SceneBVH& x) {
            auto rec=[&](auto&& self,u32 id)->void {auto& n=x.bottom[id];if(n.links[3])return;self(self,n.links[0]);self(self,n.links[1]);
                const auto& a=x.bottom[n.links[0]];const auto& b=x.bottom[n.links[1]];
                for(u32 k=0;k<3;++k){n.lo[k]=std::min(a.lo[k],b.lo[k]);n.hi[k]=std::max(a.hi[k],b.hi[k]);}};rec(rec,0);
        };
        fitParents(coincident); ray_geometry::SceneBVH c1,c2;ray_geometry::UpperBVHStats t1{},t2{};
        const bool cOk=ray_geometry::BuildUpperSAH16(coincident,c1,t1), cOk2=ray_geometry::BuildUpperSAH16(coincident,c2,t2);
        check(cOk && cOk2 && t1.candidate_cost<t1.original_cost && c1.bottom.size()==c2.bottom.size() &&
            std::memcmp(c1.bottom.data(),c2.bottom.data(),c1.bottom.size()*sizeof(ray_geometry::Node))==0,
            "coincident varying bounds obey cost and deterministic result");
        auto mixed=make(1,8); const float dxs[8]={0,100,0,101,0,102,0,103};
        for(auto& n:mixed.bottom)if(n.links[3]) {u32 j=n.links[2];n.lo[0]+=dxs[j];n.hi[0]+=dxs[j];mixed.triangles[j].a[0]+=dxs[j];}
        fitParents(mixed);ray_geometry::SceneBVH m1,m2;ray_geometry::UpperBVHStats mstats{},mstats2{};
        const bool mixedOk=ray_geometry::BuildUpperSAH16(mixed,m1,mstats), mixedOk2=ray_geometry::BuildUpperSAH16(mixed,m2,mstats2);
        bool mixedPreserved=mixedOk&&mixedOk2&&mstats.candidate_cost<mstats.original_cost && m1.bottom.size()==mixed.bottom.size() &&
            std::memcmp(m1.bottom.data(),m2.bottom.data(),m1.bottom.size()*sizeof(ray_geometry::Node))==0 &&
            std::memcmp(m1.top.data(),mixed.top.data(),mixed.top.size()*sizeof(ray_geometry::Node))==0 &&
            std::memcmp(m1.triangles.data(),mixed.triangles.data(),mixed.triangles.size()*sizeof(ray_geometry::Triangle))==0 &&
            std::memcmp(m1.instances.data(),mixed.instances.data(),mixed.instances.size()*sizeof(ray_geometry::Instance))==0;
        for(size_t k=0;k<mixed.bottom.size() && mixedPreserved;++k)if(mixed.bottom[k].links[3])
            mixedPreserved=std::memcmp(&m1.bottom[k],&mixed.bottom[k],sizeof(ray_geometry::Node))==0;
        check(mixedPreserved,"upper SAH improves with coincident child leaves and preserves payload");
        auto summedArea=[](const ray_geometry::SceneBVH& x) {double sum=0;for(const auto& n:x.bottom)if(!n.links[3]) {
            double a=double(n.hi[0])-n.lo[0],b=double(n.hi[1])-n.lo[1],c=double(n.hi[2])-n.lo[2];sum+=2*(a*b+b*c+c*a);}return sum;};
        check(mixedOk && std::abs(summedArea(mixed)-mstats.original_cost)<1e-9 &&
            std::abs(summedArea(m1)-mstats.candidate_cost)<1e-9,"upper cost matches independent internal area sum");
        auto notFinite=mixed;notFinite.bottom[0].lo[0]=std::bit_cast<float>(0x7f800000u);
        check(!ray_geometry::BuildUpperSAH16(notFinite,rejected,badStats) && rejected.bottom.empty(),"upper rejects nonfinite bound");
        auto malformed=upperSource;malformed.bottom[0].links[0]=u32(malformed.bottom.size());
        check(!ray_geometry::BuildUpperSAH16(malformed,rejected,badStats) && rejected.bottom.empty(),"upper invalid topology fallback");
        auto tooDeep=make(1,64);check(!ray_geometry::BuildUpperSAH16(tooDeep,rejected,badStats) && rejected.bottom.empty(),"upper rejects excessive depth");
        auto upperMatches=[&](u64 epoch,u64 digest,u32 slot,u64 orig,u64 cand,u64 privateHandle,u64 privateSize,const u64* h,const u64* n){
            return ray_geometry::UpperBindingMatches(7,99,1,101,102,42,512,handles,sizes,epoch,digest,slot,orig,cand,privateHandle,privateSize,h,n);};
        check(upperMatches(7,99,1,101,102,42,512,handles,sizes),"upper slot identity valid");
        check(!upperMatches(8,99,1,101,102,42,512,handles,sizes),"upper same digest epoch stale");
        check(!upperMatches(7,99,1,103,102,42,512,handles,sizes),"upper same digest different source payload");
        check(!upperMatches(7,99,1,101,103,42,512,handles,sizes),"upper candidate payload changed");
        check(!upperMatches(7,99,0,101,102,42,512,handles,sizes) && !upperMatches(7,99,2,101,102,42,512,handles,sizes),"upper different frame slots");
        check(!upperMatches(7,99,1,101,102,43,512,handles,sizes) && !upperMatches(7,99,1,101,102,42,513,handles,sizes),"upper private handle and size changed");
        check(!upperMatches(7,99,1,101,102,42,512,altered,sizes) && !upperMatches(7,99,1,101,102,42,512,handles,resized),"upper original handles and sizes changed");
        auto cyclic=fits;cyclic.top[0].links[1]=0;
        check(!ray_geometry::CertifyCompactTraversal(cyclic).valid,"compact rejects cycle");
        auto invalid=fits;invalid.bottom[0].links[0]=u32(invalid.bottom.size());
        check(!ray_geometry::CertifyCompactTraversal(invalid).valid,"compact rejects invalid child");
    }

    float o[]={0,0,0},d[]={0,0,1};ray_geometry::Hit hit;
    check(ray_geometry::Trace(scene,o,d,.05,50,hit)==1 && hit.instance_id==22 && std::abs(hit.t-2)<1e-6 && hit.triangle_id==0,"nearest transformed instance");
    auto sahInputs=in; for (auto& entry:sahInputs) entry.mesh=sah;
    ray_geometry::SceneBVH sahScene;
    check(sahScene.Build(sahInputs) && ray_geometry::Trace(sahScene,o,d,.05,50,hit)==1 &&
          hit.instance_id==22 && std::abs(hit.t-2)<1e-6 && hit.triangle_id==0,
          "experimental SAH transformed trace");
    ray_geometry::SceneBVH builtSAHScene;
    check(ray_geometry::BuildExperimentalSAHScene(in,builtSAHScene) &&
          builtSAHScene.triangles.size()==1 && builtSAHScene.instances.size()==2 &&
          ray_geometry::Trace(builtSAHScene,o,d,.05,50,hit)==1 && hit.instance_id==22,
          "experimental scene preserves shared mesh and transformed instance");
    auto incompleteSAH=in; incompleteSAH.push_back({nullptr,world,99});
    check(!ray_geometry::BuildExperimentalSAHScene(incompleteSAH,builtSAHScene) &&
          builtSAHScene.top.empty() && builtSAHScene.bottom.empty(),
          "experimental scene rejects incomplete geometry");
    {
        rhi::RHIMeshAsset grid{}; std::vector<float> points; std::vector<u32> ids;
        for (u32 row=0;row<8;++row) for (u32 col=0;col<8;++col) {
            const float x=float(col)*2.f, y=float(row)*2.f;
            for (float value:{x,y,0.f, x+1.f,y,0.f, x,y+1.f,0.f}) points.push_back(value);
            u32 base=u32(ids.size()); ids.push_back(base); ids.push_back(base+1); ids.push_back(base+2);
        }
        grid.num_vertices=u32(points.size()/3); grid.num_indices=u32(ids.size()); grid.index_size=4;
        grid.position_buffer.resize(points.size()*sizeof(float)); std::memcpy(grid.position_buffer.data(),points.data(),grid.position_buffer.size());
        grid.index_buffer.resize(ids.size()*sizeof(u32)); std::memcpy(grid.index_buffer.data(),ids.data(),grid.index_buffer.size());
        auto original=ray_geometry::BuildMeshBLAS(grid); auto optimized=original?ray_geometry::BuildExperimentalSAHMesh(*original):nullptr;
        ray_geometry::SceneBVH a,b; std::vector<ray_geometry::InstanceInput> aa{{original,world,4}},bb{{optimized,world,4}};
        bool equal=optimized && a.Build(aa) && b.Build(bb);
        for (u32 y=0;y<32 && equal;++y) for (u32 x=0;x<32 && equal;++x) {
            float ro[]={float(x)*.5f+.11f,float(y)*.5f+.13f,0.f}; ray_geometry::Hit ha,hb;
            int ra=ray_geometry::Trace(a,ro,d,.05f,50.f,ha), rb=ray_geometry::Trace(b,ro,d,.05f,50.f,hb);
            equal=ra==rb && (ra!=1 || (ha.triangle_id==hb.triangle_id && ha.instance_id==hb.instance_id && ha.t==hb.t));
        }
        check(equal,"experimental SAH 1024 grid rays match original geometry");
    }
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
    // R11: 旧用例用 "/dev/null/unwritable" 模拟写失败——该路径只在 POSIX 语义
    // 下必然不可写；Windows 把它当当前盘根相对路径 \dev\null\unwritable，十测
    // 实测该机器 create_directories 成功 → 保存成功 → persisted=true → 断言
    // 翻转失败。改用"把已存在的 BLAS 文件当目录"：两平台上 create_directories
    // 都必然失败，写失败模拟跨平台确定。
    bool persisted=true;auto memoryOnly=ray_geometry::LoadOrBuildMeshBLAS(mesh,blasFile/"unwritable",nullptr,&persisted);
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
