#include "Graphics/Volume/SparseParticleVolume.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
using namespace primal::graphics::volume;
int main() {
    int errors=0;
    auto check=[&](bool ok,const char* message){if(!ok){++errors;std::cerr<<"FAIL "<<message<<'\n';}};
    SparseVolumeConfig cfg; cfg.voxel_size=.125f;
    SparseParticleVolume v;
    check(v.Build({},cfg)&&v.BrickCount()==0&&v.atlas.empty(),"empty source has no atlas");
    VolumeParticle p{-.25f,.5f,0,1.1f,12};
    check(v.Build({&p,1},cfg),"single particle");
    auto single=v; const float d=v.Sample(p.x,p.y,p.z);
    check(d>11,"filled core");
    VolumeParticle pair[]={p,p};
    check(v.Build(pair,cfg)&&std::abs(v.Sample(p.x,p.y,p.z)-2*d)<1e-4,"overlap adds density");
    auto kernel=[&](float x,float y,float z) {
        double dx=(x-p.x)/p.radius,dy=(y-p.y)/p.radius,dz=(z-p.z)/p.radius;
        double t=std::max(0.0,1-dx*dx-dy*dy-dz*dz);return float(p.density*t*t);
    };
    // Independent dense analytic reference, including negative cells and brick seams.
    for(int i=0;i<1600;++i) {
        float x=-1.6f+float((i*37)%1600)/500, y=-1+float((i*71)%1600)/500,z=-1.6f+float((i*97)%1600)/500;
        float pos[]={x,y,z},f[3];int base[3];
        for(int a=0;a<3;++a){float q=pos[a]/cfg.voxel_size-.5f;base[a]=int(std::floor(q));f[a]=q-base[a];}
        float reference=0;
        for(int k=0;k<8;++k) reference+=kernel((base[0]+(k&1)+.5f)*cfg.voxel_size,
            (base[1]+((k>>1)&1)+.5f)*cfg.voxel_size,(base[2]+((k>>2)&1)+.5f)*cfg.voxel_size)*
            ((k&1)?f[0]:1-f[0])*((k&2)?f[1]:1-f[1])*((k&4)?f[2]:1-f[2]);
        check(std::abs(single.Sample(x,y,z)-reference)<3e-5,"sparse samples match dense reference");
    }
    pair[1].x+=1000;
    check(v.Build(pair,cfg)&&v.BrickCount()==2*single.BrickCount(),"distant sources do not allocate space between them");
    check(v.Sample(500,0,0)==0,"missing bricks sample zero");
    p.x+=20;
    check(v.Build({&p,1},cfg)&&v.Sample(-.25f,.5f,0)==0,"motion removes old bricks");
    const auto before=v.atlas;
    cfg.max_bricks=1;
    check(!v.Build({&p,1},cfg)&&v.atlas==before,"budget failure preserves previous snapshot");
    cfg.max_bricks=4096;cfg.max_voxel_visits=1;
    check(!v.Build({&p,1},cfg)&&v.atlas==before,"work budget enforced transactionally");
    cfg.max_voxel_visits=8'000'000;
    volatile uint32_t nonfiniteBits=0x7fc00000u;
    uint32_t payload=nonfiniteBits;
    std::memcpy(&p.x,&payload,sizeof(payload));
    std::string invalidError;
    const bool acceptedInvalid=v.Build({&p,1},cfg,&invalidError);
    if(acceptedInvalid || v.atlas!=before) std::cerr << "invalid accepted=" << acceptedInvalid << " reason=" << invalidError << " atlas=" << v.atlas.size() << " before=" << before.size() << "\n";
    check(!acceptedInvalid&&v.atlas==before,"nonfinite rejected under fast math");
    check(v.Build({},cfg)&&v.BrickCount()==0&&v.atlas.empty(),"removal reclaims bricks");
    std::cout<<"Sparse volume checks: "<<(errors?"FAIL":"PASS")<<'\n';return errors?1:0;
}
