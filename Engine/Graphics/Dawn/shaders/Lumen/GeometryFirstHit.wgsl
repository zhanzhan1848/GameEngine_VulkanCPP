// Native WGSL first-surface query. Buffer ABI matches GeometryFirstHit.h.
// This is the geometric stage; materials must qualify the first surface later.
struct GeometryNode { lo:vec4<f32>, hi:vec4<f32>, links:vec4<u32> }
struct GeometryTriangle { a:vec4<f32>, e1:vec4<f32>, e2:vec4<f32> }
struct GeometryInstance { inverse0:vec4<f32>, inverse1:vec4<f32>, inverse2:vec4<f32>, identity:vec4<u32> }
struct GeometryRay { origin_near:vec4<f32>, direction_far:vec4<f32> }
struct GeometryQueryParams { counts:vec4<u32>, rays_ready:vec4<u32> }
struct GeometryHit {
    distance_normal:vec4<f32>, identity:vec4<u32>,
    a:vec4<f32>, e1:vec4<f32>, e2:vec4<f32>, measured:vec4<f32>
}
@group(0) @binding(0) var<storage,read> geometryTop:array<GeometryNode>;
@group(0) @binding(1) var<storage,read> geometryBottom:array<GeometryNode>;
@group(0) @binding(2) var<storage,read> geometryTriangles:array<GeometryTriangle>;
@group(0) @binding(3) var<storage,read> geometryInstances:array<GeometryInstance>;
@group(0) @binding(4) var<storage,read> geometryRays:array<GeometryRay>;
@group(0) @binding(5) var<storage,read_write> geometryHits:array<GeometryHit>;
@group(0) @binding(6) var<uniform> geometryParams:GeometryQueryParams;

// Bit tests do not rely on WGSL arithmetic preserving NaN comparisons.
fn geometryFinite(v:f32)->bool { return (bitcast<u32>(v)&0x7f800000u)!=0x7f800000u; }
fn geometryFinite3(v:vec3<f32>)->bool { return geometryFinite(v.x)&&geometryFinite(v.y)&&geometryFinite(v.z); }
fn geometryDifferenceOfProducts(a:f32,b:f32,c:f32,d:f32)->f32 {
    let cd=c*d;
    return fma(a,b,-cd)+fma(-c,d,cd);
}
fn geometryTriangleNormal(a:vec3<f32>,b:vec3<f32>)->vec3<f32> {
    return vec3<f32>(geometryDifferenceOfProducts(a.y,b.z,a.z,b.y),
        geometryDifferenceOfProducts(a.z,b.x,a.x,b.z),geometryDifferenceOfProducts(a.x,b.y,a.y,b.x));
}
fn geometryBox(node:GeometryNode,o:vec3<f32>,d:vec3<f32>,nearT:f32,farT:f32)->bool {
    var low=nearT;var high=farT;
    for(var k=0u;k<3u;k++) {
        if(d[k]==0.0) { if(o[k]<node.lo[k]||o[k]>node.hi[k]) {return false;} }
        else {
            let a=(node.lo[k]-o[k])/d[k];let b=(node.hi[k]-o[k])/d[k];
            low=max(low,min(a,b));high=min(high,max(a,b));if(low>high){return false;}
        }
    }
    return true;
}
fn geometryBoundsValid(n:GeometryNode)->bool {
    return geometryFinite3(n.lo.xyz)&&geometryFinite3(n.hi.xyz)&&all(n.lo.xyz<=n.hi.xyz);
}
fn geometryUnknown()->GeometryHit {
    var hit:GeometryHit;hit.identity.x=0xffffffffu;hit.identity.z=0xffffffffu;hit.measured.x=-1.0;return hit;
}
fn geometryFirstHit(ray:GeometryRay)->GeometryHit {
    let counts=geometryParams.counts;let o=ray.origin_near.xyz;let d=ray.direction_far.xyz;
    let nearT=ray.origin_near.w;let farT=ray.direction_far.w;
    if(any(counts==vec4<u32>(0u))||counts.x>arrayLength(&geometryTop)||counts.y>arrayLength(&geometryBottom)||
       counts.z>arrayLength(&geometryTriangles)||counts.w>arrayLength(&geometryInstances)||
       !geometryFinite3(o)||!geometryFinite3(d)||!geometryFinite(nearT)||!geometryFinite(farT)||
       nearT<0.0||farT<nearT||dot(d,d)==0.0) {return geometryUnknown();}
    var result:GeometryHit;result.distance_normal.x=farT;result.identity.z=0xffffffffu;
    var bestT=farT;var found=false;
    var ts:array<u32,64>;var tp=1u;var topVisited=0u;ts[0]=0u;
    loop {
        if(tp==0u){break;}
        if(topVisited>=counts.x){return geometryUnknown();}topVisited++;
        tp--;let at=ts[tp];if(at>=counts.x){return geometryUnknown();}
        let n=geometryTop[at];if(!geometryBoundsValid(n)){return geometryUnknown();}
        if(!geometryBox(n,o,d,nearT,bestT)){continue;}
        if(n.links.w==0u){
            if(tp+2u>64u){return geometryUnknown();}
            ts[tp]=n.links.x;tp++;ts[tp]=n.links.y;tp++;continue;
        }
        if(n.links.z>=counts.w){return geometryUnknown();}
        let inst=geometryInstances[n.links.z];
        let localO=vec3<f32>(dot(inst.inverse0,vec4<f32>(o,1.0)),dot(inst.inverse1,vec4<f32>(o,1.0)),dot(inst.inverse2,vec4<f32>(o,1.0)));
        // Preserve world t under nonuniform scale; do not normalize localD.
        let localD=vec3<f32>(dot(inst.inverse0.xyz,d),dot(inst.inverse1.xyz,d),dot(inst.inverse2.xyz,d));
        if(!geometryFinite3(localO)||!geometryFinite3(localD)){return geometryUnknown();}
        var bs:array<u32,64>;var bp=1u;var bottomVisited=0u;bs[0]=inst.identity.x;
        loop {
            if(bp==0u){break;}
            if(bottomVisited>=counts.y){return geometryUnknown();}bottomVisited++;
            bp--;let bi=bs[bp];if(bi>=counts.y){return geometryUnknown();}
            let b=geometryBottom[bi];if(!geometryBoundsValid(b)){return geometryUnknown();}
            if(!geometryBox(b,localO,localD,nearT,bestT)){continue;}
            if(b.links.w==0u){
                if(bp+2u>64u){return geometryUnknown();}
                bs[bp]=b.links.x;bp++;bs[bp]=b.links.y;bp++;continue;
            }
            if(b.links.z>counts.z||b.links.w>counts.z-b.links.z){return geometryUnknown();}
            for(var i=0u;i<b.links.w;i++){
                let tri=geometryTriangles[b.links.z+i];let p=cross(localD,tri.e2.xyz);let det=dot(tri.e1.xyz,p);
                if(det==0.0){continue;}
                let v=localO-tri.a.xyz;let u=dot(v,p)/det;if(u<0.0||u>1.0){continue;}
                let q=cross(v,tri.e1.xyz);let bary=dot(localD,q)/det;
                let normal=geometryTriangleNormal(tri.e1.xyz,tri.e2.xyz);let planeDet=dot(localD,normal);
                if(planeDet==0.0){return geometryUnknown();}
                let t=dot(tri.a.xyz-localO,normal)/planeDet;
                if(!geometryFinite(t)||!geometryFinite(u)||!geometryFinite(bary)){return geometryUnknown();}
                if(bary<0.0||u+bary>1.0||t<nearT||t>bestT){continue;}
                let primitive=bitcast<u32>(tri.a.w);
                if(found&&t==bestT&&(inst.identity.y>result.identity.y||
                    (inst.identity.y==result.identity.y&&primitive>=result.identity.z))){continue;}
                let worldN=vec3<f32>(dot(vec3<f32>(inst.inverse0.x,inst.inverse1.x,inst.inverse2.x),normal),
                    dot(vec3<f32>(inst.inverse0.y,inst.inverse1.y,inst.inverse2.y),normal),
                    dot(vec3<f32>(inst.inverse0.z,inst.inverse1.z,inst.inverse2.z),normal));
                let scale=max(abs(worldN.x),max(abs(worldN.y),abs(worldN.z)));
                if(!geometryFinite3(worldN)||scale<=0.0){return geometryUnknown();}
                bestT=t;found=true;result.distance_normal=vec4<f32>(t,normalize(worldN/scale));
                result.identity.y=inst.identity.y;result.identity.z=primitive;
                result.a=tri.a;result.e1=tri.e1;result.e2=tri.e2;
            }
        }
    }
    result.identity.x=select(0u,1u,found);return result;
}
fn geometryClassifyMeasured(ray:GeometryRay,input:GeometryHit)->GeometryHit {
    var hit=input;hit.identity.w=0u;hit.measured=vec4<f32>(-1.0,0.0,0.0,0.0);
    // A clipped near segment cannot certify a measurement from the origin.
    if(ray.origin_near.w!=0.0){return hit;}
    if(hit.identity.x==1u){
        let t=hit.distance_normal.x;var n=hit.distance_normal.yzw;
        if(t>2e-5&&t<ray.direction_far.w-2e-5&&hit.identity.y<=65535u&&geometryFinite3(n)&&dot(n,n)>0.25){
            if(dot(n,ray.direction_far.xyz)>0.0){n=-n;}
            hit.measured=vec4<f32>(t,n);hit.identity.w=hit.identity.y+2u;
        }
    }else if(hit.identity.x==0u){
        let ahead=geometryBox(geometryTop[0],ray.origin_near.xyz,ray.direction_far.xyz,ray.direction_far.w,bitcast<f32>(0x7f7fffffu));
        hit.measured.x=select(-2.0,-3.0,ahead);
    }
    return hit;
}
@compute @workgroup_size(64)
fn geometry_first_hit(@builtin(global_invocation_id) id:vec3<u32>){
    if(id.x>=geometryParams.rays_ready.x||id.x>=arrayLength(&geometryRays)||id.x>=arrayLength(&geometryHits)){return;}
    let ray=geometryRays[id.x];var hit=geometryUnknown();
    if(geometryParams.rays_ready.y==1u){hit=geometryFirstHit(ray);}
    geometryHits[id.x]=geometryClassifyMeasured(ray,hit);
}
