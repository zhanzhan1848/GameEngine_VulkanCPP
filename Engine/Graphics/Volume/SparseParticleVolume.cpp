#include "SparseParticleVolume.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <limits>

namespace primal::graphics::volume {
namespace {
bool finite(const float& f) { return (std::bit_cast<uint32_t>(f) & 0x7f800000u) != 0x7f800000u; }
int32_t brick(int32_t v) { return int32_t(std::floor(double(v) / 8.0)); }
using Key = std::array<int32_t, 3>;
}
bool SparseParticleVolume::Build(std::span<const VolumeParticle> particles,
                                const SparseVolumeConfig& c, std::string* error) {
    auto fail = [&](const char* s) { if (error) *error = s; return false; };
    if (!finite(c.voxel_size) || (c.voxel_size < 0.0001f || c.voxel_size > 10000.0f) || c.max_bricks > 32768 ||
        !c.max_bricks || particles.size() > 65536) return fail("invalid sparse volume configuration");
    std::map<Key, std::array<float,512>> bricks;
    uint64_t visits = 0;
    for (const auto& p : particles) {
        if (!finite(p.x) || !finite(p.y) || !finite(p.z) || !finite(p.radius) ||
            !finite(p.density) || p.radius <= 0 || p.density < 0 || p.density > 60000)
            return fail("invalid volume particle");
        if (p.density == 0) continue;
        int32_t lo[3], hi[3];
        const float pos[] = {p.x,p.y,p.z};
        for (int a=0;a<3;++a) {
            double l = std::ceil((double(pos[a])-p.radius)/c.voxel_size - 0.5);
            double h = std::floor((double(pos[a])+p.radius)/c.voxel_size - 0.5);
            if (l < -1000000 || h > 1000000) return fail("particle outside supported voxel coordinates");
            lo[a] = int32_t(l); hi[a] = int32_t(h);
        }
        uint64_t count = 1;
        for (int a=0;a<3;++a) {
            uint64_t n = uint64_t(std::max<int64_t>(0,int64_t(hi[a])-lo[a]+1));
            if (n && count > c.max_voxel_visits/n) return fail("voxel work budget exceeded");
            count *= n;
        }
        if (count > c.max_voxel_visits - visits) return fail("voxel work budget exceeded");
        visits += count;
        for (int z=lo[2];z<=hi[2];++z) for (int y=lo[1];y<=hi[1];++y) for (int x=lo[0];x<=hi[0];++x) {
            double dx=((double(x)+.5)*c.voxel_size-p.x)/p.radius;
            double dy=((double(y)+.5)*c.voxel_size-p.y)/p.radius;
            double dz=((double(z)+.5)*c.voxel_size-p.z)/p.radius;
            double t=1-dx*dx-dy*dy-dz*dz;
            if (t<=0) continue;
            const float contribution=float(p.density*t*t);
            if (contribution<=0) continue;
            Key k{brick(x),brick(y),brick(z)};
            auto it=bricks.find(k);
            if (it==bricks.end()) {
                if (bricks.size()>=c.max_bricks) return fail("brick budget exceeded");
                it=bricks.emplace(k,std::array<float,512>{}).first;
            }
            auto& d=it->second[(z-k[2]*8)*64+(y-k[1]*8)*8+x-k[0]*8];
            d=std::min(60000.0f,d+contribution);
        }
    }
    SparseParticleVolume next;
    next.voxel_size_=c.voxel_size;
    int tiles=0;
    while (uint64_t(tiles)*tiles*tiles < bricks.size()) ++tiles;
    next.directory[0]={int32_t(bricks.size()),tiles,8,0};
    next.directory[1]={std::bit_cast<int32_t>(c.voxel_size),0,0,0};
    int side=tiles*8;
    next.atlas.resize(size_t(side)*side*side,0);
    Key bmin{INT32_MAX,INT32_MAX,INT32_MAX},bmax{INT32_MIN,INT32_MIN,INT32_MIN};
    int tile=0;
    for (const auto& [k,voxels]:bricks) {
        next.directory.push_back({k[0],k[1],k[2],tile});
        for(int a=0;a<3;++a) { bmin[a]=std::min(bmin[a],k[a]);bmax[a]=std::max(bmax[a],k[a]); }
        int tx=tile%tiles,ty=(tile/tiles)%tiles,tz=tile/(tiles*tiles);
        for(int z=0;z<8;++z) for(int y=0;y<8;++y) for(int x=0;x<8;++x)
            next.atlas[((tz*8+z)*side+ty*8+y)*side+tx*8+x]=voxels[z*64+y*8+x];
        ++tile;
    }
    next.directory[2]={bmin[0],bmin[1],bmin[2],0};
    next.directory[3]={bmax[0],bmax[1],bmax[2],0};
    *this=std::move(next);
    if(error) error->clear();
    return true;
}
float SparseParticleVolume::Voxel(int32_t x,int32_t y,int32_t z) const {
    Key key{brick(x),brick(y),brick(z)};
    auto it=std::lower_bound(directory.begin()+4,directory.end(),key,[](const auto& w,const Key& k){return Key{w.x,w.y,w.z}<k;});
    if(it==directory.end() || Key{it->x,it->y,it->z}!=key) return 0;
    int n=directory[0].y,side=n*8,t=it->w;
    int ax=t%n*8+x-key[0]*8,ay=(t/n)%n*8+y-key[1]*8,az=t/(n*n)*8+z-key[2]*8;
    return atlas[(az*side+ay)*side+ax];
}
float SparseParticleVolume::Sample(float x,float y,float z) const {
    if(!finite(x)||!finite(y)||!finite(z)||!BrickCount()) return 0;
    double p[]={double(x)/voxel_size_-.5,double(y)/voxel_size_-.5,double(z)/voxel_size_-.5};
    int v[3];float f[3];
    for(int a=0;a<3;++a) {if(p[a]<-1000001||p[a]>1000001)return 0;v[a]=int(std::floor(p[a]));f[a]=float(p[a]-v[a]);}
    float d=0;
    for(int k=0;k<8;++k) d+=Voxel(v[0]+(k&1),v[1]+((k>>1)&1),v[2]+((k>>2)&1)) *
        ((k&1)?f[0]:1-f[0])*((k&2)?f[1]:1-f[1])*((k&4)?f[2]:1-f[2]);
    return d;
}
}
