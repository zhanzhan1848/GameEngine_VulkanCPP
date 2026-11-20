// Gather connection qualification, separate from measured first-surface queries.
// A traversed leaf with degenerate or nearly parallel geometry cannot certify
// a clear connection. Match the frozen Vulkan world-space 1e-6 angular guard.
fn geometryConnectionFirstHit(ray:GeometryRay)->GeometryHit {
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
                let connectionN=geometryTriangleNormal(tri.e1.xyz,tri.e2.xyz);
                let connectionWorldN=inst.inverse0.xyz*connectionN.x+inst.inverse1.xyz*connectionN.y+inst.inverse2.xyz*connectionN.z;
                let connectionLength=length(connectionWorldN);let connectionFacing=dot(localD,connectionN);
                if(!geometryFinite3(connectionN)||!geometryFinite3(connectionWorldN)||!geometryFinite(connectionLength)||!(connectionLength>0.0)||!geometryFinite(connectionFacing)||!geometryFinite(det)||det==0.0||abs(connectionFacing)<=1e-6*connectionLength){return geometryUnknown();}
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
