// Native measured screen consumer for the frozen geometry-replace contract.
// Retain the legacy entry in this library only for its shared SH/depth helpers.
#define ddgi_gi_gather legacy_ddgi_gi_gather
#include "DDGIGIGather.metal"
#undef ddgi_gi_gather
#include "GeometryFirstHit.metal"
#include "GeometryConnectionQuery.metal"

struct MetalGatherConnection {
    uint4 policy;
    float4 jitter;
    uint4 geometryCounts;
    uint4 supportCounts; // origins, depth probes, flat SH probes, flat sky floats
};
static_assert(sizeof(MetalGatherConnection)==64, "Metal gather connection ABI");

// Match the frozen inverse-VP accumulation order; receiver drift can flip
// coplanar connection qualification even when the shading position looks equal.
static float4 gatherTransform(float4x4 m, float4 v) {
    return fma(m[2],float4(v.z),fma(m[1],float4(v.y),
        fma(m[0],float4(v.x),m[3]*v.w)));
}

static bool gatherSurfacePoint(int2 pixel, depth2d<float> depth,
    float4x4 invVP, float2 jitter, thread float3& p) {
    const int2 size(depth.get_width(), depth.get_height());
    if (any(pixel<0) || any(pixel>=size)) return false;
    const float z=depth.read(uint2(pixel));
    if (!isfinite(z) || z<0 || z>=0.999999f) return false;
    // Preserve the fused pixel-center transform used by the frozen path.
    const float2 ndc=fma(float2(pixel)+0.5f,float2(2,-2)*(1.0f/float2(size)),
        float2(-1,1)-jitter);
    const float4 h=gatherTransform(invVP,float4(ndc,z,1));
    if (!all(isfinite(h)) || fabs(h.w)<1e-8f) return false;
    p=h.xyz/h.w;return all(isfinite(p));
}
static bool gatherSurfaceOrigin(int2 pixel, depth2d<float> depth,
    float4x4 invVP, float3 camera, constant MetalGatherConnection& c,
    thread float3& origin) {
    if(c.policy.x!=1 || !all(isfinite(c.jitter.xy)))return false;
    float3 p;if(!gatherSurfacePoint(pixel,depth,invVP,c.jitter.xy,p))return false;
    float3 v=camera-p;if(!all(isfinite(v)) || dot(v,v)<1e-10f)return false;
    float3 a,b,y,z;
    bool ax=gatherSurfacePoint(pixel-int2(1,0),depth,invVP,c.jitter.xy,a);
    bool bx=gatherSurfacePoint(pixel+int2(1,0),depth,invVP,c.jitter.xy,b);
    bool cy=gatherSurfacePoint(pixel-int2(0,1),depth,invVP,c.jitter.xy,y);
    bool dy=gatherSurfacePoint(pixel+int2(0,1),depth,invVP,c.jitter.xy,z);
    if((!ax&&!bx)||(!cy&&!dy))return false;
    float3 dx=ax&&(!bx||distance(a,p)<distance(b,p))?p-a:b-p;
    float3 ddy=cy&&(!dy||distance(y,p)<distance(z,p))?p-y:z-p;
    const float limit=max(0.02f,length(v)*0.02f);
    if(length(dx)>limit||length(ddy)>limit)return false;
    float3 n=cross(dx,ddy);float scale=max(max(fabs(n.x),fabs(n.y)),fabs(n.z));
    if(!all(isfinite(n))||scale<1e-16f)return false;
    n=normalize(n/scale);if(dot(n,v)<0)n=-n;
    origin=p+n*0.002f;return all(isfinite(origin));
}
static int gatherConnection(float3 receiver,float3 probe,
    constant MetalGatherConnection& c,
    device const GeometryNode* top,device const GeometryNode* bottom,
    device const GeometryTriangle* triangles,device const GeometryInstance* instances) {
    if(c.policy.x!=1 || any(c.geometryCounts==uint4(0)) ||
       !all(isfinite(receiver)) || !all(isfinite(probe)))return 0;
    const float3 delta=probe-receiver;const float dist=length(delta),eps=2e-5f;
    if(!isfinite(dist)||dist<=2*eps)return 0;
    if(any((as_type<uint3>(receiver)&uint3(0x7f800000u))>=uint3(132u<<23u))||
       any((as_type<uint3>(probe)&uint3(0x7f800000u))>=uint3(132u<<23u)))return 0;
    const float3 direction=delta/dist,start=receiver-direction*eps,end=probe+direction*eps;
    const float farT=dist+2*eps,extension=farT-dist;
    const float3 startShift=receiver-start,endShift=end-probe;
    const float startMove=dot(startShift,direction),endMove=dot(endShift,direction);
    const float startSide=length(startShift-startMove*direction),endSide=length(endShift-endMove*direction);
    if(!all(isfinite(direction))||!all(isfinite(start))||!all(isfinite(end))||!isfinite(farT)||
       !isfinite(extension)||extension<eps*1.8f||extension>eps*2.2f||
       !isfinite(startSide)||!isfinite(endSide)||startSide>eps*0.1f||endSide>eps*0.1f||
       !isfinite(startMove)||!isfinite(endMove)||startMove<eps*0.9f||startMove>eps*1.1f||
       endMove<eps*0.9f||endMove>eps*1.1f)return 0;
    GeometryHit hit;
    const int status=geometryConnectionFirstHit(start,direction,0,farT,top,bottom,triangles,instances,c.geometryCounts,hit);
    if(status<0)return 0;
    if(status>0){float t=hit.distance_normal.x-eps;return isfinite(t)&&t>eps&&t<dist-eps?-1:0;}
    return 1;
}

kernel void ddgi_gi_gather_geometry(uint2 tid [[thread_position_in_grid]],
    depth2d<float> depth [[texture(0)]],texture2d<float> normals [[texture(1)]],
    texture2d<float,access::write> diffuse [[texture(2)]],texture2d<float> orm [[texture(4)]],
    texture2d<float,access::write> specular [[texture(5)]],
    constant float4* view [[buffer(0)]],constant float4* countsCamera [[buffer(2)]],
    device const float* moments [[buffer(8)]],device const float4* shData [[buffer(10)]],
    constant float4* volumes [[buffer(11)]],device const float4* origins [[buffer(12)]],
    device const float* sky [[buffer(13)]],
    device const GeometryNode* top [[buffer(14)]],device const GeometryNode* bottom [[buffer(15)]],
    device const GeometryTriangle* triangles [[buffer(16)]],device const GeometryInstance* instances [[buffer(17)]],
    constant MetalGatherConnection& connection [[buffer(18)]]) {
    const uint2 size(diffuse.get_width(),diffuse.get_height());if(any(tid>=size))return;
    diffuse.write(float4(0),tid);specular.write(float4(0),tid);
    // This entry is deliberately measured-only. Missing data is unknown, never a legacy floor.
    if(countsCamera[1].w!=7.0f || connection.policy.x!=1u)return;
    // Reciprocal multiplication preserves the reference texel at row boundaries.
    const float2 uv=(float2(tid)+0.5f)*(1.0f/float2(size));
    const uint2 full=uint2(uv*float2(depth.get_width(),depth.get_height()));
    const float z=depth.read(full);if(!isfinite(z)||z<0||z>=1)return;
    const float4x4 invVP(view[0],view[1],view[2],view[3]);
    const float4 h=gatherTransform(invVP,float4(uv.x*2-1,1-uv.y*2,z,1));
    if(!all(isfinite(h))||fabs(h.w)<1e-8f)return;
    const float3 world=h.xyz/h.w,camera=countsCamera[1].xyz;
    float3 n=normalize(normals.read(uint2(uv*float2(normals.get_width(),normals.get_height()))).xyz*2-1);
    if(!all(isfinite(n)))n=float3(0,1,0);
    float3 receiver;const bool receiverValid=gatherSurfaceOrigin(int2(full),depth,invVP,camera,connection,receiver);
    int selected=-1;float spacing=0;
    for(int i=0;i<min(int(volumes[8].x),4);++i) {
        const float s=volumes[i].w;if(!(s>0))continue;
        const float3 gp=(world-volumes[i].xyz)/s;
        if(all(gp>=0)&&all(gp<=volumes[4+i].xyz-1)&&(selected<0||s<spacing)){selected=i;spacing=s;}
    }
    if(selected!=0)return; // Far volumes have no measured origins/depth in this contract.
    const float3 dimensions=volumes[4].xyz;
    if(!all(isfinite(dimensions))||any(dimensions<1)||any(dimensions>65535)||any(dimensions!=floor(dimensions)))return;
    const uint3 counts=uint3(dimensions);
    // Bound every multiplication before calculating linear and SH/sky indices.
    if(counts.x>0xffffffffu/counts.y || counts.x*counts.y>0xffffffffu/counts.z)return;
    const uint probeCount=counts.x*counts.y*counts.z;
    const float offsetValue=volumes[4].w;
    if(!isfinite(offsetValue)||offsetValue<0||offsetValue!=floor(offsetValue)||offsetValue>=477218560.0f)return;
    const uint offset=uint(offsetValue);
    if(probeCount>477218588u-offset)return;

    const float3 gp=(world-volumes[0].xyz)/spacing;
    const uint3 b0=min(uint3(floor(gp)),counts-1),b1=min(b0+1,counts-1);
    const float3 f=fract(gp);uint ids[8];float weights[8];
    for(uint i=0;i<8;++i){const uint3 b((i&1)?b1.x:b0.x,(i&2)?b1.y:b0.y,(i&4)?b1.z:b0.z);
        ids[i]=b.x+b.y*counts.x+b.z*counts.x*counts.y;
        weights[i]=((i&1)?f.x:1-f.x)*((i&2)?f.y:1-f.y)*((i&4)?f.z:1-f.z);
    }
    const bool classify=volumes[8].y>0.5f,v2=volumes[8].z>0.5f;
    float pixelOpen=0,pixelW=0;
    if(classify)for(uint i=0;i<8;++i)if(weights[i]>=0.01f){
        const uint at=(offset+ids[i])*2;
        if(at+1>=connection.supportCounts.w)return;
        pixelOpen+=sky[at]*weights[i];pixelW+=weights[i];
    }
    if(pixelW>0)pixelOpen/=pixelW;
    const float3 V=normalize(camera-world),R=reflect(-V,n),biased=world+n*spacing*0.05f;
    const float noV=clamp(dot(n,V),0.05f,1.0f);
    float3 result=0,refl=0;float total=0,confidenceBase=0;
    for(uint i=0;i<8;++i){
        const uint p=ids[i];const float tw=weights[i];if(!(tw>0))continue;
        if(p>=connection.supportCounts.x||p>=connection.supportCounts.y||offset+p>=connection.supportCounts.z)continue;
        float3 sh[9];bool finite=true;
        for(uint j=0;j<9;++j){sh[j]=shData[(offset+p)*9+j].xyz;finite=finite&&all(isfinite(sh[j]));}
        if(!finite)continue;
        float attenuation=1;
        if(classify){uint at=(offset+p)*2;if(at+1>=connection.supportCounts.w)continue;
            float classification=1-smoothstep(0.15f,0.45f,sky[at])*(1-smoothstep(0.05f,0.25f,pixelOpen))*0.85f;
            attenuation=v2?mix(1.0f,classification,sky[at+1]):classification;
        }
        float3 probe=volumes[0].xyz+float3(uint3(p%counts.x,(p/counts.x)%counts.y,p/(counts.x*counts.y)))*spacing;
        float originState=0;
        if(origins[p].w>0.25f&&all(isfinite(origins[p]))){probe=origins[p].xyz;originState=origins[p].w;}
        float3 delta=probe-biased;float dist=length(delta);float3 toProbe=dist>1e-5f?delta/max(dist,1e-5f):n;
        const float nw=max(mix(0.2f,1.0f,clamp((dot(n,toProbe)+0.5f)/0.7f,0.0f,1.0f)),0.40f);
        float2 mv=sampleNearProbeDepth(p,-toProbe,moments);
        float burial=1,inside=1;
        if(mv.x<spacing*0.2f){float t=smoothstep(0.0f,0.2f,mv.x/spacing);inside=mix(0.2f,1.0f,t);burial=mix(0.1f,1.0f,t);}
        confidenceBase+=tw*burial*attenuation;
        const bool qualified=all(isfinite(mv))&&all(mv>=0)&&originState>0.25f;
        const bool clear=qualified&&receiverValid&&gatherConnection(receiver,probe,connection,top,bottom,triangles,instances)==1;
        const float visibility=clear?mix(0.5f,1.0f,step(0.75f,originState)):0;
        const float w=tw*attenuation*nw*visibility*inside;
        result+=evalSH9IrradianceOverPi(sh,n)*w;refl+=evalSH9Canonical(sh,R)*w;total+=w;
    }
    if(!(total>0))return;
    result/=total;refl/=total;result*=clamp(total/max(confidenceBase,1e-4f),0.0f,1.0f);
    const float roughness=clamp(orm.read(uint2(uv*float2(orm.get_width(),orm.get_height()))).g,0.04f,1.0f);
    diffuse.write(float4(max(result*PROBE_SH_RAD_TO_IRRAD,float3(0)),1),tid);
    specular.write(float4(mix(refl,max(result,float3(0)),roughness*roughness),noV),tid);
}
