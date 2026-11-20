@group(0) @binding(1) var<storage,read> rayPositions:array<vec4<f32>>;
@group(0) @binding(2) var<storage,read> rayNormals:array<vec4<f32>>;
@group(0) @binding(3) var<storage,read> importanceHistory:array<vec4<f32>>;
struct ScreenProbeRay {originWeight:vec4<f32>,directionCosine:vec4<f32>}
@group(0) @binding(4) var<storage,read_write> screenRays:array<ScreenProbeRay>;
@compute @workgroup_size(64)
fn measured_screen_rays(@builtin(global_invocation_id) gid:vec3<u32>){
    let count=u32(sp.grid.w);let index=gid.x;if(index>=u32(sp.grid.x)*u32(sp.grid.y)*count){return;}
    let probe=index/count;let ray=index%count;screenRays[index]=ScreenProbeRay(vec4<f32>(0),vec4<f32>(0));
    if(rayPositions[probe].w<=0.0){return;}let n=normalize(rayNormals[probe].xyz);
    var direction=spRotation(probe,sp.camera.w)*spFibonacci(ray,count);var weight=1.0;
    if(sp.cache.y>.5){let b=probe*4u;let history=array<vec4<f32>,4>(importanceHistory[b],importanceHistory[b+1u],importanceHistory[b+2u],importanceHistory[b+3u]);
        let sample=spImportance(ray,count,probe,sp.camera.w,n,history);direction=sample.xyz;weight=sample.w;}
    direction=spOrient(direction,n);let origin=rayPositions[probe].xyz+n*sp.sdfVoxels[0].x*.25;
    screenRays[index]=ScreenProbeRay(vec4<f32>(origin,weight),vec4<f32>(direction,max(dot(n,direction),0.0)));
}
