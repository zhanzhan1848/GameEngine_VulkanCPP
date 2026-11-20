// Frozen-profile radiance SH -> full-resolution irradiance. Native WGSL.
// Native WGSL source shared by the frozen-profile SPGI stages.
struct ScreenProbeData {
    viewProjection:mat4x4<f32>,inverseVP:mat4x4<f32>,camera:vec4<f32>,
    grid:vec4<f32>,trace:vec4<f32>,sdfOrigins:array<vec4<f32>,3>,
    sdfVoxels:array<vec4<f32>,3>,sdfExtents:array<vec4<f32>,3>,sdfResolutions:vec4<f32>,
    surfaceCache:vec4<f32>,ddgi:array<vec4<f32>,2>,hzb:vec4<f32>,cache:vec4<f32>,
    diagnostics:vec4<f32>,hint:vec4<f32>,sky:vec4<f32>,wrc:array<vec4<f32>,2>,
    representative:vec4<f32>,tier:vec4<f32>,tierCounts:vec4<f32>,
}

@group(0) @binding(0) var depthTexture:texture_depth_2d;
@group(0) @binding(1) var normalTexture:texture_2d<f32>;
@group(0) @binding(2) var outputTexture:texture_storage_2d<rgba16float,write>;
@group(0) @binding(3) var<storage,read> positions:array<vec4<f32>>;
@group(0) @binding(4) var<storage,read> sh:array<vec4<f32>>;
@group(0) @binding(5) var<uniform> sp:ScreenProbeData;
@group(0) @binding(6) var<storage,read> normals:array<vec4<f32>>;
fn sampleNormal(uv:vec2<f32>)->vec4<f32>{
    let size=vec2<f32>(textureDimensions(normalTexture));let px=uv*size-.5;let p=vec2<i32>(floor(px));let f=fract(px);let hi=vec2<i32>(size)-1;
    let a=textureLoad(normalTexture,clamp(p,vec2<i32>(0),hi),0);let b=textureLoad(normalTexture,clamp(p+vec2<i32>(1,0),vec2<i32>(0),hi),0);
    let c=textureLoad(normalTexture,clamp(p+vec2<i32>(0,1),vec2<i32>(0),hi),0);let d=textureLoad(normalTexture,clamp(p+vec2<i32>(1,1),vec2<i32>(0),hi),0);
    return mix(mix(a,b,f.x),mix(c,d,f.x),f.y);
}
fn sampleDepth(uv:vec2<f32>)->f32{
    let size=vec2<f32>(textureDimensions(depthTexture));let px=uv*size-.5;let p=vec2<i32>(floor(px));let f=fract(px);let hi=vec2<i32>(size)-1;
    let a=textureLoad(depthTexture,clamp(p,vec2<i32>(0),hi),0);let b=textureLoad(depthTexture,clamp(p+vec2<i32>(1,0),vec2<i32>(0),hi),0);
    let c=textureLoad(depthTexture,clamp(p+vec2<i32>(0,1),vec2<i32>(0),hi),0);let d=textureLoad(depthTexture,clamp(p+vec2<i32>(1,1),vec2<i32>(0),hi),0);
    return mix(mix(a,b,f.x),mix(c,d,f.x),f.y);
}
@compute @workgroup_size(8,8,1)
fn measured_screen_gather(@builtin(global_invocation_id) id:vec3<u32>){
    let size=vec2<u32>(sp.trace.zw);if(any(id.xy>=size)){return;}
    let uv=(vec2<f32>(id.xy)+.5)*(1.0/vec2<f32>(size));let depth=sampleDepth(uv);
    if(depth<=.0001||depth>=.999){textureStore(outputTexture,vec2<i32>(id.xy),vec4<f32>(0));return;}
    let n=normalize(sampleNormal(uv).xyz*2.0-1.0);let ndc=vec2<f32>(uv.x*2.0-1.0,1.0-uv.y*2.0);
    let receiverH=sp.inverseVP*vec4<f32>(ndc,depth,1);let receiver=receiverH.xyz/receiverH.w;
    let footprintH=sp.inverseVP*vec4<f32>(ndc+vec2<f32>(2.0*sp.grid.z/f32(size.x),0),depth,1);
    let sigma=max(.02,distance(receiver,footprintH.xyz/footprintH.w)*.25);
    let coord=(vec2<f32>(id.xy)+.5)/sp.grid.z-.5;let base=vec2<i32>(floor(coord));
    var sum=vec3<f32>(0);var confidence=0.0;var weightSum=0.0;
    var fallback=vec3<f32>(0);var fallbackConfidence=0.0;var fallbackWeight=0.0;
    for(var i=0;i<16;i++){
        let cell=base+vec2<i32>(i%4-1,i/4-1);let tent=max(vec2<f32>(0),1.0-abs(vec2<f32>(cell)-coord)*.5);let spatial=tent.x*tent.y;
        if(spatial<=0.0||any(cell<vec2<i32>(0))||any(cell>=vec2<i32>(sp.grid.xy))){continue;}
        let slots=select(1u,4u,sp.tier.x>.5);let tile=u32(cell.y)*u32(sp.grid.x)+u32(cell.x);var best=0xffffffffu;var geometry=-1.0;
        for(var slot=0u;slot<slots;slot++){
            let candidate=tile*slots+slot;let pos=positions[candidate];if(pos.w<=0.0){continue;}
            let plane=abs(dot(pos.xyz-receiver,n));let g=exp(-.5*plane*plane/(sigma*sigma));
            if(g>geometry){geometry=g;best=candidate;}
        }
        if(best==0xffffffffu||geometry<1e-5){continue;}
        let b=best*4u;let conf=sh[b].w;if(conf<=0.0){continue;}
        let irr=max(vec3<f32>(0),sh[b].xyz*(.282095*3.14159265)+sh[b+1u].xyz*(-.488603*2.09439510*n.y)+sh[b+2u].xyz*(.488603*2.09439510*n.z)+sh[b+3u].xyz*(-.488603*2.09439510*n.x));
        let weight=spatial*geometry*conf;fallback+=irr*weight;fallbackConfidence+=conf*weight;fallbackWeight+=weight;
        var normalWeight=1.0;let pn=normals[best];if(pn.w>0.0){normalWeight=mix(.15,1.0,smoothstep(-.25,.8,dot(n,normalize(pn.xyz))));}
        let w=weight*normalWeight;sum+=irr*w;confidence+=conf*w;weightSum+=w;
    }
    if(weightSum<=1e-6&&fallbackWeight>1e-6){sum=fallback;confidence=fallbackConfidence;weightSum=fallbackWeight;}
    if(weightSum<=1e-6){textureStore(outputTexture,vec2<i32>(id.xy),vec4<f32>(0));return;}
    textureStore(outputTexture,vec2<i32>(id.xy),vec4<f32>(max(sum/weightSum,vec3<f32>(0)),confidence/weightSum));
}
