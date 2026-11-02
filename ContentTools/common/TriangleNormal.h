#pragma once

// Face-normal calculation for finite float positions. Float cross products can
// underflow/overflow and an absolute area cutoff changes with asset scale.
#include <cmath>
#include <cstdint>
#include <cstring>

namespace primal::tools::geometry_detail {
struct TriangleNormal { float x=0.f, y=0.f, z=0.f; };

inline bool FinitePosition(float value) {
    // An integer exponent test remains meaningful under -ffast-math, where
    // std::isfinite can be optimized under an assumed finite-input contract.
    uint32_t copied;std::memcpy(&copied,&value,sizeof(copied));
    volatile uint32_t bits=copied;
    return (bits & 0x7f800000u) != 0x7f800000u;
}

inline bool CalculateTriangleNormal(const float (&a)[3], const float (&b)[3],
                                    const float (&c)[3], TriangleNormal& out) {
    out={};
    const float* points[]={a,b,c};
    for (const float* p : points)
        for (unsigned axis=0;axis<3;++axis)
            if (!FinitePosition(p[axis])) return false;

    // Cyclic rotation preserves winding. Starting at the closest pair reduces
    // cancellation for a very short edge next to very large coordinates.
    double edgeLengthSquared[3]{};
    for (unsigned i=0;i<3;++i) for (unsigned k=0;k<3;++k) {
        const double d=double(points[(i+1)%3][k])-double(points[i][k]);
        edgeLengthSquared[i]+=d*d;
    }
    unsigned first=0;
    if(edgeLengthSquared[1]<edgeLengthSquared[first])first=1;
    if(edgeLengthSquared[2]<edgeLengthSquared[first])first=2;
    const float* p=points[first];const float* q=points[(first+1)%3];const float* r=points[(first+2)%3];
    double u[3],v[3];
    for(unsigned k=0;k<3;++k){u[k]=double(q[k])-double(p[k]);v[k]=double(r[k])-double(p[k]);}
    const double x=u[1]*v[2]-u[2]*v[1];
    const double y=u[2]*v[0]-u[0]*v[2];
    const double z=u[0]*v[1]-u[1]*v[0];
    const double length=std::hypot(x,y,z);
    if(length==0.0)return false;
    out={float(x/length),float(y/length),float(z/length)};
    return true;
}
}
