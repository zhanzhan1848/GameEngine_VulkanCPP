// Trace and reconstruct the scalar reflection-lobe visibility in source texel space.
#include "RawBufferDimensions.hlsli"
#include "../Common/TextureDimensions.hlsli"
#if defined(ENV_TRACE_WG32) && !defined(ENV_VIS_RESOLVE)
#define ENV_GROUP_HEIGHT 4
#else
#define ENV_GROUP_HEIGHT 8
#endif
Texture2D<float4> depthTexture : register(t0, space0);
Texture2D<float4> normalTexture : register(t1, space0);
Texture2D<float4> ormTexture : register(t2, space0);
RWTexture2D<float4> outputVisibility : register(u3, space0);
Texture2D<float4> halfVisibility : register(t5, space0);
cbuffer VisibilityCB : register(b4, space0)
{
    row_major float4x4 inverseViewProjection;
    float4 camera; // xyz position, w actual original clip/NDC jitter.x
    uint4 dimensions; // source.xy (GBuffer texel size), trace.zw
    uint4 policy; // scene ready, legacy bit0 / jitter invalid bit31, jitter.y bits, frame
    float4 parameters; // geometry bias, plane qualification, Ng cosine, rough diff
    uint4 targetDimensions; // resolve/targetDimensions.xy; z bit0 = temporal single-ray, trace size is ceil(target/2)
    row_major float4x4 prevViewProjection; // unjittered previous-frame proj*view, history reprojection
};
ByteAddressBuffer geometryTop : register(t13, space0);
ByteAddressBuffer geometryBottom : register(t14, space0);
ByteAddressBuffer geometryTriangles : register(t15, space0);
ByteAddressBuffer geometryInstances : register(t16, space0);
#include "GeometryFirstHit.hlsli"
// The GI pixel budget is a function of the logical viewport, not of the render
// target: render_scale is a raster supersampling knob, and letting it multiply
// the per-pixel GI rays spends four times the frame on redundant rays.
// dimensions.xy stays the GBuffer (source) texel size so pointAt/receiver keep
// working in source texels, dimensions.zw is the trace grid and targetDimensions.xy is
// the resolve output. At the historical 2x ratio this mapping is exactly the
// old `pixel*2+1` block centre.
int2 traceToSource(int2 tracePixel) {
    int2 sourceSize=(int2)(dimensions.xy);
    int2 traceSize=(int2)(dimensions.zw);
    // Integer block ratio. When trace is exactly ceil(source/2) this reduces to
    // the historical `pixel*2+1` texel (including its odd-size clamp), so the
    // coupled configuration keeps bit-identical sampling; a decoupled grid uses
    // the centre of its source footprint instead of a float-projected texel.
    int2 ratio=max((int2)(1),(sourceSize+traceSize-(int2)(1))/traceSize);
    return min(tracePixel*ratio+ratio/2,sourceSize-(int2)(1));
}
bool finite3(float3 v){return !any(isnan(v))&&!any(isinf(v));}
bool finite1(float v){return !isnan(v)&&!isinf(v);}
uint hash32(uint x){x^=x>>16u;x*=0x7feb352du;x^=x>>15u;x*=0x846ca68bu;return x^(x>>16u);}
float2 random2(uint seed){return (float2(hash32(seed^0x68bc21ebu)>>8u, hash32(seed^0x02e5be93u)>>8u)+.5)/16777216.;}
bool pointAt(int2 pixel,out float3 p) {
    p = 0;
    int2 size=(int2)(dimensions.xy);
    if(any(((pixel) < ((int2)(0))))||any(((pixel) >= (size))))return false;
    float z=depthTexture.Load(int3(pixel, 0)).r;
    if(!finite1(z)||z<0.||z>=.999999)return false;
    float2 uv=((float2)(pixel)+.5)/(float2)(size);
    // The raster adds jitter before Naga flips gl_Position.y. This screen
    // convention already reverses that flip, so subtract BOTH original axes.
    float2 ndc=float2(uv.x*2.-1., 1.-uv.y*2.)-float2(camera.w, asfloat(policy.z));
    if(any(isnan(ndc))||any(isinf(ndc)))return false;
    float4 h=mul(float4(ndc, z, 1.), inverseViewProjection);
    if(!finite1(h.w)||abs(h.w)<1e-8)return false;
    p=h.xyz/h.w;return finite3(p);
}
struct Receiver {float3 p;float3 ng;float3 ns;float3 v;float roughness;};
#ifdef ENV_VIS_RESOLVE
// An 8x8 full-resolution group references at most a 6x6 half-source grid.
// Cache the SAME receiver operator once per source; keep target computation,
// candidate iteration order and every qualification/ray predicate unchanged.
groupshared Receiver sourceReceiverCache[36];
groupshared uint sourceReceiverValid[36];
#endif
bool receiver(int2 pixel,out Receiver r) {
    r = (Receiver)0;
    if((policy.y&0x80000000u)!=0u)return false;
    if(any(((textureDimensions(depthTexture, 0)) != ((int2)(dimensions.xy))))||
       any(((textureDimensions(normalTexture, 0)) != ((int2)(dimensions.xy))))||
       any(((textureDimensions(ormTexture, 0)) != ((int2)(dimensions.xy)))))return false;
    if(!pointAt(pixel,r.p))return false;
    float3 view=camera.xyz-r.p;
    if(!finite3(view)||dot(view,view)<1e-10)return false;r.v=normalize(view);
    // Choose the closer valid derivative in each axis. Large depth jumps
    // cannot establish a surface tangent and must not supply a bias normal.
    float3 px0,px1,py0,py1;bool x0=pointAt(pixel-int2(1, 0),px0),x1=pointAt(pixel+int2(1, 0),px1);
    bool y0=pointAt(pixel-int2(0, 1),py0),y1=pointAt(pixel+int2(0, 1),py1);
    if((!x0&&!x1)||(!y0&&!y1))return false;
    float3 dx=(x0&&(!x1||distance(px0,r.p)<distance(px1,r.p)))?r.p-px0:px1-r.p;
    float3 dy=(y0&&(!y1||distance(py0,r.p)<distance(py1,r.p)))?r.p-py0:py1-r.p;
    float limit=max(.02,length(view)*.02);
    if(length(dx)>limit||length(dy)>limit)return false;
    float3 n=cross(dx,dy);float s=max(abs(n.x),max(abs(n.y),abs(n.z)));
    if(!finite3(n)||s<1e-16)return false;r.ng=normalize(n/s);
    if(dot(r.ng,r.v)<0.)r.ng=-r.ng;
    float3 ns=normalTexture.Load(int3(pixel, 0)).xyz*2.-1.;
    if(!finite3(ns)||dot(ns,ns)<.25)return false;r.ns=normalize(ns);
    if(dot(r.ns,r.v)<=1e-5||dot(r.ns,r.ng)<=0.)return false;
    float rough=ormTexture.Load(int3(pixel, 0)).g;
    if(!finite1(rough)||rough<0.||rough>1.)return false;r.roughness=max(rough,.04);
    return true;
}
// Visibility needs an existential opaque witness, not the nearest triangle.
// The complete/current opaque scene certificate is still required by caller.
// A valid interior witness suffices even if a different branch is irrelevant;
// without such a witness every intersected branch must complete before miss.
bool environmentNodeValid(GeometryNode node) {
    return finite3(node.lo.xyz)&&finite3(node.hi.xyz)&&
        all(((node.lo.xyz) <= (node.hi.xyz)));
}
int environmentAnyHit(float3 origin,float3 direction,float nearT,float farT) {
    if(!finite3(origin)||!finite3(direction)||!finite1(nearT)||!finite1(farT)||
       nearT<0.||farT<=nearT||dot(direction,direction)<=0.)return -1;
    bool endpoint=false;
    uint topStack[64];uint topSize=1u,topVisited=0u;topStack[0]=0u;
    while(topSize!=0u) {
        if(topVisited++>=(byteSize(geometryTop) / 48u))return -1;
        uint at=topStack[--topSize];if(at>=(byteSize(geometryTop) / 48u))return -1;
        GeometryNode node=geometryTop.Load<GeometryNode>((at) * 48u);if(!environmentNodeValid(node))return -1;
        if(!geometryBox(node,origin,direction,nearT,farT))continue;
        if(node.links.w==0u) {
            if(topSize+2u>64u||node.links.x>=(byteSize(geometryTop) / 48u)||node.links.y>=(byteSize(geometryTop) / 48u))return -1;
            topStack[topSize++]=node.links.x;topStack[topSize++]=node.links.y;continue;
        }
        if(node.links.w!=1u||node.links.z>=(byteSize(geometryInstances) / 64u))return -1;
        GeometryInstance instance=geometryInstances.Load<GeometryInstance>((node.links.z) * 64u);
        if(any(isnan(instance.inverse0))||any(isinf(instance.inverse0))||
           any(isnan(instance.inverse1))||any(isinf(instance.inverse1))||
           any(isnan(instance.inverse2))||any(isinf(instance.inverse2)))return -1;
        float inverseDet=dot(instance.inverse0.xyz,cross(instance.inverse1.xyz,instance.inverse2.xyz));
        if(!finite1(inverseDet)||inverseDet==0.)return -1;
        float3 localOrigin=float3(dot(instance.inverse0,float4(origin, 1)), dot(instance.inverse1,float4(origin, 1)), dot(instance.inverse2,float4(origin, 1)));
        // Preserve WORLD t; inverse-scaled local direction is not normalized.
        float3 localDirection=float3(dot(instance.inverse0.xyz,direction), dot(instance.inverse1.xyz,direction), dot(instance.inverse2.xyz,direction));
        float directionSquared=dot(localDirection,localDirection);
        if(!finite3(localOrigin)||!finite3(localDirection)||!finite1(directionSquared)||directionSquared<=0.)return -1;
        uint bottomStack[64];uint bottomSize=1u,bottomVisited=0u;bottomStack[0]=instance.identity.x;
        while(bottomSize!=0u) {
            if(bottomVisited++>=(byteSize(geometryBottom) / 48u))return -1;
            at=bottomStack[--bottomSize];if(at>=(byteSize(geometryBottom) / 48u))return -1;
            GeometryNode bottom=geometryBottom.Load<GeometryNode>((at) * 48u);if(!environmentNodeValid(bottom))return -1;
            if(!geometryBox(bottom,localOrigin,localDirection,nearT,farT))continue;
            if(bottom.links.w==0u) {
                if(bottomSize+2u>64u||bottom.links.x>=(byteSize(geometryBottom) / 48u)||bottom.links.y>=(byteSize(geometryBottom) / 48u))return -1;
                bottomStack[bottomSize++]=bottom.links.x;bottomStack[bottomSize++]=bottom.links.y;continue;
            }
            if(bottom.links.z>(byteSize(geometryTriangles) / 48u)||bottom.links.w>(byteSize(geometryTriangles) / 48u)-bottom.links.z)return -1;
            for(uint i=0u;i<bottom.links.w;++i) {
                GeometryTriangle triangleValue=geometryTriangles.Load<GeometryTriangle>((bottom.links.z+i) * 48u);
                if(!finite3(triangleValue.a.xyz)||!finite3(triangleValue.e1.xyz)||!finite3(triangleValue.e2.xyz))return -1;
                float3 p=cross(localDirection,triangleValue.e2.xyz);float det=dot(triangleValue.e1.xyz,p);
                if(!finite1(det))return -1;if(det==0.)continue;
                float3 delta=localOrigin-triangleValue.a.xyz;float u=dot(delta,p)/det;
                if(!finite1(u))return -1;if(u<0.||u>1.)continue;
                float3 q=cross(delta,triangleValue.e1.xyz);float v=dot(localDirection,q)/det;
                if(!finite1(v))return -1;if(v<0.||u+v>1.)continue;
                // Same compensated plane solve as the mature closest helper;
                // only run it after containment, then stop at the first proof.
                float3 normal=geometryTriangleNormal(triangleValue.e1.xyz,triangleValue.e2.xyz);
                float planeDet=dot(localDirection,normal);
                if(!finite3(normal)||!finite1(planeDet)||planeDet==0.)return -1;
                float t=dot(triangleValue.a.xyz-localOrigin,normal)/planeDet;
                if(!finite1(t))return -1;if(t<nearT||t>farT)continue;
                if(t<=nearT+2e-5||t>=farT-2e-5){endpoint=true;continue;}
                return 1;
            }
        }
    }
    return endpoint?-1:0;
}
#if defined(ENV_VIS_PAIR) && !defined(ENV_VIS_RESOLVE)
#include "EnvironmentVisibilityPair.hlsli"
#endif
// 0=unknown, 1=known occluded, 2=known complete-scene environment escape.
int environmentRay(Receiver r,float3 direction) {
    if(!finite3(direction))return 0;
    // A shading-normal lobe below the geometric hemisphere sees no exterior.
    if(dot(direction,r.ng)<=0.)return 1;
    if(policy.x!=1u||(byteSize(geometryTop) / 48u)==0||(byteSize(geometryBottom) / 48u)==0||
       (byteSize(geometryTriangles) / 48u)==0||(byteSize(geometryInstances) / 64u)==0)return 0;
    GeometryNode root=geometryTop.Load<GeometryNode>((0) * 48u);
    if(!finite3(root.lo.xyz)||!finite3(root.hi.xyz)||any(root.lo.xyz > root.hi.xyz))return 0;
    float3 origin=r.p+r.ng*parameters.x;
    float entry=0.,exit=3.402823466e38;
    for(uint k=0u;k<3u;++k) {
        if(direction[k]==0.) {if(origin[k]<root.lo[k]||origin[k]>root.hi[k])return 2;}
        else {
            float a=(root.lo[k]-origin[k])/direction[k],b=(root.hi[k]-origin[k])/direction[k];
            entry=max(entry,min(a,b));exit=min(exit,max(a,b));if(entry>exit)return 2;
        }
    }
    if(!finite1(exit))return 0;if(exit<0.)return 2;
    // Trace THROUGH the complete root, never call a finite-range clear sky.
    float tmax=exit+max(.001,abs(exit)*1e-5);
    int hit=environmentAnyHit(origin,direction,0.,tmax);
    return hit<0?0:(hit>0?1:2);
}
#if defined(ENV_VIS_PAIR) && !defined(ENV_VIS_RESOLVE)
// Return -1 only when the original ray would enter environmentAnyHit.
// Other values are the exact original early outcome and are not traced.
int environmentPairPrepare(Receiver r,float3 direction,out float3 origin,out float tmax) {
    origin = 0; tmax = 0;
    if(!finite3(direction))return 0;
    if(dot(direction,r.ng)<=0.)return 1;
    if(policy.x!=1u||(byteSize(geometryTop) / 48u)==0||(byteSize(geometryBottom) / 48u)==0||(byteSize(geometryTriangles) / 48u)==0||(byteSize(geometryInstances) / 64u)==0)return 0;
    GeometryNode root=geometryTop.Load<GeometryNode>((0) * 48u);
    if(!finite3(root.lo.xyz)||!finite3(root.hi.xyz)||any(root.lo.xyz > root.hi.xyz))return 0;
    origin=r.p+r.ng*parameters.x;float entry=0.,exit=3.402823466e38;
    for(uint k=0u;k<3u;++k){if(direction[k]==0.){if(origin[k]<root.lo[k]||origin[k]>root.hi[k])return 2;}
      else {float a=(root.lo[k]-origin[k])/direction[k],b=(root.hi[k]-origin[k])/direction[k];entry=max(entry,min(a,b));exit=min(exit,max(a,b));if(entry>exit)return 2;}}
    if(!finite1(exit))return 0;if(exit<0.)return 2;tmax=exit+max(.001,abs(exit)*1e-5);
    // environmentAnyHit's own original entry validation returns unknown.
    if(!finite3(origin)||!finite3(direction)||!finite1(tmax)||tmax<=0.||dot(direction,direction)<=0.)return 0;
    return -1;
}
#endif
float3 sampleVisibleGGX(float3 view,float alpha,float2 xi) {
    float3 vh=normalize(float3(alpha*view.xy, view.z));
    float3 t1=vh.z<.99999?normalize(cross(float3(0, 0, 1),vh)):float3(1, 0, 0);
    float3 t2=cross(vh,t1);float radius=sqrt(xi.x),phi=6.28318530718*xi.y;
    float a=radius*cos(phi),b=radius*sin(phi),s=.5*(1.+vh.z);
    b=(1.-s)*sqrt(max(0.,1.-a*a))+s*b;
    float3 nh=a*t1+b*t2+sqrt(max(0.,1.-a*a-b*b))*vh;
    return normalize(float3(alpha*nh.xy, max(0.,nh.z)));
}
float4 traceLobe(Receiver r,int2 pixel,uint firstRay,uint rayCount) {
    if(policy.y==1u)return float4(1, 0, 1, 1); // explicit legacy A/B only
    if(policy.x!=1u)return float4(0, 0, 1, 1);
    float3 t=normalize(cross(abs(r.ns.z)<.999?float3(0, 0, 1):float3(1, 0, 0),r.ns));
    float3 b=cross(r.ns,t);float3 v=float3(dot(r.v,t), dot(r.v,b), dot(r.v,r.ns));
    uint count=r.roughness<=.06?1u:4u;float visible=0.,known=0.,total=0.;
    bool anyUnknown=false,anyEscape=false,allEscape=true;
    float alpha=r.roughness*r.roughness;
    uint seed=uint(pixel.x)*0x9e3779b9u^uint(pixel.y)*0x85ebca6bu;
    float2 rotation=random2(seed);
#if defined(ENV_VIS_PAIR) && !defined(ENV_VIS_RESOLVE)
    bool pendingActive[2]={false,false};uint3 directionBits[2];uint weightBits[2];
    for(uint i=firstRay;i<count && i<firstRay+rayCount;++i) {
        uint lane=i&1u;pendingActive[lane]=false;float3 localL;float weight=1.;bool valid=true;
        if(count==1u)localL=float3(-v.xy, v.z);
        else {
            float2 xi=frac(float2((float(i)+.5)/4., float((i&1u)*2u+(i>>1u))*.25+.125)+rotation);
            float3 h=sampleVisibleGGX(v,alpha,xi);localL=reflect(-v,h);
            if(localL.z<=0.)valid=false;
            if(valid){float c=localL.z;weight=2.*c/(c+sqrt(alpha*alpha+(1.-alpha*alpha)*c*c));}
        }
        if(valid){float3 worldL=normalize(t*localL.x+b*localL.y+r.ns*localL.z);
            directionBits[lane]=asuint(worldL);weightBits[lane]=asuint(weight);pendingActive[lane]=true;}
        if(count==1u || lane==1u || i+1u>=firstRay+rayCount){
            int outcome[2]={0,0};bool needs[2]={false,false};float3 origins[2],dirs[2];float fars[2];
            uint lanes=count==1u?1u:2u;
            for(uint k=0u;k<lanes;++k)if(pendingActive[k]){dirs[k]=asfloat(directionBits[k]);
                if(count==1u)outcome[k]=environmentRay(r,dirs[k]);
                else {int early=environmentPairPrepare(r,dirs[k],origins[k],fars[k]);if(early<0)needs[k]=true;else outcome[k]=early;}}
            if(lanes==2u && needs[0] && needs[1]){EnvironmentPairResult pr;
                if(environmentAnyHitPair(origins,dirs,fars,pr))for(uint k=0u;k<2u;++k)outcome[k]=pr.status[k]<0?0:(pr.status[k]>0?1:2);
                else {outcome[0]=environmentRay(r,dirs[0]);outcome[1]=environmentRay(r,dirs[1]);}}
            else for(uint k=0u;k<lanes;++k)if(needs[k])outcome[k]=environmentRay(r,dirs[k]);
            for(uint k=0u;k<lanes;++k)if(pendingActive[k]){float w=asfloat(weightBits[k]);int result=outcome[k];
                total+=w;anyUnknown=anyUnknown || result==0;anyEscape=anyEscape || result==2;allEscape=allEscape && result==2;
                if(result!=0)known+=w;if(result==2)visible+=w;pendingActive[k]=false;}
        }
    }
#else
    for(uint i=firstRay;i<count && i<firstRay+rayCount;++i) {
        float3 localL;float weight=1.;
        if(count==1u)localL=float3(-v.xy, v.z);
        else {
            // Four stratified VNDF draws; fixed per receiver pixel avoids
            // stochastic flicker without silently adding temporal history.
            float2 xi=frac(float2((float(i)+.5)/4., float((i&1u)*2u+(i>>1u))*.25+.125)+rotation);
            float3 h=sampleVisibleGGX(v,alpha,xi);localL=reflect(-v,h);
            if(localL.z<=0.)continue;
            // VNDF BRDF*NdotL/pdf reduces to Smith G1(L) for F=1.
            // This is the geometric GGX lobe, not material Fresnel radiance.
            float c=localL.z;weight=2.*c/(c+sqrt(alpha*alpha+(1.-alpha*alpha)*c*c));
        }
        float3 worldL=normalize(t*localL.x+b*localL.y+r.ns*localL.z);
        int outcome=environmentRay(r,worldL);
        total+=weight;
        anyUnknown=anyUnknown || outcome==0;
        anyEscape=anyEscape || outcome==2;
        allEscape=allEscape && outcome==2;
        if(outcome!=0)known+=weight;if(outcome==2)visible+=weight;
    }
#endif
    if(total<=0.)return float4(0, 1, 0, 1);
    // Preserve categorical endpoints exactly: backend reciprocal/division
    // approximations must not turn all-escape/all-known into an FP16 step
    // below one, nor infer validity from comparing accumulated float sums.
    float visibility=allEscape?1.:(anyEscape?clamp(visible/total,0.,1.):0.);
    float knownFraction=anyUnknown?clamp(known/total,0.,1.):1.;
    return float4(visibility, knownFraction, anyUnknown?1.:0., 1.);
}
float4 mirrorFallback(Receiver r) {
    if(policy.y==1u)return float4(1, 0, 2, 1);
    if(policy.x!=1u)return float4(0, 0, 3, 1);
    int result=environmentRay(r,normalize(reflect(-r.v,r.ns)));
    // B=2 is a KNOWN one-ray fallback, B=3 an UNKNOWN fallback attempt.
    // G describes only this single measurement, never four VNDF samples.
    return float4(result==2?1.:0., result!=0?1.:0., result!=0?2.:3., 1.);
}
[numthreads(8, ENV_GROUP_HEIGHT, 1)]
void main(uint3 threadID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID) {
    int2 pixel=(int2)(threadID.xy);
#ifdef ENV_VIS_RESOLVE
    int2 cacheOrigin=(int2)(groupID.xy)*4-(int2)(1);
    uint lane=localID.y*8u+localID.x;
    if(lane<36u) {
        int2 halfPixel=cacheOrigin+int2(int(lane%6u), int(lane/6u));
        bool valid=all(((halfPixel) >= ((int2)(0))))&&
            all(((halfPixel) < ((int2)(dimensions.zw))));
        Receiver cached;
        if(valid) {
            int2 samplePixel=traceToSource(halfPixel);
            valid=receiver(samplePixel,cached);
        }
        if(valid)sourceReceiverCache[lane]=cached;
        sourceReceiverValid[lane]=valid?1u:0u;
    }
    // Every lane participates, including dispatch-edge lanes which do not
    // own an output pixel. Early return before this barrier would deadlock.
    GroupMemoryBarrierWithGroupSync();
    if(any(((pixel) >= ((int2)(targetDimensions.xy)))))return;
    // receiver()/pointAt() take GBuffer SOURCE texel coordinates
    // (texelFetch(depthTexture,pixel) normalised by dimensions.xy). The resolve
    // runs on the target grid, which is 2*divisor*render_scale times coarser, so
    // the target receiver must be mapped back to its source texel first. When
    // target == source this is exactly the identity, which is why the coupled
    // configuration never needed it; without it every gather gate fails and
    // every pixel falls through to a full-scene mirrorFallback ray traced from
    // the wrong surface - visible as IBL speckle on dark glossy surfaces.
    int2 targetSource=clamp((int2)(((float2)(pixel)+.5)/(float2)(targetDimensions.xy)*(float2)(dimensions.xy)),
                             (int2)(0),(int2)(dimensions.xy)-(int2)(1));
    Receiver target;if(!receiver(targetSource,target)){outputVisibility[pixel] = float4(0, 0, 1, 0);return;}
    float best=3.402823466e38;float4 result=(float4)(0);bool found=false;
    // targetDimensions.w bit0: weighted blend of every qualified neighbour instead of
    // nearest-only. Nearest selection makes the scalar piecewise constant on a
    // trace grid that is (2*divisor*render_scale)x coarser than the GBuffer, so
    // any per-source flip between "environment visible" and "occluded" turns
    // whole blocks of shading on/off - visible as bands of IBL colour on dark
    // glossy surfaces. Blending keeps the same samples but removes the step.
    const bool blendQualified=(targetDimensions.w&1u)!=0u;
    float4 accum=(float4)(0);float accumWeight=0.;
    int2 base=max((pixel-(int2)(1))/2,(int2)(0));
    for(int y=0;y<=1;++y)for(int x=0;x<=1;++x) {
        int2 h=base+int2(x, y);if(any(((h) < ((int2)(0))))||any(((h) >= ((int2)(dimensions.zw)))))continue;
        int2 cachePixel=h-cacheOrigin;
        if(any(((cachePixel) < ((int2)(0))))||any(((cachePixel) >= ((int2)(6)))))continue;
        uint cacheIndex=uint(cachePixel.y*6+cachePixel.x);
        if(sourceReceiverValid[cacheIndex]==0u)continue;
        Receiver source=sourceReceiverCache[cacheIndex];
        float3 delta=source.p-target.p;
        float nsGate=lerp(.9995,.9,target.roughness*target.roughness);
        if(dot(source.ng,target.ng)<parameters.z||dot(source.ns,target.ns)<nsGate||
           abs(source.roughness-target.roughness)>parameters.w||
           abs(dot(delta,target.ng))>parameters.y||abs(dot(delta,source.ng))>parameters.y)continue;
        float key=dot(delta,delta);
        if(key>max(.0025,dot(camera.xyz-target.p,camera.xyz-target.p)*.000025))continue;
        float4 value=halfVisibility.Load(int3(h, 0));
        if(any(isnan(value))||any(isinf(value))||value.a<.5)continue;
        if(key<best){best=key;result=value;found=true;}
        if(blendQualified){float w=1./(1e-3+key);accum+=value*w;accumWeight+=w;}
    }
    if(blendQualified&&found&&accumWeight>0.)result=accum/accumWeight;
    // Trace-pixel fallback was measured to add 1.16 ms with no change to the
    // reported speckle (same number of rays, different origin), so the target
    // receiver is kept.
    outputVisibility[pixel] = found?result:mirrorFallback(target);
#else
    if(any(((pixel) >= ((int2)(dimensions.zw)))))return;
    int2 source=traceToSource(pixel);
    Receiver r;
    if(!receiver(source,r)){outputVisibility[pixel] = float4(0, 0, 1, 0);return;}
    // Temporal single-ray mode (opt-in per dispatch): one stratum per frame,
    // accumulated over the 3-slot ring. Off by default so every existing
    // contract test keeps the deterministic four-sample output byte-identical.
    if((targetDimensions.z&1u)==0u){outputVisibility[pixel] = traceLobe(r,source,0u,4u);return;}
    float4 current=traceLobe(r,source,policy.w&3u,1u);
    float4 result=current;
    // Warm up for a full ring before trusting history, and reject every sample
    // whose reprojected source texel does not resolve to the same world point
    // (disocclusion, camera cut, moving occluder).
    if(policy.w>=4u) {
        float4 clip=mul(float4(r.p, 1.), prevViewProjection);
        if(finite1(clip.w)&&clip.w>1e-6) {
            float2 ndc=clip.xy/clip.w;
            float2 uv=float2(ndc.x*.5+.5, .5-ndc.y*.5);
            int2 hp=(int2)(uv*(float2)(dimensions.zw));
            if(all(((hp) >= ((int2)(0))))&&all(((hp) < ((int2)(dimensions.zw))))) {
                float4 history=halfVisibility.Load(int3(hp, 0));
                Receiver hr;
                bool compatible=history.a>=.5&&finite3(history.rgb)&&finite1(history.r)&&
                    receiver(traceToSource(hp),hr)&&
                    distance(hr.p,r.p)<=max(parameters.x,length(camera.xyz-r.p)*.02);
                if(compatible)result=lerp(history,current,.25);
            }
        }
    }
    outputVisibility[pixel] = result;
#endif
}
