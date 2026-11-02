#include "Graphics/Utilities/ComputeWorkgroupExperiment.h"
#include <array>
#include <vector>
#include <cstdio>
using namespace primal::graphics::workgroup_experiment;
int main() {
    uint32_t parsed=99;
    if(!Parse(nullptr,true,parsed)||parsed || !Parse("32",false,parsed)||parsed!=32 || Parse("64",false,parsed) || Parse("32x",true,parsed))return 1;
    for(uint32_t w:{1u,7u,8u,9u,15u,17u})for(uint32_t h:{1u,7u,8u,9u,15u,17u})for(uint32_t ly:{4u,8u}) {
        std::vector<unsigned> hits(w*h);
        for(uint32_t gy=0;gy<Groups(h,ly);++gy)for(uint32_t gx=0;gx<Groups(w,8);++gx)
            for(uint32_t y=0;y<ly;++y)for(uint32_t x=0;x<8;++x) {
                uint32_t px=gx*8+x,py=gy*ly+y;if(px<w && py<h)++hits[py*w+px];
            }
        for(auto count:hits)if(count!=1)return 2;
    }
    std::puts("workgroup parsing and odd/even coverage passed");return 0;
}
