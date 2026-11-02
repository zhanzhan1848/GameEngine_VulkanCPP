#include "ContentTools/common/TriangleNormal.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <initializer_list>
#include <limits>
using primal::tools::geometry_detail::CalculateTriangleNormal;
using primal::tools::geometry_detail::TriangleNormal;
namespace {
int checks=0, failures=0;
void check(bool condition,const char* name){++checks;if(!condition){++failures;std::cerr<<"FAIL "<<name<<'\n';}}
void expected(const float (&a)[3],const float (&b)[3],const float (&c)[3],
              float x,float y,float z,const char* name){
    TriangleNormal n{};bool ok=CalculateTriangleNormal(a,b,c,n);
    check(ok && std::fabs(n.x-x)<.00005f && std::fabs(n.y-y)<.00005f && std::fabs(n.z-z)<.00005f &&
          std::fabs((n.x*n.x+n.y*n.y+n.z*n.z)-1.f)<.0001f,name);
}
void rejected(const float (&a)[3],const float (&b)[3],const float (&c)[3],const char* name){
    TriangleNormal n{1,2,3};check(!CalculateTriangleNormal(a,b,c,n) && n.x==0 && n.y==0 && n.z==0,name);
}
float fromBits(uint32_t bits){float f;std::memcpy(&f,&bits,sizeof f);return f;}
}
int main(){
    // Runtime triangle coordinates from normal-source.txt; expectations are
    // independent geometric direction calculations, not authored normals.
    const float a0[]={-2.3535709381103516f,4.25299072265625f,1.6286590099334717f};
    const float b0[]={-2.366502046585083f,4.179931640625f,1.6307228803634644f};
    const float c0[]={-2.3865349292755127f,4.240878105163574f,1.6236289739608765f};
    expected(a0,b0,c0,.17142902f,-.05812478f,-.98348035f,"Sponza cloth triangle 1919");
    const float a1[]={-2.6163170337677f,4.175917148590088f,1.6258509159088135f};
    const float b1[]={-2.6159439086914062f,4.2396039962768555f,1.6171048879623413f};
    const float c1[]={-2.5783538818359375f,4.229017734527588f,1.6020100116729736f};
    expected(a1,b1,c1,-.39934002f,-.12243589f,-.90859067f,"Sponza cloth triangle 1455");
    expected(a0,c0,b0,-.17142902f,.05812478f,.98348035f,"winding reverses normal");
    constexpr float unit=.5773502691896258f;
    for(float s:{1e-6f,1e-3f,1.f,1e3f,1e6f}) {
        const float p[]={0,0,0},q[]={s,0,s},r[]={0,s,s};
        expected(p,q,r,-unit,-unit,unit,"scale independent diagonal face");
    }
    const float origin[]={0,0,0},duplicate[]={0,0,0},line[]={1,2,3},line2[]={2,4,6};
    rejected(origin,duplicate,line,"duplicate vertex");rejected(origin,line,line2,"collinear vertices");
    const float nanPoint[]={fromBits(0x7fc00001u),0,0},infPoint[]={0,fromBits(0x7f800000u),0};
    rejected(nanPoint,line,line2,"NaN input even under fast math");
    rejected(origin,infPoint,line,"infinite input even under fast math");
    const float largest=std::numeric_limits<float>::max();
    const float big0[]={-largest,0,0},big1[]={largest,0,0},big2[]={0,largest,0};
    expected(big0,big1,big2,0,0,1,"large finite input avoids float edge overflow");
    const float far[]={1e30f,1e30f,0},near0[]={0,1,0},near1[]={1,0,0};
    expected(far,near0,near1,0,0,1,"short edge preserved next to very large coordinate");
    std::cout<<checks-failures<<'/'<<checks<<" triangle normal checks passed\n";
    return failures?1:0;
}
