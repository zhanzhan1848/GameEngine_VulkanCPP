// Native Metal environment-lobe visibility. Scalar four-ray and temporal
// contracts follow the frozen Vulkan reference; no runtime shader translation.
// Unknown geometry contributes zero and retains ownership at composition.
#include <metal_stdlib>
using namespace metal;
#include "GeometryFirstHit.metal"
struct EnvironmentVisibilityParams {
    float4x4 inverseViewProjection;
    float4 camera;
    uint4 dimensions;
    uint4 policy;
    float4 parameters;
    uint4 target;
    float4x4 prevViewProjection;
    uint4 counts; // Native buffer capacities: TLAS, BLAS, triangles, instances.
};
static_assert(sizeof(EnvironmentVisibilityParams)==224,"Metal visibility ABI");
struct Receiver {float3 p;float3 ng;float3 ns;float3 v;float roughness;};
static void store(texture2d<float,access::write> output,int2 pixel,float4 value) {
    output.write(value,uint2(pixel));
}
struct EnvironmentVisibilityContext {
    texture2d<float,access::read> depthTexture, normalTexture, ormTexture;
    texture2d<float,access::write> outputVisibility;
    texture2d<float,access::read> halfVisibility;
    constant EnvironmentVisibilityParams* parameters;
    device const GeometryNode* top;
    device const GeometryNode* bottomNodes;
    device const GeometryTriangle* triangles;
    device const GeometryInstance* instances;
#define cb (*parameters)
int2 traceToSource(int2 tracePixel) {
    int2 sourceSize=int2(cb.dimensions.xy);
    int2 traceSize=int2(cb.dimensions.zw);
    // Integer block ratio. When trace is exactly ceil(source/2) this reduces to
    // the historical `pixel*2+1` texel (including its odd-size clamp), so the
    // coupled configuration keeps bit-identical sampling; a decoupled grid uses
    // the centre of its source footprint instead of a float-projected texel.
    int2 ratio=max(int2(1),(sourceSize+traceSize-int2(1))/traceSize);
    return min(tracePixel*ratio+ratio/2,sourceSize-int2(1));
}
bool finite3(float3 v){return !any(isnan(v))&&!any(isinf(v));}
bool finite1(float v){return !isnan(v)&&!isinf(v);}
uint hash32(uint x){x^=x>>16u;x*=0x7feb352du;x^=x>>15u;x*=0x846ca68bu;return x^(x>>16u);}
float2 random2(uint seed){return (float2(hash32(seed^0x68bc21ebu)>>8u,hash32(seed^0x02e5be93u)>>8u)+.5f)/16777216.f;}
bool pointAt(int2 pixel,thread float3& p) {
    int2 size=int2(cb.dimensions.xy);
    if(any(((pixel)<(int2(0))))||any(((pixel)>=(size))))return false;
    float z=depthTexture.read(uint2(pixel)).r;
    if(!finite1(z)||z<0.f||z>=.999999f)return false;
    float2 uv=(float2(pixel)+.5f)/float2(size);
    // The raster adds jitter before Naga flips gl_Position.y. This screen
    // convention already reverses that flip, so subtract BOTH original axes.
    float2 ndc=float2(uv.x*2.f-1.f,1.f-uv.y*2.f)-float2(cb.camera.w,as_type<float>(cb.policy.z));
    if(any(isnan(ndc))||any(isinf(ndc)))return false;
    float4 h=cb.inverseViewProjection*float4(ndc,z,1.f);
    if(!finite1(h.w)||abs(h.w)<1e-8f)return false;
    p=h.xyz/h.w;return finite3(p);
}

#ifdef ENV_VIS_RESOLVE
// An 8x8 full-resolution group references at most a 6x6 half-source grid.
// Cache the SAME receiver operator once per source; keep target computation,
// candidate iteration order and every qualification/ray predicate unchanged.


#endif
bool receiver(int2 pixel,thread Receiver& r) {
    if((cb.policy.y&0x80000000u)!=0u)return false;
    if(any(((int2(depthTexture.get_width(),depthTexture.get_height()))!=(int2(cb.dimensions.xy))))||
       any(((int2(normalTexture.get_width(),normalTexture.get_height()))!=(int2(cb.dimensions.xy))))||
       any(((int2(ormTexture.get_width(),ormTexture.get_height()))!=(int2(cb.dimensions.xy)))))return false;
    if(!pointAt(pixel,r.p))return false;
    float3 view=cb.camera.xyz-r.p;
    if(!finite3(view)||dot(view,view)<1e-10f)return false;r.v=normalize(view);
    // Choose the closer valid derivative in each axis. Large depth jumps
    // cannot establish a surface tangent and must not supply a bias normal.
    float3 px0,px1,py0,py1;bool x0=pointAt(pixel-int2(1,0),px0),x1=pointAt(pixel+int2(1,0),px1);
    bool y0=pointAt(pixel-int2(0,1),py0),y1=pointAt(pixel+int2(0,1),py1);
    if((!x0&&!x1)||(!y0&&!y1))return false;
    float3 dx=(x0&&(!x1||distance(px0,r.p)<distance(px1,r.p)))?r.p-px0:px1-r.p;
    float3 dy=(y0&&(!y1||distance(py0,r.p)<distance(py1,r.p)))?r.p-py0:py1-r.p;
    float limit=max(.02f,length(view)*.02f);
    if(length(dx)>limit||length(dy)>limit)return false;
    float3 n=cross(dx,dy);float s=max(abs(n.x),max(abs(n.y),abs(n.z)));
    if(!finite3(n)||s<1e-16f)return false;r.ng=normalize(n/s);
    if(dot(r.ng,r.v)<0.f)r.ng=-r.ng;
    float3 ns=normalTexture.read(uint2(pixel)).xyz*2.f-1.f;
    if(!finite3(ns)||dot(ns,ns)<.25f)return false;r.ns=normalize(ns);
    if(dot(r.ns,r.v)<=1e-5f||dot(r.ns,r.ng)<=0.f)return false;
    float rough=ormTexture.read(uint2(pixel)).g;
    if(!finite1(rough)||rough<0.f||rough>1.f)return false;r.roughness=max(rough,.04f);
    return true;
}
// Visibility needs an existential opaque witness, not the nearest triangle.
// The complete/current opaque scene certificate is still required by caller.
// A valid interior witness suffices even if a different branch is irrelevant;
// without such a witness every intersected branch must complete before miss.
bool environmentNodeValid(GeometryNode node) {
    return finite3(node.lo.xyz)&&finite3(node.hi.xyz)&&
        all(((node.lo.xyz)<=(node.hi.xyz)));
}
int environmentAnyHit(float3 origin,float3 direction,float nearT,float farT) {
    if(!finite3(origin)||!finite3(direction)||!finite1(nearT)||!finite1(farT)||
       nearT<0.f||farT<=nearT||dot(direction,direction)<=0.f)return -1;
    bool endpoint=false;
    uint topStack[64];uint topSize=1u,topVisited=0u;topStack[0]=0u;
    while(topSize!=0u) {
        if(topVisited++>=cb.counts.x)return -1;
        uint at=topStack[--topSize];if(at>=cb.counts.x)return -1;
        GeometryNode node=top[at];if(!environmentNodeValid(node))return -1;
        if(!geometryBox(node,origin,direction,nearT,farT))continue;
        if(node.links.w==0u) {
            if(topSize+2u>64u||node.links.x>=cb.counts.x||node.links.y>=cb.counts.x)return -1;
            topStack[topSize++]=node.links.x;topStack[topSize++]=node.links.y;continue;
        }
        if(node.links.w!=1u||node.links.z>=cb.counts.w)return -1;
        GeometryInstance instance=instances[node.links.z];
        if(any(isnan(instance.inverse0))||any(isinf(instance.inverse0))||
           any(isnan(instance.inverse1))||any(isinf(instance.inverse1))||
           any(isnan(instance.inverse2))||any(isinf(instance.inverse2)))return -1;
        float inverseDet=dot(instance.inverse0.xyz,cross(instance.inverse1.xyz,instance.inverse2.xyz));
        if(!finite1(inverseDet)||inverseDet==0.f)return -1;
        float3 localOrigin=float3(dot(instance.inverse0,float4(origin,1)),
            dot(instance.inverse1,float4(origin,1)),dot(instance.inverse2,float4(origin,1)));
        // Preserve WORLD t; inverse-scaled local direction is not normalized.
        float3 localDirection=float3(dot(instance.inverse0.xyz,direction),
            dot(instance.inverse1.xyz,direction),dot(instance.inverse2.xyz,direction));
        float directionSquared=dot(localDirection,localDirection);
        if(!finite3(localOrigin)||!finite3(localDirection)||!finite1(directionSquared)||directionSquared<=0.f)return -1;
        uint bottomStack[64];uint bottomSize=1u,bottomVisited=0u;bottomStack[0]=instance.identity.x;
        while(bottomSize!=0u) {
            if(bottomVisited++>=cb.counts.y)return -1;
            at=bottomStack[--bottomSize];if(at>=cb.counts.y)return -1;
            GeometryNode bottom=bottomNodes[at];if(!environmentNodeValid(bottom))return -1;
            if(!geometryBox(bottom,localOrigin,localDirection,nearT,farT))continue;
            if(bottom.links.w==0u) {
                if(bottomSize+2u>64u||bottom.links.x>=cb.counts.y||bottom.links.y>=cb.counts.y)return -1;
                bottomStack[bottomSize++]=bottom.links.x;bottomStack[bottomSize++]=bottom.links.y;continue;
            }
            if(bottom.links.z>cb.counts.z||bottom.links.w>cb.counts.z-bottom.links.z)return -1;
            for(uint i=0u;i<bottom.links.w;++i) {
                GeometryTriangle triangle=triangles[bottom.links.z+i];
                if(!finite3(triangle.a.xyz)||!finite3(triangle.e1.xyz)||!finite3(triangle.e2.xyz))return -1;
                float3 p=cross(localDirection,triangle.e2.xyz);float det=dot(triangle.e1.xyz,p);
                if(!finite1(det))return -1;if(det==0.f)continue;
                float3 delta=localOrigin-triangle.a.xyz;float u=dot(delta,p)/det;
                if(!finite1(u))return -1;if(u<0.f||u>1.f)continue;
                float3 q=cross(delta,triangle.e1.xyz);float v=dot(localDirection,q)/det;
                if(!finite1(v))return -1;if(v<0.f||u+v>1.f)continue;
                // Same compensated plane solve as the mature closest helper;
                // only run it after containment, then stop at the first proof.
                float3 normal=geometryTriangleNormal(triangle.e1.xyz,triangle.e2.xyz);
                float planeDet=dot(localDirection,normal);
                if(!finite3(normal)||!finite1(planeDet)||planeDet==0.f)return -1;
                float t=dot(triangle.a.xyz-localOrigin,normal)/planeDet;
                if(!finite1(t))return -1;if(t<nearT||t>farT)continue;
                if(t<=nearT+2e-5f||t>=farT-2e-5f){endpoint=true;continue;}
                return 1;
            }
        }
    }
    return endpoint?-1:0;
}
// 0=unknown, 1=known occluded, 2=known complete-scene environment escape.
int environmentRay(Receiver r,float3 direction) {
    if(!finite3(direction))return 0;
    // A shading-normal lobe below the geometric hemisphere sees no exterior.
    if(dot(direction,r.ng)<=0.f)return 1;
    if(cb.policy.x!=1u||cb.counts.x==0||cb.counts.y==0||
       cb.counts.z==0||cb.counts.w==0)return 0;
    GeometryNode root=top[0];
    if(!finite3(root.lo.xyz)||!finite3(root.hi.xyz)||any(((root.lo.xyz)>(root.hi.xyz))))return 0;
    float3 origin=r.p+r.ng*cb.parameters.x;
    float entry=0.f,exit=3.402823466e38f;
    for(uint k=0u;k<3u;++k) {
        if(direction[k]==0.f) {if(origin[k]<root.lo[k]||origin[k]>root.hi[k])return 2;}
        else {
            float a=(root.lo[k]-origin[k])/direction[k],b=(root.hi[k]-origin[k])/direction[k];
            entry=max(entry,min(a,b));exit=min(exit,max(a,b));if(entry>exit)return 2;
        }
    }
    if(!finite1(exit))return 0;if(exit<0.f)return 2;
    // Trace THROUGH the complete root, never call a finite-range clear sky.
    float tmax=exit+max(.001f,abs(exit)*1e-5f);
    int hit=environmentAnyHit(origin,direction,0.f,tmax);
    return hit<0?0:(hit>0?1:2);
}
float3 sampleVisibleGGX(float3 view,float alpha,float2 xi) {
    float3 vh=normalize(float3(alpha*view.xy,view.z));
    float3 t1=vh.z<.99999f?normalize(cross(float3(0,0,1),vh)):float3(1,0,0);
    float3 t2=cross(vh,t1);float radius=sqrt(xi.x),phi=6.28318530718f*xi.y;
    float a=radius*cos(phi),b=radius*sin(phi),s=.5f*(1.f+vh.z);
    b=(1.f-s)*sqrt(max(0.f,1.f-a*a))+s*b;
    float3 nh=a*t1+b*t2+sqrt(max(0.f,1.f-a*a-b*b))*vh;
    return normalize(float3(alpha*nh.xy,max(0.f,nh.z)));
}
float4 traceLobe(Receiver r,int2 pixel,uint firstRay,uint rayCount) {
    if(cb.policy.y==1u)return float4(1,0,1,1); // explicit legacy A/B only
    if(cb.policy.x!=1u)return float4(0,0,1,1);
    float3 t=normalize(cross(abs(r.ns.z)<.999f?float3(0,0,1):float3(1,0,0),r.ns));
    float3 b=cross(r.ns,t);float3 v=float3(dot(r.v,t),dot(r.v,b),dot(r.v,r.ns));
    uint count=r.roughness<=.06f?1u:4u;float visible=0.f,known=0.f,total=0.f;
    bool anyUnknown=false,anyEscape=false,allEscape=true;
    float alpha=r.roughness*r.roughness;
    uint seed=uint(pixel.x)*0x9e3779b9u^uint(pixel.y)*0x85ebca6bu;
    float2 rotation=random2(seed);
    for(uint i=firstRay;i<count && i<firstRay+rayCount;++i) {
        float3 localL;float weight=1.f;
        if(count==1u)localL=float3(-v.xy,v.z);
        else {
            // Four stratified VNDF draws; fixed per receiver pixel avoids
            // stochastic flicker without silently adding temporal history.
            float2 xi=fract(float2((float(i)+.5f)/4.f,float((i&1u)*2u+(i>>1u))*.25f+.125f)+rotation);
            float3 h=sampleVisibleGGX(v,alpha,xi);localL=reflect(-v,h);
            if(localL.z<=0.f)continue;
            // VNDF BRDF*NdotL/pdf reduces to Smith G1(L) for F=1.f
            // This is the geometric GGX lobe, not material Fresnel radiance.
            float c=localL.z;weight=2.f*c/(c+sqrt(alpha*alpha+(1.f-alpha*alpha)*c*c));
        }
        float3 worldL=normalize(t*localL.x+b*localL.y+r.ns*localL.z);
        int outcome=environmentRay(r,worldL);
        total+=weight;
        anyUnknown=anyUnknown || outcome==0;
        anyEscape=anyEscape || outcome==2;
        allEscape=allEscape && outcome==2;
        if(outcome!=0)known+=weight;if(outcome==2)visible+=weight;
    }
    if(total<=0.f)return float4(0,1,0,1);
    // Preserve categorical endpoints exactly: backend reciprocal/division
    // approximations must not turn all-escape/all-known into an FP16 step
    // below one, nor infer validity from comparing accumulated float sums.
    float visibility=allEscape?1.f:(anyEscape?clamp(visible/total,0.f,1.f):0.f);
    float knownFraction=anyUnknown?clamp(known/total,0.f,1.f):1.f;
    return float4(visibility,knownFraction,anyUnknown?1.f:0.f,1.f);
}
float4 mirrorFallback(Receiver r) {
    if(cb.policy.y==1u)return float4(1,0,2,1);
    if(cb.policy.x!=1u)return float4(0,0,3,1);
    int result=environmentRay(r,normalize(reflect(-r.v,r.ns)));
    // B=2 is a KNOWN one-ray fallback, B=3 an UNKNOWN fallback attempt.
    // G describes only this single measurement, never four VNDF samples.
    return float4(result==2?1.f:0.f,result!=0?1.f:0.f,result!=0?2.f:3.f,1.f);
}
void run(int2 pixel, uint2 group, uint2 local,
             threadgroup Receiver* sourceReceiverCache, threadgroup uint* sourceReceiverValid) {
#ifdef ENV_VIS_RESOLVE
    int2 cacheOrigin=int2(group)*4-int2(1);
    uint lane=local.y*8u+local.x;
    if(lane<36u) {
        int2 halfPixel=cacheOrigin+int2(int(lane%6u),int(lane/6u));
        bool valid=all(((halfPixel)>=(int2(0))))&&
            all(((halfPixel)<(int2(cb.dimensions.zw))));
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
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if(any(((pixel)>=(int2(cb.target.xy)))))return;
    // receiver()/pointAt() take GBuffer SOURCE texel coordinates
    // (texelFetch(depthTexture,pixel) normalised by dimensions.xy). The resolve
    // runs on the target grid, which is 2*divisor*render_scale times coarser, so
    // the target receiver must be mapped back to its source texel first. When
    // target == source this is exactly the identity, which is why the coupled
    // configuration never needed it; without it every gather gate fails and
    // every pixel falls through to a full-scene mirrorFallback ray traced from
    // the wrong surface - visible as IBL speckle on dark glossy surfaces.
    int2 targetSource=clamp(int2((float2(pixel)+.5f)/float2(cb.target.xy)*float2(cb.dimensions.xy)),
                             int2(0),int2(cb.dimensions.xy)-int2(1));
    Receiver target;if(!receiver(targetSource,target)){store(outputVisibility,pixel,float4(0,0,1,0));return;}
    float best=3.402823466e38f;float4 result=float4(0);bool found=false;
    // target.w bit0: weighted blend of every qualified neighbour instead of
    // nearest-only. Nearest selection makes the scalar piecewise constant on a
    // trace grid that is (2*divisor*render_scale)x coarser than the GBuffer, so
    // any per-source flip between "environment visible" and "occluded" turns
    // whole blocks of shading on/off - visible as bands of IBL colour on dark
    // glossy surfaces. Blending keeps the same samples but removes the step.
    const bool blendQualified=(cb.target.w&1u)!=0u;
    float4 accum=float4(0);float accumWeight=0.f;
    int2 base=max((pixel-int2(1))/2,int2(0));
    for(int y=0;y<=1;++y)for(int x=0;x<=1;++x) {
        int2 h=base+int2(x,y);if(any(((h)<(int2(0))))||any(((h)>=(int2(cb.dimensions.zw)))))continue;
        int2 cachePixel=h-cacheOrigin;
        if(any(((cachePixel)<(int2(0))))||any(((cachePixel)>=(int2(6)))))continue;
        uint cacheIndex=uint(cachePixel.y*6+cachePixel.x);
        if(sourceReceiverValid[cacheIndex]==0u)continue;
        Receiver source=sourceReceiverCache[cacheIndex];
        float3 delta=source.p-target.p;
        float nsGate=mix(.9995f,.9f,target.roughness*target.roughness);
        if(dot(source.ng,target.ng)<cb.parameters.z||dot(source.ns,target.ns)<nsGate||
           abs(source.roughness-target.roughness)>cb.parameters.w||
           abs(dot(delta,target.ng))>cb.parameters.y||abs(dot(delta,source.ng))>cb.parameters.y)continue;
        float key=dot(delta,delta);
        if(key>max(.0025f,dot(cb.camera.xyz-target.p,cb.camera.xyz-target.p)*.000025f))continue;
        float4 value=halfVisibility.read(uint2(h));
        if(any(isnan(value))||any(isinf(value))||value.a<.5f)continue;
        if(key<best){best=key;result=value;found=true;}
        if(blendQualified){float w=1.f/(1e-3f+key);accum+=value*w;accumWeight+=w;}
    }
    if(blendQualified&&found&&accumWeight>0.f)result=accum/accumWeight;
    // Trace-pixel fallback was measured to add 1.16f ms with no change to the
    // reported speckle (same number of rays, different origin), so the target
    // receiver is kept.
    store(outputVisibility,pixel,found?result:mirrorFallback(target));
#else
    if(any(((pixel)>=(int2(cb.dimensions.zw)))))return;
    int2 source=traceToSource(pixel);
    Receiver r;
    if(!receiver(source,r)){store(outputVisibility,pixel,float4(0,0,1,0));return;}
    // Temporal single-ray mode (opt-in per dispatch): one stratum per frame,
    // accumulated over the 3-slot ring. Off by default so every existing
    // contract test keeps the deterministic four-sample output byte-identical.
    if((cb.target.z&1u)==0u){store(outputVisibility,pixel,traceLobe(r,source,0u,4u));return;}
    float4 current=traceLobe(r,source,cb.policy.w&3u,1u);
    float4 result=current;
    // Warm up for a full ring before trusting history, and reject every sample
    // whose reprojected source texel does not resolve to the same world point
    // (disocclusion, camera cut, moving occluder).
    if(cb.policy.w>=4u) {
        float4 clip=cb.prevViewProjection*float4(r.p,1.f);
        if(finite1(clip.w)&&clip.w>1e-6f) {
            float2 ndc=clip.xy/clip.w;
            float2 uv=float2(ndc.x*.5f+.5f,.5f-ndc.y*.5f);
            int2 hp=int2(uv*float2(cb.dimensions.zw));
            if(all(((hp)>=(int2(0))))&&all(((hp)<(int2(cb.dimensions.zw))))) {
                float4 history=halfVisibility.read(uint2(hp));
                Receiver hr;
                bool compatible=history.a>=.5f&&finite3(history.rgb)&&finite1(history.r)&&
                    receiver(traceToSource(hp),hr)&&
                    distance(hr.p,r.p)<=max(cb.parameters.x,length(cb.camera.xyz-r.p)*.02f);
                if(compatible)result=mix(history,current,.25f);
            }
        }
    }
    store(outputVisibility,pixel,result);
#endif
}

#undef cb
};
#ifdef ENV_VIS_RESOLVE
kernel void environment_visibility_resolve(
#else
kernel void environment_visibility_trace(
#endif
    texture2d<float,access::read> depth [[texture(0)]],
    texture2d<float,access::read> normal [[texture(1)]],
    texture2d<float,access::read> orm [[texture(2)]],
    texture2d<float,access::write> output [[texture(3)]],
    texture2d<float,access::read> history [[texture(5)]],
    constant EnvironmentVisibilityParams& params [[buffer(4)]],
    device const GeometryNode* top [[buffer(13)]],
    device const GeometryNode* bottom [[buffer(14)]],
    device const GeometryTriangle* triangles [[buffer(15)]],
    device const GeometryInstance* instances [[buffer(16)]],
    uint2 pixel [[thread_position_in_grid]],
    uint2 group [[threadgroup_position_in_grid]],
    uint2 local [[thread_position_in_threadgroup]]) {
    threadgroup Receiver cache[36];
    threadgroup uint valid[36];
    EnvironmentVisibilityContext context{depth,normal,orm,output,history,&params,top,bottom,triangles,instances};
    context.run(int2(pixel),group,local,cache,valid);
}
