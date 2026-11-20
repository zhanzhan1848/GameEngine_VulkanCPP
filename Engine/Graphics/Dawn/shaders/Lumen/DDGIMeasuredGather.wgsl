// Native mode-7 screen gather. GeometryFirstHit's native query functions are
// prefixed by the host; the frozen Vulkan geometry-replace branch is the contract.
struct GatherParams {
    inverseVP: mat4x4<f32>, camera: vec4<f32>, originSpacing: vec4<f32>,
    counts: vec4<u32>, jitterPolicy: vec4<f32>, // jitter.xy, classify, classifyV2
}
struct ProbeStats { confidence: vec4<f32>, sky: vec4<f32> }
@group(0) @binding(7) var<uniform> gather: GatherParams;
@group(0) @binding(8) var<storage,read> probeSH: array<vec4<f32>>;
@group(0) @binding(9) var<storage,read> probeDepth: array<f32>;
@group(0) @binding(10) var<storage,read> probeOrigins: array<vec4<f32>>;
@group(0) @binding(11) var<storage,read> probeStats: array<ProbeStats>;
@group(0) @binding(12) var screenDepth: texture_depth_2d;
@group(0) @binding(13) var screenNormal: texture_2d<f32>;
@group(0) @binding(14) var screenORM: texture_2d<f32>;
@group(0) @binding(15) var diffuseOut: texture_storage_2d<rgba16float,write>;
@group(0) @binding(16) var specularOut: texture_storage_2d<rgba16float,write>;

// Preserve the frozen inverse-VP accumulation order (3, 0, 1, 2).
// Micrometer receiver drift can flip coplanar connection qualification.
fn gatherTransform(v:vec4<f32>)->vec4<f32>{let m=gather.inverseVP;return fma(m[2],vec4<f32>(v.z),fma(m[1],vec4<f32>(v.y),fma(m[0],vec4<f32>(v.x),m[3]*v.w)));}
fn surfacePoint(pixel:vec2<i32>)->vec4<f32> {
    let size=vec2<i32>(textureDimensions(screenDepth));
    if(any(pixel<vec2<i32>(0))||any(pixel>=size)){return vec4<f32>(0);}
    let z=textureLoad(screenDepth,pixel,0);
    if(!geometryFinite(z)||z<0.0||z>=.999999){return vec4<f32>(0);}
    let uv=(vec2<f32>(pixel)+.5)/vec2<f32>(size);
    let ndc=vec2<f32>(2.0*uv.x-1.0,1.0-2.0*uv.y)-gather.jitterPolicy.xy;
    let h=gatherTransform(vec4<f32>(ndc,z,1));
    if(!geometryFinite3(h.xyz)||!geometryFinite(h.w)||abs(h.w)<1e-8){return vec4<f32>(0);}
    let p=h.xyz/h.w;if(!geometryFinite3(p)){return vec4<f32>(0);}return vec4<f32>(p,1);
}
fn surfaceOrigin(pixel:vec2<i32>)->vec4<f32> {
    let p=surfacePoint(pixel);if(p.w==0.0){return p;}
    let v=gather.camera.xyz-p.xyz;if(!geometryFinite3(v)||dot(v,v)<1e-10){return vec4<f32>(0);}
    let a=surfacePoint(pixel-vec2<i32>(1,0));let b=surfacePoint(pixel+vec2<i32>(1,0));
    let c=surfacePoint(pixel-vec2<i32>(0,1));let d=surfacePoint(pixel+vec2<i32>(0,1));
    if((a.w==0.0&&b.w==0.0)||(c.w==0.0&&d.w==0.0)){return vec4<f32>(0);}
    let dx=select(b.xyz-p.xyz,p.xyz-a.xyz,a.w!=0.0&&(b.w==0.0||distance(a.xyz,p.xyz)<distance(b.xyz,p.xyz)));
    let dy=select(d.xyz-p.xyz,p.xyz-c.xyz,c.w!=0.0&&(d.w==0.0||distance(c.xyz,p.xyz)<distance(d.xyz,p.xyz)));
    let limit=max(.02,length(v)*.02);if(length(dx)>limit||length(dy)>limit){return vec4<f32>(0);}
    var n=cross(dx,dy);let scale=max(abs(n.x),max(abs(n.y),abs(n.z)));
    if(!geometryFinite3(n)||scale<1e-16){return vec4<f32>(0);}
    n=normalize(n/scale);if(dot(n,v)<0.0){n=-n;}return vec4<f32>(p.xyz+n*.002,1);
}
// Only a certified open segment returns true. Boundary hits and malformed
// queries remain unknown. The physical 2mm origin is independent of SH bias.
fn connectionClear(receiver:vec3<f32>,probe:vec3<f32>)->bool {
    let delta=probe-receiver;let dist=length(delta);let eps=.00002;
    if(!geometryFinite3(receiver)||!geometryFinite3(probe)||!geometryFinite(dist)||dist<=2.0*eps){return false;}
    if(any((bitcast<vec3<u32>>(receiver)&vec3<u32>(0x7f800000u))>=vec3<u32>(132u<<23u))||
       any((bitcast<vec3<u32>>(probe)&vec3<u32>(0x7f800000u))>=vec3<u32>(132u<<23u))){return false;}
    let direction=delta/dist;let start=receiver-direction*eps;let end=probe+direction*eps;
    let farT=dist+2.0*eps;let extension=farT-dist;let ss=receiver-start;let es=end-probe;
    let sm=dot(ss,direction);let em=dot(es,direction);let sideS=length(ss-sm*direction);let sideE=length(es-em*direction);
    if(!geometryFinite3(start)||!geometryFinite3(end)||!geometryFinite(farT)||
       extension<eps*1.8||extension>eps*2.2||sideS>eps*.1||sideE>eps*.1||
       sm<eps*.9||sm>eps*1.1||em<eps*.9||em>eps*1.1){return false;}
    let hit=geometryConnectionFirstHit(GeometryRay(vec4<f32>(start,0),vec4<f32>(direction,farT)));
    return hit.identity.x==0u;
}
fn octEncode(d:vec3<f32>)->vec2<f32> {
    var p=d.xy/(abs(d.x)+abs(d.y)+abs(d.z));
    if(d.z<0.0){p=(1.0-abs(p.yx))*select(vec2<f32>(-1),vec2<f32>(1),p>=vec2<f32>(0));}return p*.5+.5;
}
fn evaluateSH(p:u32,d:vec3<f32>,cosine:bool)->vec3<f32> {
    let a=select(1.0,.6666667,cosine);let b=select(1.0,.25,cosine);let base=p*9u;
    return probeSH[base].xyz*.28209479
        +probeSH[base+1u].xyz*(-.48860251*a*d.y)+probeSH[base+2u].xyz*(.48860251*a*d.z)
        +probeSH[base+3u].xyz*(-.48860251*a*d.x)+probeSH[base+4u].xyz*(1.09254843*b*d.x*d.y)
        +probeSH[base+5u].xyz*(-1.09254843*b*d.y*d.z)+probeSH[base+6u].xyz*(.31539157*b*(3.0*d.z*d.z-1.0))
        +probeSH[base+7u].xyz*(-1.09254843*b*d.z*d.x)+probeSH[base+8u].xyz*(.54627421*b*(d.x*d.x-d.y*d.y));
}
@compute @workgroup_size(8,8)
fn measured_gather(@builtin(global_invocation_id) gid:vec3<u32>) {
    let size=textureDimensions(diffuseOut);if(any(gid.xy>=size)){return;}let pixel=vec2<i32>(gid.xy);
    textureStore(diffuseOut,pixel,vec4<f32>(0));textureStore(specularOut,pixel,vec4<f32>(0));
    let uv=(vec2<f32>(gid.xy)+.5)/vec2<f32>(size);let full=vec2<i32>(uv*vec2<f32>(textureDimensions(screenDepth)));
    let z=textureLoad(screenDepth,full,0);if(!geometryFinite(z)||z<0.0||z>=1.0){return;}
    let h=gatherTransform(vec4<f32>(uv.x*2.0-1.0,1.0-uv.y*2.0,z,1));
    if(!geometryFinite3(h.xyz)||!geometryFinite(h.w)||abs(h.w)<1e-8){return;}let world=h.xyz/h.w;
    var n=normalize(textureLoad(screenNormal,full,0).xyz*2.0-1.0);if(!geometryFinite3(n)){n=vec3<f32>(0,1,0);}
    let receiver=surfaceOrigin(full);let counts=gather.counts.xyz;let spacing=gather.originSpacing.w;
    let gp=(world-gather.originSpacing.xyz)/spacing;if(any(gp<vec3<f32>(0))||any(gp>vec3<f32>(counts-1u))){return;}
    let b0=min(vec3<u32>(floor(gp)),counts-1u);let b1=min(b0+1u,counts-1u);let f=fract(gp);
    var ids:array<u32,8>;var weights:array<f32,8>;var pixelOpen=0.0;var pixelW=0.0;
    for(var i=0u;i<8u;i++){
        let upper=vec3<bool>((i&1u)!=0u,(i&2u)!=0u,(i&4u)!=0u);let b=select(b0,b1,upper);let w=select(1.0-f,f,upper);
        ids[i]=b.x+b.y*counts.x+b.z*counts.x*counts.y;weights[i]=w.x*w.y*w.z;
        if(gather.jitterPolicy.z>.5&&weights[i]>=.01){pixelOpen+=probeStats[ids[i]].sky.x*weights[i];pixelW+=weights[i];}
    }
    if(pixelW>0.0){pixelOpen/=pixelW;}
    let view=normalize(gather.camera.xyz-world);let reflection=reflect(-view,n);let noV=clamp(dot(n,view),.05,1.0);
    let biased=world+n*spacing*.05;var result=vec3<f32>(0);var refl=vec3<f32>(0);var total=0.0;var confidenceBase=0.0;
    for(var i=0u;i<8u;i++){
        let p=ids[i];let tw=weights[i];if(!(tw>0.0)){continue;}
        var valid=true;for(var j=0u;j<9u;j++){valid=valid&&geometryFinite3(probeSH[p*9u+j].xyz);}if(!valid){continue;}
        var attenuation=1.0;if(gather.jitterPolicy.z>.5){let sky=probeStats[p].sky;
            let classification=1.0-smoothstep(.15,.45,sky.x)*(1.0-smoothstep(.05,.25,pixelOpen))*.85;
            attenuation=select(classification,mix(1.0,classification,sky.y),gather.jitterPolicy.w>.5);}
        let origin=probeOrigins[p];var state=0.0;
        let gc=vec3<u32>(p%counts.x,(p/counts.x)%counts.y,p/(counts.x*counts.y));
        var probe=gather.originSpacing.xyz+vec3<f32>(gc)*spacing;
        if(origin.w>.25&&geometryFinite3(origin.xyz)&&geometryFinite(origin.w)){probe=origin.xyz;state=origin.w;}
        let delta=probe-biased;let dist=length(delta);var toProbe=n;if(dist>1e-5){toProbe=delta/max(dist,1e-5);}
        let nw=max(mix(.2,1.0,clamp((dot(n,toProbe)+.5)/.7,0.0,1.0)),.40);
        let oct=clamp(vec2<u32>(octEncode(-toProbe)*8.0),vec2<u32>(0),vec2<u32>(7));let at=p*128u+oct.y*8u+oct.x;
        let mv=vec2<f32>(probeDepth[at],probeDepth[at+64u]);var burial=1.0;var inside=1.0;
        if(mv.x<spacing*.2){let t=smoothstep(0.0,.2,mv.x/spacing);inside=mix(.2,1.0,t);burial=mix(.1,1.0,t);}
        confidenceBase+=tw*burial*attenuation;
        let qualified=geometryFinite(mv.x)&&geometryFinite(mv.y)&&all(mv>=vec2<f32>(0))&&state>.25;
        var visibility=0.0;if(qualified&&receiver.w>0.0&&connectionClear(receiver.xyz,probe)){visibility=select(.5,1.0,state>=.75);}
        let weight=tw*attenuation*nw*visibility*inside;
        result+=evaluateSH(p,n,true)*weight;refl+=evaluateSH(p,reflection,false)*weight;total+=weight;
    }
    if(!(total>0.0)){return;}result/=total;refl/=total;result*=clamp(total/max(confidenceBase,1e-4),0.0,1.0);
    let roughness=clamp(textureLoad(screenORM,full,0).g,.04,1.0);
    textureStore(diffuseOut,pixel,vec4<f32>(max(result*3.14159265,vec3<f32>(0)),1));
    textureStore(specularOut,pixel,vec4<f32>(mix(refl,max(result,vec3<f32>(0)),roughness*roughness),noV));
}
