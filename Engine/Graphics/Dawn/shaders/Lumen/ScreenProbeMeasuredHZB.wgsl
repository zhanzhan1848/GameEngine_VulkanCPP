// Frozen split-trace HZB stage. Directions come from the single native ray
// producer; HZB deliberately uses its own 2-voxel origin (SDF uses 0.25).
@group(0) @binding(1) var hzb:texture_2d<f32>;
@group(0) @binding(2) var<storage,read> positions:array<vec4<f32>>;
@group(0) @binding(3) var<storage,read> normals:array<vec4<f32>>;
struct HZBRay {originWeight:vec4<f32>,directionCosine:vec4<f32>}
@group(0) @binding(4) var<storage,read> rays:array<HZBRay>;
@group(0) @binding(5) var<storage,read_write> hits:array<vec4<f32>>;
fn hzbLinear(d:f32)->f32 {return sp.hzb.x*sp.hzb.y/(sp.hzb.y-d*(sp.hzb.y-sp.hzb.x));}
@compute @workgroup_size(64)
fn measured_hzb_trace(@builtin(global_invocation_id) gid:vec3<u32>){
    let index=gid.x;let count=u32(sp.grid.w);if(index>=u32(sp.grid.x)*u32(sp.grid.y)*count){return;}
    hits[index]=vec4<f32>(-1,0,0,0);let probe=index/count;if(positions[probe].w<=0){return;}
    let direction=rays[index].directionCosine.xyz;
    let origin=positions[probe].xyz+normalize(normals[probe].xyz)*sp.sdfVoxels[0].x*2.0;
    let limit=sp.hzb.w;if(limit<=0){return;}let levels=max(u32(sp.hzb.z),1u);
    let clip0=sp.viewProjection*vec4<f32>(origin,1);let clip1=sp.viewProjection*vec4<f32>(origin+direction*limit,1);
    if(clip0.w<=0||clip1.w<=0){return;}
    if(distance(clip0.xy/clip0.w*.5+.5,clip1.xy/clip1.w*.5+.5)>1.5){return;}
    let stepSize=limit/32.0;var t=0.0;var mip=1u;
    for(var step=0u;step<32u;step++){
        let scale=f32(1u<<mip);t+=stepSize*scale;if(t>limit){break;}
        let clip=sp.viewProjection*vec4<f32>(origin+direction*t,1);if(clip.w<=0){break;}
        let ndc=clip.xyz/clip.w;let uv=vec2<f32>(ndc.x*.5+.5,1.0-(ndc.y*.5+.5));
        if(any(uv<vec2<f32>(0))||any(uv>vec2<f32>(1))){break;}
        // Frozen kernel starts at mip 1. Explicitly defer unavailable mips;
        // no out-of-range textureLoad and no fabricated depth-zero occluder.
        if(mip>=levels){break;}
        let size=textureDimensions(hzb,i32(mip));let coord=clamp(vec2<i32>(uv*vec2<f32>(size)),vec2<i32>(0),vec2<i32>(size)-1);
        let scene=textureLoad(hzb,coord,i32(mip)).r;let sceneLin=hzbLinear(scene);let rayLin=hzbLinear(ndc.z);
        if(rayLin>sceneLin){
            if(mip==0u){
                if(rayLin<=sceneLin+.3){
                    let dims=vec2<f32>(textureDimensions(hzb,0));let center=(floor(uv*dims)+.5)/dims;
                    let screen=vec2<f32>(center.x*2-1,1-center.y*2);
                    let hp=sp.inverseVP*vec4<f32>(screen,scene,1);if(abs(hp.w)<1e-6){continue;}
                    let surface=hp.xyz/hp.w;
                    let px=sp.inverseVP*vec4<f32>(screen+vec2<f32>(2/dims.x,0),scene,1);if(abs(px.w)<1e-6){continue;}
                    let footprint=distance(surface,px.xyz/px.w);let along=dot(surface-origin,direction);
                    let error=distance(surface,origin+direction*along);
                    if(along<=0||along>limit||error>max(.02,footprint*1.5)){continue;}
                    hits[index]=vec4<f32>(along,center,1);return;
                }
            }else{t-=stepSize*scale;mip--;}
        }else{mip=min(mip+1u,levels-1u);}
    }
}
