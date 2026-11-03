#include "Engine/Graphics/Lumen/SurfaceCache/OwnerCardRanges.h"
#include "Engine/Graphics/Lumen/SurfaceCache/CardTileOwners.h"
#include "Engine/Graphics/Lumen/SurfaceCache/StaticSurfacePool.h"
#include "Engine/Graphics/Lumen/SurfaceCache/TriangleSupportIndex.h"
#include "Engine/Graphics/Lumen/SurfaceCache/GeometrySurfaceResample.h"
#include "Engine/Graphics/Lumen/SurfaceCache/CoverageCardBudget.h"
#include "Engine/Graphics/Lumen/SurfaceCache/SurfaceStaticPages.h"
#include "Engine/Graphics/RHI/Core/CardSurfaceIdentity.h"
#include "Engine/Graphics/RHI/Core/CardBoundarySidecar.h"
#include "Engine/Graphics/Lumen/SurfaceCache/BoundaryStaticSamples.h"
#include "Engine/Graphics/Lumen/SurfaceCache/BoundaryPointIndex.h"
#include "Engine/Graphics/Lumen/SurfaceCache/BoundaryLightingPoints.h"
#include "Engine/Graphics/Lumen/SurfaceCache/BoundaryRadianceCache.h"
#include "Engine/Graphics/Lumen/SurfaceCache/BoundaryMixedIndex.h"
#include "Engine/Graphics/RHI/Core/FrameSlotCompletion.h"
#include <filesystem>
#include <iostream>
#include <cstring>
// R4: unistd.h 不存在于 MSVC——getpid 仅用于临时文件名防撞，走可移植 shim
// （Windows 的 process.h 提供 _getpid，语义相同）。
#if defined(_WIN32)
#include <process.h>
static unsigned sc_test_pid() { return static_cast<unsigned>(_getpid()); }
#else
#include <unistd.h>
static unsigned sc_test_pid() { return static_cast<unsigned>(getpid()); }
#endif
using namespace primal::graphics::lumen;
namespace ray_geometry = primal::graphics::ray_geometry;
int main() {
    const auto path = std::filesystem::temp_directory_path() / ("sc-pages-" + std::to_string(sc_test_pid()) + ".scsurf");
    ScsHeaderRT h{};
    h.magic = SurfaceStaticPages::kScsMagic; h.version = 4; h.header_bytes = sizeof(h);
    h.capture_near=.05f; h.capture_far_scale=1.f; h.capture_far_bias=.5f;
    h.flags = 3; h.texels_per_unit = 64; h.page_size = 32; h.min_resolution = 64; h.max_resolution = 128; h.mesh_count = 1;
    constexpr uint64_t plane = 64 * 64 * 4;
    ScsMeshRecordRT m{}; m.geometry_digest = 1; m.layout_digest = 2; m.material_digest = 3; m.card_count = 1;
    m.pages_offset = sizeof(h) + sizeof(m) + sizeof(ScsCardPageEntryRT); m.pages_bytes = plane * 5;
    ScsCardPageEntryRT pg{0,64,m.pages_offset,m.pages_offset+plane,m.pages_offset+2*plane,m.pages_offset+3*plane,m.pages_offset+4*plane};
    auto write = [&](bool truncate = false, bool corrupt_identity = false) {
        FILE* f = fopen(path.c_str(), "wb");
        fwrite(&h, sizeof(h),1,f); fwrite(&m,sizeof(m),1,f); fwrite(&pg,sizeof(pg),1,f);
        std::vector<unsigned char> bytes(truncate ? plane : plane*5,0);
        if(!truncate) std::memset(bytes.data()+plane*4,255,plane); if(corrupt_identity) std::memset(bytes.data()+plane*4,0,4);
        fwrite(bytes.data(),bytes.size(),1,f); fclose(f);
    };
    int failures = 0, checks = 0;
    auto check = [&](bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::cerr << "FAIL " << name << '\n'; } };
    {
        struct V {float x,y,z,w;};struct Matrix {V columns[4];};
        struct Card {V center,extent;uint32_t axis_direction,resolution;};
        Card card{{7,8,6,0},{10,10,10,0},258,64};
        Matrix matrix{{{2,0,0,0},{1,3,0,0},{0,0,-.5f,0},{4,5,6,1}}};
        primal::graphics::rhi::card_boundary::Mesh mesh;mesh.triangle_count=1;
        primal::graphics::rhi::card_boundary::Sample sample{};sample.triangle_id=0;sample.position[0]=1;sample.position[1]=1;
        sample.normal[0]=sample.normal[1]=float(1/std::sqrt(2.));sample.albedo[0]=128;sample.albedo[3]=255;
        mesh.samples.push_back(sample);std::vector<BoundaryLightPoint> points;
        check(AppendBoundaryLightPoints(mesh,card,matrix,3,41,20,points) && points.size()==1,"boundary light affine point pack");
        if(points.empty())return 1;
        check(points[0].world_footprint[0]==7 && points[0].world_footprint[1]==8 && points[0].world_footprint[2]==6,"boundary true position under shear scale translation");
        check(std::abs(points[0].normal[0]-float(3/std::sqrt(10.)))<1e-6f && std::abs(points[0].normal[1]-float(1/std::sqrt(10.)))<1e-6f,"inverse transpose normal under shear");
        check(points[0].identity[0]==3 && points[0].identity[2]==41 && points[0].identity[3]==20 && std::bit_cast<uint32_t>(points[0].albedo[3])==0,"light points retain identity epoch frame source");
        const size_t size=points.size();matrix.columns[2].z=0;
        check(!AppendBoundaryLightPoints(mesh,card,matrix,3,41,20,points) && points.size()==size,"singular transform preserves prior bank");matrix.columns[2].z=-.5f;
        matrix.columns[0].w=1;check(!AppendBoundaryLightPoints(mesh,card,matrix,3,41,20,points),"non affine transform rejected");matrix.columns[0].w=0;
        matrix.columns[0].x=std::bit_cast<float>(0x7fc00000u);
        check(!AppendBoundaryLightPoints(mesh,card,matrix,3,41,20,points),"NaN transform rejected under fast math");matrix.columns[0].x=2;
        mesh.samples[0].normal[0]=mesh.samples[0].normal[1]=0;mesh.samples[0].normal[2]=1;card.axis_direction=2;
        points.clear();check(AppendBoundaryLightPoints(mesh,card,matrix,3,41,20,points) && points[0].normal[2]==1,"mirrored normal faces card eye");
        card.center.x-=10;points.clear();
        check(AppendBoundaryLightPoints(mesh,card,matrix,3,41,20,points) && points.empty(),"exact far UV border excluded");
    }
    {
        using Index=BoundaryPointIndex;
        Index::Group group{1,7,{{{0,0,0},12},{{1,2,3},0x80000002u}}};
        std::vector<uint32_t> words;
        check(Index::Build(2,41,std::span(&group,1),words) && words[0]==Index::Magic && words[1]==41 && words[7]==2,"mixed point index header");
        check(words[8]==0 && words[9]==0 && words[11]==1 && words[words[10]]==7,"point index card and primitive isolation");
        auto saved=words;check(Index::Build(2,41,std::span(&group,1),words) && saved==words,"point index deterministic packing");
        group.card=2;check(!Index::Build(2,41,std::span(&group,1),words) && words.empty(),"invalid card clears stale index");group.card=1;
        group.triangle=0xffffffffu;check(!Index::Build(2,41,std::span(&group,1),words),"invalid original primitive rejected");group.triangle=7;
        group.points[0].position[0]=std::bit_cast<float>(0x7fc00000u);
        check(!Index::Build(2,41,std::span(&group,1),words),"point index NaN rejected under fast math");
        group.points[0].position[0]=std::bit_cast<float>(0x7f800000u);
        check(!Index::Build(2,41,std::span(&group,1),words),"point index infinity rejected under fast math");group.points[0].position[0]=0;
        Index::Group duplicate[]={group,group};check(!Index::Build(2,41,duplicate,words),"duplicate card primitive rejected");
        auto source=group.points[1].source;group.points[1].source=group.points[0].source;
        check(!Index::Build(2,41,std::span(&group,1),words),"duplicate point source rejected");
        group.points[1].source=0xffffffffu;
        check(!Index::Build(2,41,std::span(&group,1),words),"invalid point source rejected");group.points[1].source=source;
        group.points.resize(Index::MaxGroupPoints+1);
        check(!Index::Build(2,41,std::span(&group,1),words),"per primitive point budget enforced");
        check(!Index::Build(Index::MaxCards+1,41,{},words),"card directory budget enforced");
    }
    {
        namespace boundary=primal::graphics::rhi::card_boundary;
        boundary::Mesh mesh; mesh.geometry_digest=1; mesh.layout_digest=2; mesh.material_digest=3; mesh.triangle_count=2;
        boundary::Sample sample; sample.triangle_id=1; sample.normal[1]=1; sample.albedo[3]=255;
        mesh.samples.push_back(sample);
        std::vector<uint8_t> good;
        check(boundary::Encode({mesh},good),"boundary encode");
        std::vector<boundary::Mesh> loaded;
        check(boundary::Decode(good,loaded) && loaded.size()==1 && loaded[0].samples[0].triangle_id==1,"boundary decode");
        check(boundary::FindMesh(loaded,1,2,3)==0 && boundary::FindMesh(loaded,1,2,4)==0xffffffffu,
              "boundary material identity isolation");
        auto rejected=[&](std::vector<uint8_t> bad, bool rehash=true) {
            if (rehash) {
                boundary::Header header; std::memcpy(&header,bad.data(),sizeof(header));
                header.body_hash=boundary::Hash(bad.data()+sizeof(header),bad.size()-sizeof(header));
                std::memcpy(bad.data(),&header,sizeof(header));
            }
            loaded={mesh};
            return !boundary::Decode(bad,loaded) && loaded.empty();
        };
        auto bad=good; bad.back()^=1;
        check(rejected(bad,false),"boundary checksum rejects corrupt payload");
        bad=good; bad.resize(bad.size()-1);
        check(rejected(bad),"boundary truncated payload rejects and clears stale data");
        bad=good; bad.push_back(0);
        check(rejected(bad),"boundary trailing payload rejected");
        boundary::Header header; std::memcpy(&header,good.data(),sizeof(header));
        auto wrong=header; wrong.version=2; bad=good; std::memcpy(bad.data(),&wrong,sizeof(wrong));
        check(rejected(bad),"boundary unsupported version rejected");
        wrong=header; wrong.mesh_count=boundary::MaxMeshes+1; bad=good; std::memcpy(bad.data(),&wrong,sizeof(wrong));
        check(rejected(bad),"boundary directory budget rejected");
        boundary::Record rec; std::memcpy(&rec,good.data()+sizeof(header),sizeof(rec));
        auto recordBad=[&](boundary::Record r) { auto b=good; std::memcpy(b.data()+sizeof(header),&r,sizeof(r)); return rejected(b); };
        auto r=rec; r.samples_offset=sizeof(header);
        check(recordBad(r),"boundary payload may not alias directory");
        r=rec; r.samples_bytes=~uint64_t(0);
        check(recordBad(r),"boundary overflowing payload size rejected");
        r=rec; r.sample_count=boundary::MaxSamplesPerMesh+1;
        check(recordBad(r),"boundary per-mesh sample budget rejected");
        r=rec; r.material_digest=0;
        check(recordBad(r),"boundary unknown material rejected");
        auto sampleBad=[&](boundary::Sample s) { auto b=good; std::memcpy(b.data()+rec.samples_offset,&s,sizeof(s)); return rejected(b); };
        auto s=sample; s.triangle_id=2;
        check(sampleBad(s),"boundary original TriangleID range rejected");
        s=sample; const uint32_t nan=0x7fc00001u; std::memcpy(&s.position[0],&nan,4);
        check(sampleBad(s),"boundary NaN position rejected under fast math");
        s=sample; s.normal[1]=0;
        check(sampleBad(s),"boundary zero normal rejected");
        s=sample; s.albedo[3]=0;
        check(sampleBad(s),"boundary uncovered point rejected");
        auto other=mesh; other.material_digest=4;
        check(boundary::Encode({other,mesh},good) && boundary::Decode(good,loaded) &&
              boundary::FindMesh(loaded,1,2,3)==0 && boundary::FindMesh(loaded,1,2,4)==1,
              "boundary variants keep canonical distinct owner material records");
        bad=good; boundary::Record duplicate;
        std::memcpy(&duplicate,bad.data()+sizeof(header)+sizeof(duplicate),sizeof(duplicate));
        duplicate.material_digest=3;
        std::memcpy(bad.data()+sizeof(header)+sizeof(duplicate),&duplicate,sizeof(duplicate));
        check(rejected(bad),"boundary duplicate directory rejected even with repaired checksum");
        check(!boundary::Encode({mesh,mesh},good) && good.empty(),"boundary duplicate identity rejected");
        const auto oversized=path.string()+".scboundary";
        FILE* file=std::fopen(oversized.c_str(),"wb");
        bool wrote=false;
        if (file) {
            wrote=std::fseek(file,static_cast<long>(boundary::MaxFileBytes),SEEK_SET)==0 && std::fputc(0,file)!=EOF;
            std::fclose(file);
        }
        loaded={mesh};
        check(wrote && !boundary::Read(oversized.c_str(),loaded) && loaded.empty(),"boundary oversized file rejected before payload allocation");
        std::remove(oversized.c_str());
    }
    {
        TriangleSupportIndex index(3,7,true);
        std::vector<uint32_t> ids(64,9u);std::vector<float> depth(64,1.f);std::vector<uint8_t> rgba(256,255u);
        ids[0]=11u;ids[1]=12u;rgba[7]=0u;ids[2]=13u;depth[2]=std::numeric_limits<float>::quiet_NaN();
        check(index.Add(0,8,ids,depth,rgba),"presence complete mixed sources");
        auto offset=index.words[4],count=index.words[5];
        check(count==4 && index.words[offset]==9 && index.words[offset+3]==11 && index.words[offset+6]==12 && index.words[offset+9]==13,"presence includes singleton transparent and nonfinite IDs");
        check(index.words[offset+5]==0 && index.words[offset+8]==0 && index.words[offset+11]==0,"unsupported IDs have no hull support");
        std::fill(ids.begin(),ids.end(),0xffffffffu);
        check(index.Add(1,8,ids,depth,rgba),"presence known empty card");
        check(!index.Add(2,8,std::span<const uint32_t>(ids).first(1),depth,rgba),"presence incomplete upload rejects");
        check(index.FinalizePresence(),"presence sideband finalization");
        const size_t footer=index.words.size()-4;const uint32_t directory=index.words[footer+1];
        check(index.words[footer]==0x53435031u && directory==index.words[3] && index.words[footer+2]==3 && index.words[footer+3]==index.words[3],"presence sideband layout and legacy bounds");
        check(index.words[directory]==1 && index.words[directory+1]==1 && index.words[directory+2]==0,"presence complete empty and unknown states distinguished");
        const size_t size=index.words.size();check(index.FinalizePresence() && index.words.size()==size,"presence finalize idempotent");
        check(!index.Add(2,8,ids,depth,rgba),"finalized presence rejects late mutation");
    }
    {
        TriangleSupportIndex index(4,7,true);std::vector<uint32_t> ids(16,9u);std::vector<float> depth(16,1.f);std::vector<uint8_t> rgba(64,255);
        check(index.Add(0,4,ids,depth,rgba) && index.Add(3,4,ids,depth,rgba),"owner mask known endpoints");
        std::fill(ids.begin(),ids.end(),7u);check(index.Add(2,4,ids,depth,rgba),"owner mask interleaved owner");
        std::vector<uint32_t> owners={2,2,1,2};
        check(index.FinalizeOwnerMasks(owners,4),"owner mask reverse directory build");
        check(!index.Add(1,4,ids,depth,rgba),"owner mask completion freezes source");
        const size_t size=index.words.size();check(index.FinalizeOwnerMasks(owners,4) && index.words.size()==size,"owner mask completion idempotent");
        check(index.FinalizePresence(),"owner mask retains per-card completeness footer");
        auto f=index.words[index.words.size()-3]-5u,base=index.words[f+1];
        check(index.words[f]==0x53434f31u && index.words[f+2]==4 && index.words[f+3]==f && base==index.words[3],"owner mask payload bounds");
        auto row=base+2u*7u,record=index.words[row+4];
        check(index.words[row]==0 && index.words[row+1]==4 && index.words[row+2]==2 && index.words[row+3]==0,"unknown card always allowed, interleaved owner excluded");
        check(index.words[row+5]==1 && index.words[record]==9 && index.words[record+1]==9 && index.words[record+2]==0,"shared triangle merged into ordered card bits");
        check(index.words[base+6]==0 && index.words[base+3u*7u+6]==0,"absent owners retain per-card fallback");
    }
    {
        TriangleSupportIndex index(64,7,true);std::vector<uint32_t> ids(16,12u);std::vector<float> depth(16,1.f);std::vector<uint8_t> rgba(64,255);
        check(index.Add(63,4,ids,depth,rgba),"owner mask highest card source");
        std::vector<uint32_t> owners(64,0u);check(index.FinalizeOwnerMasks(owners,1) && index.FinalizePresence(),"owner mask supports 64-card span");
        auto f=index.words[index.words.size()-3]-5u,base=index.words[f+1],record=index.words[base+4];
        check(index.words[record+1]==0 && index.words[record+2]==0x80000000u && index.words[base+2]==0xffffffffu && index.words[base+3]==0x7fffffffu,"owner mask bit63 and unknown masks exact");
    }
    {
        TriangleSupportIndex index(65,7,true);std::vector<uint32_t> owners(65,0u);
        check(index.FinalizeOwnerMasks(owners,1) && index.FinalizePresence(),"oversized owner safely finalizes");
        auto f=index.words[index.words.size()-3]-5u,base=index.words[f+1];check(index.words[base+6]==0,"oversized span uses per-card fallback");
        TriangleSupportIndex invalid(1,7,true);std::vector<uint32_t> bad={1};auto original=invalid.words;
        check(!invalid.FinalizeOwnerMasks(bad,1) && invalid.words==original,"invalid owner directory leaves original words intact");
    }
    { // Whole-card hull index: sparse support, holes and primitive boundaries.
        TriangleSupportIndex index(2,7);std::vector<uint32_t> ids(64,9u);
        std::vector<float> depths(64,.2f);std::vector<uint8_t> rgba(256,255);
        ids[0]=0xffffffffu;ids[63]=3u;depths[7]=std::bit_cast<float>(0x7fc00000u);rgba[56*4+3]=0;
        check(index.Add(1,8,ids,depths,rgba),"support index build");
        check(index.words[0]==TriangleSupportIndex::Magic && index.words[1]==7 && index.words[2]==2 && index.words[3]==index.words.size(),"support epoch header");
        check(index.words[5]==0 && index.words[7]==1,"support omits singleton primitive and empty card");
        auto record=index.words[6];check(index.words[record]==9,"support uses original primitive");
        auto offset=index.words[record+1],count=index.words[record+2];bool valid=count>=3 && count<=64;
        for(uint32_t k=0;k<count;++k){auto pixel=index.words[offset+k];auto at=(pixel>>16)*8+(pixel&65535);valid &= at<64 && ids[at]==9 && rgba[at*4+3]>=128 && at!=7;}
        check(valid,"support hull never includes invalid or other primitive texel");
        check(!index.Add(1,8,ids,depths,rgba),"support duplicate card rejected");
        check(!index.Add(2,8,ids,depths,rgba),"support card range rejected");
        check(!index.Add(0,8,{},depths,rgba),"support plane size rejected");
        std::fill(ids.begin(),ids.end(),0xffffffffu);ids[27]=ids[28]=9;
        check(index.Add(0,8,ids,depths,rgba) && index.words[index.words[4]+2]==2,"support finite segment preserved");
    }
    // Independently known sloped surface: z=.2+.3*x+.1*y, under a
    // smaller front triangle (ID9) which must not borrow the back layer ID7.
    ray_geometry::Node node{};
    node.lo[0]=node.lo[1]=0;node.lo[2]=.1;node.hi[0]=node.hi[1]=1;node.hi[2]=1;
    node.links[3]=2;
    ray_geometry::Triangle rt[2]{};
    const float slopeA[3]={0,0,.2},slopeB[3]={1,0,.3},slopeC[3]={0,1,.1};
    std::memcpy(rt[0].a,slopeA,12);std::memcpy(rt[0].e1,slopeB,12);std::memcpy(rt[0].e2,slopeC,12);
    rt[0].a[3]=std::bit_cast<float>(7u);
    rt[1].a[0]=rt[1].a[1]=.1;rt[1].a[2]=.1;rt[1].e1[0]=rt[1].e2[1]=.5;rt[1].a[3]=std::bit_cast<float>(9u);
    const float origin[3]={.2,.2,0},direction[3]={0,0,1};ray_geometry::Hit mh;
    check(ray_geometry::TraceMesh({&node,1},rt,0,origin,direction,.05,2,mh)==1 && mh.triangle_id==9,
          "mesh resampling chooses actual front triangle");
    check(ray_geometry::TraceMesh({&node,1},rt,0,origin,direction,.15,2,mh)==1 && mh.triangle_id==7,
          "mesh trace near clip preserves back primitive ordinal");
    auto invalid=node;invalid.links[2]=3;
    check(ray_geometry::TraceMesh({&invalid,1},rt,0,origin,direction,.05,2,mh)==-1,"invalid BLAS range unavailable");
    float nanO[3]={std::bit_cast<float>(0x7fc00000u),0,0};
    check(ray_geometry::TraceMesh({&node,1},rt,0,nanO,direction,.05,2,mh)==-1,"nonfinite mesh ray rejected");
    std::vector<uint8_t> sa(64*64*4,0),sn(sa.size(),0);
    std::vector<uint32_t> si(64*64,0xffffffffu);
    for(uint32_t y=0;y<64;++y)for(uint32_t x=0;x<64;++x) {
        float a=(x+.5f)/64,b=(y+.5f)/64;
        if(a+b>1)continue;
        const uint32_t at=y*64+x;si[at]=a>=.1 && b>=.1 && a+b<=.7?9u:7u;
        sa[at*4]=uint8_t(si[at]);sa[at*4+3]=255;sn[at*4]=31;
    }
    GeometrySurfacePage rebuilt;
    const float center[3]={.5,.5,0},extent[3]={.5,.5,.5};
    check(ResampleGeometrySurface({&node,1},rt,0,center,extent,258,64,32,sa,sn,si,rebuilt),"sloped target-center resampling");
    bool exact=true,attributes=true,holes=true;
    for(uint32_t y=0;y<32;++y)for(uint32_t x=0;x<32;++x) {
        const uint32_t at=y*32+x;double a=(x+.5)/32,b=(y+.5)/32;
        if(rebuilt.triangles[at]!=0xffffffffu) {
            const double truth=rebuilt.triangles[at]==9?.1:.2+.3*a+.1*b;
            exact &= std::abs(rebuilt.depth[at]-truth)<1e-6;
            attributes &= rebuilt.albedo[at*4]==rebuilt.triangles[at] && rebuilt.normal[at*4]==31;
        } else holes &= rebuilt.depth[at]<0 && rebuilt.albedo[at*4+3]==0;
    }
    check(exact && rebuilt.valid>0,"all target depths match independent sloped-plane equation");
    check(attributes,"depth and attribute original TriangleID agree");check(holes,"honest uncovered texels");
    std::fill(si.begin(),si.end(),7u); // front triangle has no source attributes
    check(ResampleGeometrySurface({&node,1},rt,0,center,extent,258,64,32,sa,sn,si,rebuilt) &&
          rebuilt.depth[6*32+6]<0 && rebuilt.triangles[6*32+6]==0xffffffffu,
          "missing front primitive never borrows rear source attributes");
    // A smaller primitive appears only off the nearest source center, but its
    // target-center triangle is real. The footprint recovers its identity.
    std::fill(sa.begin(),sa.end(),0);std::fill(si.begin(),si.end(),0xffffffffu);
    si[12*64+12]=9;sa[(12*64+12)*4]=9;sa[(12*64+12)*4+3]=255;
    check(ResampleGeometrySurface({&node,1},rt,0,center,extent,258,64,32,sa,sn,si,rebuilt) &&
          rebuilt.triangles[6*32+6]==9,"footprint retains matching primitive lost by nearest-center downsample");
    check(!ResampleGeometrySurface({&node,1},rt,0,center,extent,258,64,32,{},sn,si,rebuilt),"invalid source plane rejected");
    // Upsample and opposite axis: target centers remain true geometry rather
    // than repeating depth at a displaced source coordinate.
    std::fill(sa.begin(),sa.end(),0);std::fill(si.begin(),si.end(),0xffffffffu);
    for(uint32_t at=0;at<si.size();++at){sa[at*4+3]=255;sa[at*4]=7;si[at]=7;}
    const float upperCenter[3]={.5,.5,1};
    auto slopeNode=node;slopeNode.links[3]=1;
    check(ResampleGeometrySurface({&slopeNode,1},{rt,1},0,upperCenter,extent,2,64,96,sa,sn,si,rebuilt),"negative axis noninteger upsample");
    exact=rebuilt.triangles.size()==96*96;
    for(uint32_t y=0;exact && y<96;++y)for(uint32_t x=0;x<96;++x)if(rebuilt.triangles[y*96+x]!=0xffffffffu)
        exact &= std::abs(rebuilt.depth[y*96+x]-(.8-.3*(x+.5)/96-.1*(y+.5)/96))<1e-6;
    check(exact && rebuilt.valid>0,"negative-axis target points match independent plane");
    // Regression: a float plane must occupy N words / 4N bytes, not N/4
    // words followed by unrelated TriangleIDs at the shader's depth offsets.
    std::vector<float> sourceDepth(4096);
    for(size_t i=0;i<sourceDepth.size();++i) sourceDepth[i]=i%7==0 ? -1.f : .125f+float(i)*.0625f;
    std::vector<uint32_t> pool{0x01020304u,0xfedcba98u};
    const uint32_t depthAt=AppendStaticDepthPlane(pool,sourceDepth);
    const uint32_t trianglesAt=uint32_t(pool.size());
    pool.insert(pool.end(),sourceDepth.size(),0x00012345u);
    check(depthAt==2 && trianglesAt==2+sourceDepth.size(),"complete depth plane offset");
    for(size_t q=0;q<4;++q)
        check(std::memcmp(pool.data()+depthAt+q*1024,sourceDepth.data()+q*1024,1024*sizeof(float))==0,
              "depth quarter preserved next to triangle IDs");
    check(pool[0]==0x01020304u && pool[1]==0xfedcba98u && pool[trianglesAt]==0x00012345u,
          "adjacent planes preserved");
    const auto before=pool.size();
    check(AppendStaticDepthPlane(pool,{})==before && pool.size()==before,"empty depth plane");
    std::vector<CoverageCardRequest> requests;
    for(u32 i=0;i<2532;++i) requests.push_back({i,i/389,128,1.f+float(i%17)});
    CoverageCardPlan plan;
    check(PlanCoverageCards(requests,2048,32,4096,plan) && plan.placements.size()==2532,
          "coverage plan retains all Sponza-sized card candidates");
    std::vector<u32> coverageOwners;
    check(BuildCardTileOwners(std::span<const CoverageCardPlacement>(plan.placements),2048,32,coverageOwners),
          "coverage plan has no overlap and remains in atlas bounds");
    uint64_t allocated=0;std::vector<u32> seen(requests.size(),0);
    for(const auto& pl:plan.placements) {
        allocated+=uint64_t(pl.resolution)*pl.resolution;++seen[pl.pool_index];
        if(pl.resolution<32 || pl.resolution>128) ++failures;
    }
    check(allocated<=2048ull*2048 && std::all_of(seen.begin(),seen.end(),[](u32 n){return n==1;}),
          "coverage budget exact and every candidate emitted once");
    std::vector<CoverageCardRequest> density{{0,0,128,1,1},{1,2,128,64,8},{2,1,128,64,8},{3,0,128,64,8},{4,0,128,1,1}};
    check(PlanCoverageCards(density,128,32,5,plan,true),"density refinement plan");
    std::vector<u32> densityRes(5);
    for(const auto& p:plan.placements)densityRes[p.pool_index]=p.resolution;
    check(densityRes[0]==32 && densityRes[4]==32 && densityRes[1]==64 && densityRes[2]==64 && densityRes[3]==64,
          "long world spans refine before short high-rank cards");
    check(BuildCardTileOwners(std::span<const CoverageCardPlacement>(plan.placements),128,32,coverageOwners),
          "density refinement remains disjoint and in budget");
    density[0].projected_span=std::bit_cast<float>(0x7fc00000u);
    check(!PlanCoverageCards(density,128,32,5,plan,true),"nonfinite density span rejected under fast math");
    check(PlanCoverageCards(requests,2048,32,4096,plan,true) && plan.placements.size()==2532 &&
          BuildCardTileOwners(std::span<const CoverageCardPlacement>(plan.placements),2048,32,coverageOwners),
          "dense Sponza-sized refinement keeps every card and exact owner isolation");
    std::vector<CoverageCardRequest> priority{{0,1,64,100},{1,0,64,1},{2,0,64,2},{3,2,64,100}};
    check(PlanCoverageCards(priority,128,32,4,plan),"small priority plan");
    // Four cards can all refine to64 exactly; the dense fill is independently
    // checked with the tile owner grid, including the atlas's final page.
    check(BuildCardTileOwners(std::span<const CoverageCardPlacement>(plan.placements),128,32,coverageOwners) &&
          std::all_of(coverageOwners.begin(),coverageOwners.end(),[](u32 id){return id!=0xffffffffu;}),"exact capacity fill");
    priority.push_back({4,0,64,1});
    check(!PlanCoverageCards(priority,128,32,4,plan) && plan.placements.empty(),"card buffer capacity respected");
    check(!PlanCoverageCards(requests,1024,32,4096,plan),"insufficient base-page budget rejected");
    check(PlanCoverageCards(priority,128,32,8,plan),"priority budget fits base coverage");
    std::vector<u32> refined(5);
    for(const auto& pl:plan.placements) refined[pl.pool_index]=pl.resolution;
    check(refined[1]==64 && refined[2]==64 && refined[4]==64 && refined[0]==32 && refined[3]==32,
          "main-card rank refinement does not drop lower-rank coverage");
    priority[1].projected_area=std::bit_cast<float>(0x7fc00000u);
    check(!PlanCoverageCards(priority,128,32,8,plan),"nonfinite coverage priority rejected");
    priority[1].projected_area=1.f;
    priority[1].pool_index=0;
    check(!PlanCoverageCards(priority,128,32,8,plan),"duplicate candidate rejected");
    check(!PlanCoverageCards({},2048,48,4096,plan),"unsupported grid rejected");
    check(PlanCoverageCards({},2048,32,4096,plan) && plan.placements.empty(),"empty coverage plan");
    SurfaceStaticPages pages;
    write(); check(pages.Attach(path.c_str()),"valid v4");
    {
        namespace boundary=primal::graphics::rhi::card_boundary;
        boundary::Mesh mesh; mesh.geometry_digest=1; mesh.layout_digest=2; mesh.material_digest=3; mesh.triangle_count=1;
        boundary::Sample sample; sample.triangle_id=0; sample.normal[1]=1; sample.albedo[3]=255; mesh.samples.push_back(sample);
        const auto file=path.string()+".scboundary";
        BoundaryStaticSamples auxiliary;
        check(boundary::Write(file.c_str(),{mesh}) && auxiliary.Attach(file.c_str(),pages) &&
              auxiliary.MeshCount()==1 && auxiliary.SampleCount()==1 && auxiliary.Find(0,pages),"boundary primary asset binding");
        check(!auxiliary.Find(1,pages),"boundary missing static slot rejected");
        pages.Detach();
        check(!auxiliary.Find(0,pages) && !auxiliary.Attach(file.c_str(),pages) && auxiliary.MeshCount()==0,
              "boundary primary detach invalidates lookup and clears failed attachment");
        write(); check(pages.Attach(path.c_str()),"primary reattach after boundary test");
        mesh.material_digest=4;
        check(boundary::Write(file.c_str(),{mesh}) && !auxiliary.Attach(file.c_str(),pages) && auxiliary.MeshCount()==0,
              "boundary foreign material attachment rejected");
        mesh.material_digest=3; mesh.geometry_digest=4;
        check(boundary::Write(file.c_str(),{mesh}) && !auxiliary.Attach(file.c_str(),pages),"boundary foreign geometry attachment rejected");
        mesh.geometry_digest=1; mesh.layout_digest=4;
        check(boundary::Write(file.c_str(),{mesh}) && !auxiliary.Attach(file.c_str(),pages),"boundary foreign layout attachment rejected");
        std::remove(file.c_str());
    }
    check(pages.FindMesh(1,2,3)==0,"matching identities");
    check(pages.FindMesh(1,2,4)==pages.kInvalidSlot,"material mismatch");
    check(pages.FindMesh(4,2,3)==pages.kInvalidSlot,"geometry mismatch");
    check(pages.FindMesh(1,4,3)==pages.kInvalidSlot,"layout mismatch");
    check(pages.FindMesh(1,2,0)==pages.kInvalidSlot,"unknown material");
    std::vector<u8> a,n; std::vector<f32> d; std::vector<u32> ids; u32 res=0;
    check(pages.ReadCardPlanes(0,0,a,n,d,res,&ids) && res==64 && a.size()==plane && d.size()==4096 && ids.size()==4096 && ids[0]==0xffffffffu,"planes");
    check(!pages.ReadCardPlanes(0,1,a,n,d,res),"absent card");
    pages.Detach();
    {
        // Two materials share geometry/layout; selecting one must retain its
        // own payload and must never accept an unrelated material fingerprint.
        auto header=h; header.mesh_count=2;
        auto aRecord=m, bRecord=m; bRecord.material_digest=4;
        const auto payload=sizeof(h)+2*(sizeof(m)+sizeof(pg));
        aRecord.pages_offset=payload; bRecord.pages_offset=payload+plane*5;
        ScsCardPageEntryRT aPage{0,64,payload,payload+plane,payload+2*plane,payload+3*plane,payload+4*plane};
        ScsCardPageEntryRT bPage{0,64,payload+5*plane,payload+6*plane,payload+7*plane,payload+8*plane,payload+9*plane};
        FILE* file=fopen(path.c_str(),"wb");
        fwrite(&header,sizeof(header),1,file);
        fwrite(&aRecord,sizeof(aRecord),1,file); fwrite(&aPage,sizeof(aPage),1,file);
        fwrite(&bRecord,sizeof(bRecord),1,file); fwrite(&bPage,sizeof(bPage),1,file);
        std::vector<u8> pixels(plane*5,0); std::memset(pixels.data()+plane*4,255,plane);
        pixels[0]=80; fwrite(pixels.data(),pixels.size(),1,file);
        pixels[0]=40; fwrite(pixels.data(),pixels.size(),1,file); fclose(file);
        check(pages.Attach(path.c_str()) && pages.FindMesh(1,2,3)==0 && pages.FindMesh(1,2,4)==1,
              "material variants select exact triple key");
        check(pages.FindMesh(1,2,5)==pages.kInvalidSlot,"unbaked material variant rejected");
        check(pages.ReadCardPlanes(0,0,a,n,d,res,&ids) && a[0]==80 &&
              pages.ReadCardPlanes(1,0,a,n,d,res,&ids) && a[0]==40,"variant payload remains isolated");
        pages.Detach();
    }
    pages.Detach(); write(false,true); check(pages.Attach(path.c_str()) && !pages.ReadCardPlanes(0,0,a,n,d,res,&ids),"invalid texel identity rejected");
    pages.Detach(); h.version=1; write(); check(!pages.Attach(path.c_str()),"old version rejected"); h.version=4;
    h.version=2; write(); check(!pages.Attach(path.c_str()),"v2 depth contract rejected"); h.version=4;
    h.version=3; write(); check(!pages.Attach(path.c_str()),"v3 without triangle identity rejected"); h.version=4;
    h.flags=1; write(); check(!pages.Attach(path.c_str()),"missing triangle plane rejected"); h.flags=3;
    pg.triangle_offset=0; write(); check(!pages.Attach(path.c_str()),"triangle metadata alias rejected"); pg.triangle_offset=m.pages_offset+4*plane;
    h.capture_far_scale=3.f; write(); check(!pages.Attach(path.c_str()),"far contract rejected"); h.capture_far_scale=1.f;
    h.capture_near=.01f; write(); check(!pages.Attach(path.c_str()),"near contract rejected"); h.capture_near=.05f;
    write(true); check(!pages.Attach(path.c_str()),"truncated payload");
    pg.albedo_offset=0; write(); check(!pages.Attach(path.c_str()),"metadata alias rejected"); pg.albedo_offset=m.pages_offset;
    pg.resolution=65; write(); check(!pages.Attach(path.c_str()),"invalid resolution"); pg.resolution=64;
    h.texels_per_unit=0; write(); check(!pages.Attach(path.c_str()),"invalid density");
    namespace id = primal::graphics::rhi::card_surface_identity;
    unsigned char pixels[]{10,20,30,255}; const auto tex=id::Texture(1,1,pixels); pixels[0]++;
    check(tex != id::Texture(1,1,pixels),"texture contents change identity");
    float tint[]{1,1,1}; const auto mat=id::Material(tex,tint,128,0); tint[0]=.5f;
    check(mat != id::Material(tex,tint,128,0),"tint changes identity");
    struct Card { u32 atlas_offset_x,atlas_offset_y,resolution; };
    std::vector<Card> cards;
    for(u32 i=0;i<40;++i) cards.push_back({(i%8)*64,(i/8)*64,64});
    std::vector<u32> owners;
    check(BuildCardTileOwners(std::span<const Card>(cards),512,16,owners),"full owner table");
    check(owners[(4*4+2)*32+7*4+2]==39,"owner beyond first 24 cards");
    cards[39]={0,0,64};
    check(!BuildCardTileOwners(std::span<const Card>(cards),512,16,owners),"overlap rejected");
    cards[39]={448,256,0};
    check(BuildCardTileOwners(std::span<const Card>(cards),512,16,owners) && owners[31*32+31]==0xFFFFFFFFu,"unallocated tile invalid");
    check(!BuildCardTileOwners(std::span<const Card>(cards),512,0,owners),"zero tile rejected");
    {
        BoundaryRadianceCache cache;BoundaryRadianceCache::Key key{7,11,19};
        BoundaryLightPoint point{};point.identity[0]=2;point.identity[1]=31;point.identity[2]=7;point.identity[3]=120;
        BoundaryRadianceEntry entry{};std::memcpy(entry.identity,point.identity,sizeof(entry.identity));
        entry.state[0]=120;entry.state[1]=7;entry.state[2]=1;entry.radiance[3]=entry.indirect[3]=1;
        auto begin=[&](){return cache.Begin(key,120,{&point,1});};auto publish=[&](){return cache.Publish(key,120,{&entry,1});};
        auto find=[&](){return cache.Find(key,120,0,2,31);};
        check(begin() && !find(),"cache unavailable until complete refresh");
        check(publish() && find(),"valid black measurement cached");
        check(!cache.Find(key,121,0,2,31),"skipped next frame cannot reuse old light");
        check(!cache.Find({8,11,19},120,0,2,31),"allocation change invalidates cache");
        check(!cache.Find({7,12,19},120,0,2,31),"geometry change invalidates cache");
        check(!cache.Find({7,11,20},120,0,2,31),"material/transform content change invalidates cache");
        check(!cache.Find(key,120,0,2,32) && !cache.Find(key,120,0,3,31),"cache isolates card and original primitive");
        entry.identity[3]=119;check(!publish() && !find(),"stale refresh removes previous light");entry.identity[3]=120;
        check(begin() && publish() && begin() && !find(),"refresh start hides old measurement");
        entry.radiance[0]=std::bit_cast<float>(0x7fc00001u);check(!publish() && !find(),"nonfinite cache rejected under fast math");entry.radiance[0]=0;
        entry.radiance[0]=std::bit_cast<float>(0x7f800000u);check(!publish(),"infinite radiance rejected");
        entry.radiance[0]=65;check(!publish(),"unclamped radiance rejected");
        entry.radiance[0]=-1;check(!publish(),"negative radiance rejected");entry.radiance[0]=0;
        entry.indirect[0]=1;check(!publish(),"indirect exceeds total rejected");entry.indirect[0]=0;
        entry.state[2]=0;entry.state[3]=8;entry.radiance[3]=entry.indirect[3]=0;
        check(publish() && !find(),"insufficient radio measurements never cached as valid black");
        entry.radiance[0]=1;check(!publish(),"invalid refresh cannot retain old color");entry.radiance[0]=0;
        entry.state[2]=1;entry.state[3]=0;entry.radiance[3]=entry.indirect[3]=1;
        check(!cache.Publish(key,120,{}),"partial bank publication rejected");
        check(begin() && publish(),"valid refresh recovers after failure");cache.Clear();check(!find(),"shutdown clears cache");
    }
    {
        struct V {float x,y,z,w;};struct Matrix {V columns[4];};
        struct AtlasCard {V center,extent;uint32_t axis_direction,resolution,atlas_offset_x,atlas_offset_y;};
        AtlasCard card{{10,20,30,0},{2,4,6,0},258,2,8,12};
        Matrix matrix{{{1,0,0,0},{0,2,0,0},{0,0,3,0},{10,20,30,1}}};
        std::vector<uint32_t> ids{7,8,UINT32_MAX,7};std::vector<float> depths{1,2,3,4};
        std::vector<uint8_t> albedo(16,255);albedo[7]=0;
        std::vector<BoundaryPointIndex::Group> groups;
        auto append=[&](){return AppendBoundaryAtlasPoints(card,matrix,1,258,2,ids,depths,albedo,groups);};
        check(append() && groups.size()==1 && groups[0].points.size()==2,"atlas mirror excludes uncovered and invalid primitive pixels");
        const auto first=groups[0].points[0];
        check(first.position[0]==9 && first.position[1]==18 && first.position[2]==33 && first.source==(8u|(12u<<16)),"atlas mirror retains GPU world position and atlas source");
        check(groups[0].points[1].position[2]==42,"atlas depth receives instance scale");
        depths[3]=std::bit_cast<float>(0x7fc00001u);
        check(!append() && groups.size()==1,"bad later atlas depth preserves complete prior groups");depths[3]=4;
        matrix.columns[1].x=1;check(!append(),"rotated or sheared atlas requires actual GPU reconstruction");matrix.columns[1].x=0;
        matrix.columns[2].z=-3;check(!append(),"mirrored static composite rejected by automatic mirror");matrix.columns[2].z=3;
        card.atlas_offset_y=32767;check(!append(),"atlas reference cannot alias auxiliary high bit");card.atlas_offset_y=12;
        check(!AppendBoundaryAtlasPoints(card,matrix,1,2,2,ids,depths,albedo,groups),"wrong local face rejected");
        check(!AppendBoundaryAtlasPoints(card,matrix,1,258,4,ids,depths,albedo,groups),"resampled atlas requires actual GPU reconstruction");
        BoundaryLightPoint point{};point.identity[0]=1;point.identity[1]=7;point.identity[2]=41;
        point.world_footprint[0]=10;point.world_footprint[1]=20;point.world_footprint[2]=33;
        std::vector<BoundaryLightPoint> aux{point,point};aux[1].identity[1]=9;
        std::vector<uint32_t> words;
        auto build=[&](){return BuildBoundaryMixedIndex(2,41,groups,aux,words);};
        check(build() && words[7]==4 && words[11]==2,"runtime merge includes old atlas only for actual auxiliary primitive keys");
        auto packed=std::span(words).subspan(words[5]);bool source0=false,source1=false;
        for(size_t i=3;i<packed.size();i+=4){source0|=packed[i]==0x80000000u;source1|=packed[i]==0x80000001u;}
        check(source0 && source1,"automatic index sources use actual GPU bank indices");
        auto saved=words;check(build() && words==saved,"automatic mixed index deterministic");
        aux[0].identity[2]=40;check(!build() && words.empty(),"epoch mismatch clears automatic index");aux[0].identity[2]=41;
        aux[0].world_footprint[0]=std::bit_cast<float>(0x7fc00001u);check(!build(),"nonfinite auxiliary point rejected under fast math");aux[0].world_footprint[0]=10;
        groups.push_back(groups.front());check(!build(),"duplicate retained atlas primitive rejected");groups.pop_back();
        groups[0].points.resize(BoundaryPointIndex::MaxGroupPoints);check(!build(),"combined old and auxiliary point budget enforced");
    }
    {
        // More than the retired 16K limit, spread across bounded primitive
        // groups. The encoded auxiliary source must retain its high index.
        std::vector<BoundaryLightPoint> bank(16385);
        for(uint32_t i=0;i<bank.size();++i){bank[i].identity[0]=0;bank[i].identity[1]=i/8192;
            bank[i].identity[2]=41;bank[i].identity[3]=120;bank[i].world_footprint[0]=float(i);}
        std::vector<uint32_t> words;
        check(BuildBoundaryMixedIndex(1,41,{},bank,words),"source-point bank above 16K has a bounded mixed index");
        bool high=false;if(!words.empty())for(size_t i=words[5]+3;i<words.size();i+=4)high|=words[i]==0x80004000u;
        check(high,"mixed index preserves auxiliary source above 16K");
        bank.resize(BoundaryMaxLightPoints+1);
        check(!BuildBoundaryMixedIndex(1,41,{},bank,words) && words.empty(),"shared point limit overflow clears index");
        for(auto& p:bank){p.identity[2]=41;p.identity[3]=120;}
        BoundaryRadianceCache cache;BoundaryRadianceCache::Key key{41,1,1};
        check(cache.Begin(key,120,std::span(bank).first(BoundaryMaxLightPoints)),"current-frame cache accepts shared point bound");
        check(!cache.Begin(key,120,bank),"current-frame cache rejects shared bound overflow");
    }
    {
        primal::graphics::rhi::FrameSlotCompletion completed;
        check(!completed.CanReuse(0,0),"frame arithmetic alone cannot authorize slot reuse");
        check(completed.Publish(0) && completed.CanReuse(0,0),"successful fence authorizes first slot use");
        const auto serial=completed.Serial(0);
        check(!completed.CanReuse(0,serial),"same completion cannot authorize a second overwrite");
        check(!completed.CanReuse(1,0),"one fence cannot authorize another slot");
        completed.Invalidate();check(!completed.CanReuse(0,0),"failed next wait revokes active slot");
        check(completed.Publish(1) && completed.CanReuse(1,0) && !completed.CanReuse(0,0),"active host slot must match consumer slot");
        check(completed.Publish(0) && completed.CanReuse(0,serial),"later successful slot fence permits reuse without readback");
        check(!completed.Publish(3) && !completed.CanReuse(0,serial),"invalid completion fails closed");
        check(completed.Serial(3)==0 && !completed.CanReuse(3,0),"out of range slot has no completion");
    }
    {
        struct Card{uint32_t mesh_instance_id;};struct Range{uint32_t card_start,card_count;uint32_t _pad[2]{};};
        std::vector<Card> cards={{0},{0},{9},{0}};std::vector<Range> ranges={{0,2},{2,1},{3,1}};
        check(ValidateOwnerCardRanges(cards,ranges),"owner intervals support owner zero and disjoint ranges for same owner");
        ranges[0].card_count=3;check(!ValidateOwnerCardRanges(cards,ranges),"mixed-owner range rejected");ranges[0].card_count=2;
        ranges[1].card_start=3;check(!ValidateOwnerCardRanges(cards,ranges),"gap in range coverage rejected");ranges[1].card_start=1;
        check(!ValidateOwnerCardRanges(cards,ranges),"overlapping range coverage rejected");ranges[1].card_start=2;
        ranges.pop_back();check(!ValidateOwnerCardRanges(cards,ranges),"unmapped trailing cards rejected");ranges.push_back({3,1});
        ranges[2].card_count=UINT32_MAX;check(!ValidateOwnerCardRanges(cards,ranges),"overflowing card count rejected");ranges[2].card_count=1;
        ranges.insert(ranges.begin(),{0,0});check(ValidateOwnerCardRanges(cards,ranges),"empty range does not conceal any card");
        check(!ValidateOwnerCardRanges(cards,std::vector<Range>{}),"missing lookup table rejected for nonempty cards");
    }
    {
        struct Card{uint32_t mesh_instance_id;};struct Range{uint32_t card_start,card_count;uint32_t _pad[2]{};};
        std::vector<Card> cards={{0},{0},{2},{0}};std::vector<Range> ranges={{0,2},{2,1},{3,1}},directory;
        ranges[1]._pad[0]=77;ranges[1]._pad[1]=88;
        check(BuildOwnerCardDirectory(cards,ranges,directory),"dense owner directory accepts full validated partition");
        check(directory[0]._pad[0]==0 && directory[0]._pad[1]==4,"owner zero and disjoint ranges keep full ascending span");
        check(directory[2]._pad[0]==2 && directory[2]._pad[1]==3,"last directory slot maps exact owner span");
        check(directory[1]._pad[0]==0 && directory[1]._pad[1]==0,"missing owner clears padding rather than stale directory");
        check(ranges[1]._pad[0]==77 && ranges[1]._pad[1]==88 && directory[1].card_start==2 && directory[1].card_count==1,"GPU directory does not mutate CPU table or original range fields");
        auto saved=directory;check(BuildOwnerCardDirectory(cards,ranges,directory) && directory[0]._pad[1]==saved[0]._pad[1],"directory rebuild deterministic");
        cards[2].mesh_instance_id=3;check(!BuildOwnerCardDirectory(cards,ranges,directory) && directory.empty(),"owner outside table rejects and clears prior directory");
        cards[2].mesh_instance_id=UINT32_MAX;check(!BuildOwnerCardDirectory(cards,ranges,directory),"sparse high owner safely falls back to range scan");
        cards[2].mesh_instance_id=2;ranges[0].card_count=3;check(!BuildOwnerCardDirectory(cards,ranges,directory) && directory.empty(),"mixed owner range cannot build direct directory");
        check(BuildOwnerCardDirectory(std::vector<Card>{},std::vector<Range>{},directory) && directory.empty(),"empty scene has empty directory");
    }
    std::filesystem::remove(path);
    std::cout << checks-failures << '/' << checks << " checks passed\n";
    return failures ? 1 : 0;
}
