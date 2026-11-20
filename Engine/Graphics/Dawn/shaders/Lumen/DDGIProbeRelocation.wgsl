// Native WGSL translation of the frozen measured DDGI relocation policy.
// Explicit texel interpolation is required: hardware filtering can pick a
// different maximum in the ordered 14-direction search.
struct RelocationParameters { origin:vec4<f32>, extent:vec4<f32>, policy:vec4<f32>, counts:vec4<u32> }
@group(0) @binding(0) var<storage,read> baseOrigins:array<vec4<f32>>;
@group(0) @binding(1) var<storage,read_write> relocatedOrigins:array<vec4<f32>>;
@group(0) @binding(2) var field:texture_3d<f32>;
@group(0) @binding(3) var<uniform> params:RelocationParameters;
fn sampleField(position:vec3<f32>)->f32 {
    let local=position-params.origin.xyz;
    if(any(local<vec3<f32>(0.0)) || any(local>=params.extent.xyz)){return 1e10;}
    let p=local/params.extent.xyz*f32(params.counts.x)-0.5;
    let base=vec3<i32>(floor(p));let f=p-vec3<f32>(base);
    let limit=vec3<i32>(i32(params.counts.x)-1);
    let a=clamp(base,vec3<i32>(0),limit);let b=clamp(base+vec3<i32>(1),vec3<i32>(0),limit);
    let d000=textureLoad(field,vec3<i32>(a.x,a.y,a.z),0).r;
    let d100=textureLoad(field,vec3<i32>(b.x,a.y,a.z),0).r;
    let d010=textureLoad(field,vec3<i32>(a.x,b.y,a.z),0).r;
    let d110=textureLoad(field,vec3<i32>(b.x,b.y,a.z),0).r;
    let d001=textureLoad(field,vec3<i32>(a.x,a.y,b.z),0).r;
    let d101=textureLoad(field,vec3<i32>(b.x,a.y,b.z),0).r;
    let d011=textureLoad(field,vec3<i32>(a.x,b.y,b.z),0).r;
    let d111=textureLoad(field,vec3<i32>(b.x,b.y,b.z),0).r;
    return mix(mix(mix(d000,d100,f.x),mix(d010,d110,f.x),f.y),mix(mix(d001,d101,f.x),mix(d011,d111,f.x),f.y),f.z);
}
fn relocate(probe:vec3<f32>)->vec4<f32> {
    let spacing=params.policy.x;let voxel=params.policy.y;
    let skin=max(voxel*1.5,spacing*0.05);let maxPush=spacing*0.95;
    let initial=sampleField(probe);
    if(initial>=1e9 || initial>skin){return vec4<f32>(probe,1.0);}
    var best=probe;var bestD=initial;
    let dirs=array<vec3<f32>,14>(vec3<f32>(1,0,0),vec3<f32>(-1,0,0),vec3<f32>(0,1,0),vec3<f32>(0,-1,0),vec3<f32>(0,0,1),vec3<f32>(0,0,-1),
        normalize(vec3<f32>(1,1,1)),normalize(vec3<f32>(-1,1,1)),normalize(vec3<f32>(1,-1,1)),normalize(vec3<f32>(-1,-1,1)),
        normalize(vec3<f32>(1,1,-1)),normalize(vec3<f32>(-1,1,-1)),normalize(vec3<f32>(1,-1,-1)),normalize(vec3<f32>(-1,-1,-1)));
    let step=max(voxel,spacing*0.1);
    for(var round=0u;round<3u;round++){
        var roundBest=best;var roundD=bestD;
        for(var k=0u;k<14u;k++){
            var p=best;
            for(var i=0u;i<10u;i++){
                // Match the fused multiply-add used by the frozen GLSL/MSL
                // traversal. Splitting this changes nine Sponza origins by ULPs.
                p=fma(dirs[k],vec3<f32>(step),p);let delta=p-probe;
                if(dot(delta,delta)>maxPush*maxPush){break;}
                let d=sampleField(p);
                if(d<1e9 && d>roundD){roundD=d;roundBest=p;}
                if(d>skin){break;}
            }
        }
        if(roundD<=bestD){break;}
        best=roundBest;bestD=roundD;if(bestD>skin){break;}
    }
    var state=1.0;
    if(bestD<=skin){
        var freeBest=best;var freeD=bestD;
        for(var x=-1;x<=1;x++){for(var y=-1;y<=1;y++){for(var z=-1;z<=1;z++){
            if(x==0 && y==0 && z==0){continue;}
            let p=probe+vec3<f32>(f32(x),f32(y),f32(z))*spacing;let d=sampleField(p);
            if(d<1e9 && d>freeD){freeD=d;freeBest=p;}
        }}}
        if(freeD>skin){best=freeBest;state=0.5;}
    }
    return vec4<f32>(best,state);
}
@compute @workgroup_size(64)
fn ddgi_relocate_probes(@builtin(global_invocation_id) id:vec3<u32>){
    if(id.x>=params.counts.y || id.x>=arrayLength(&baseOrigins) || id.x>=arrayLength(&relocatedOrigins)){return;}
    if(params.counts.z!=1u || params.counts.x==0u){relocatedOrigins[id.x]=vec4<f32>(baseOrigins[id.x].xyz,0.0);return;}
    relocatedOrigins[id.x]=relocate(baseOrigins[id.x].xyz);
}
