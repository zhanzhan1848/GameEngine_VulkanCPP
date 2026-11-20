// Frozen offline-volume profile: one cascade, no optional detail slots.
// TraceLib's implementation is trilinear despite its historical comments.
@group(0) @binding(1) var<storage,read> sdfPositions:array<vec4<f32>>;
struct SDFRay {originWeight:vec4<f32>,directionCosine:vec4<f32>}
@group(0) @binding(2) var<storage,read> sdfRays:array<SDFRay>;
@group(0) @binding(3) var<storage,read> hzbHits:array<vec4<f32>>;
@group(0) @binding(4) var sdfField:texture_3d<f32>;
@group(0) @binding(5) var<storage,read_write> sdfHits:array<vec4<f32>>;
// Optional bounded audit: [count, ray IDs...]; zero count on normal runs.
@group(0) @binding(6) var<storage,read> sdfAuditRays:array<u32>;
@group(0) @binding(7) var<storage,read_write> sdfAuditSteps:array<vec4<f32>>;
fn screenSDFSample(pos:vec3<f32>)->f32 {
    let local=fma(pos,vec3<f32>(1),-sp.sdfOrigins[0].xyz);let extent=sp.sdfExtents[0].xyz;
    if(any(local<vec3<f32>(0))||any(local>=extent)){return 1e10;}
    let size=vec3<i32>(textureDimensions(sdfField));let uv=local/extent;
    let p=clamp(uv,vec3<f32>(0),vec3<f32>(1))*vec3<f32>(size)-.5;
    let base=vec3<i32>(floor(p));let f=p-vec3<f32>(base);
    let a=clamp(base,vec3<i32>(0),size-1);let b=clamp(base+1,vec3<i32>(0),size-1);
    let v000=textureLoad(sdfField,a,0).r;let v100=textureLoad(sdfField,vec3<i32>(b.x,a.y,a.z),0).r;
    let v010=textureLoad(sdfField,vec3<i32>(a.x,b.y,a.z),0).r;let v110=textureLoad(sdfField,vec3<i32>(b.x,b.y,a.z),0).r;
    let v001=textureLoad(sdfField,vec3<i32>(a.x,a.y,b.z),0).r;let v101=textureLoad(sdfField,vec3<i32>(b.x,a.y,b.z),0).r;
    let v011=textureLoad(sdfField,vec3<i32>(a.x,b.y,b.z),0).r;let v111=textureLoad(sdfField,b,0).r;
    return mix(mix(mix(v000,v100,f.x),mix(v010,v110,f.x),f.y),mix(mix(v001,v101,f.x),mix(v011,v111,f.x),f.y),f.z);
}
fn screenSDFNormal(pos:vec3<f32>,audit:u32)->u32 {
    let h=max(sp.sdfVoxels[0].x,1e-4);
    let dx=screenSDFSample(fma(vec3<f32>(h,0,0),vec3<f32>(1),pos))-screenSDFSample(fma(vec3<f32>(h,0,0),vec3<f32>(-1),pos));
    let dy=screenSDFSample(fma(vec3<f32>(0,h,0),vec3<f32>(1),pos))-screenSDFSample(fma(vec3<f32>(0,h,0),vec3<f32>(-1),pos));
    let dz=screenSDFSample(fma(vec3<f32>(0,0,h),vec3<f32>(1),pos))-screenSDFSample(fma(vec3<f32>(0,0,h),vec3<f32>(-1),pos));
    if(audit!=0xffffffffu){sdfAuditSteps[audit*66u+64u]=vec4<f32>(pos,0);sdfAuditSteps[audit*66u+65u]=vec4<f32>(dx,dy,dz,0);}
    let n=normalize(vec3<f32>(dx,dy,dz)+vec3<f32>(1e-6));
    var oct=n.xy/(abs(n.x)+abs(n.y)+abs(n.z));
    if(n.z<0){oct=(1.0-abs(oct.yx))*select(vec2<f32>(-1),vec2<f32>(1),oct>=vec2<f32>(0));}
    // Keep the frozen [0,1] encode then signed remap, including its rounding.
    let encoded=oct*.5+.5;return pack2x16snorm(clamp(encoded*2.0-1.0,vec2<f32>(-1),vec2<f32>(1)));
}
@compute @workgroup_size(64)
fn measured_sdf_trace(@builtin(global_invocation_id) gid:vec3<u32>){
    let index=gid.x;let rays=u32(sp.grid.w);if(index>=u32(sp.grid.x)*u32(sp.grid.y)*rays){return;}
    var audit=0xffffffffu;
    for(var a=0u;a<sdfAuditRays[0];a++){if(sdfAuditRays[a+1u]==index){audit=a;break;}}
    if(audit!=0xffffffffu){for(var step=0u;step<32u;step++){sdfAuditSteps[audit*66u+step*2u]=vec4<f32>(0);sdfAuditSteps[audit*66u+step*2u+1u]=vec4<f32>(0);}}
    let previous=hzbHits[index];if(sp.hzb.z>0&&previous.w==1){sdfHits[index]=previous;return;}
    sdfHits[index]=vec4<f32>(-1,0,0,0);if(sdfPositions[index/rays].w<=0){return;}
    let origin=sdfRays[index].originWeight.xyz;let direction=sdfRays[index].directionCosine.xyz;
    var remaining=sp.trace.x;if(sp.cache.x>0){remaining=min(remaining,sp.cache.x);}
    var position=origin;let threshold=sp.sdfVoxels[0].x*.5;let minStep=sp.sdfVoxels[0].x*.25;
    for(var step=0u;step<32u;step++){
        let d=screenSDFSample(position);
        if(audit!=0xffffffffu){sdfAuditSteps[audit*66u+step*2u]=vec4<f32>(position,d);sdfAuditSteps[audit*66u+step*2u+1u]=vec4<f32>(remaining,threshold,sp.hint.z,1);}
        if(d<threshold){
            let distance=length(position-origin);let hitPosition=origin+direction*distance;
            // y is an opaque packed integer, possibly a NaN float bit pattern.
            // Consumers must bitcast it, never sanitize it as radiance.
            sdfHits[index]=vec4<f32>(distance,bitcast<f32>(screenSDFNormal(hitPosition,audit)),select(0.0,distance,sp.diagnostics.x>5.5&&sp.diagnostics.x<6.5),2);return;
        }
        let advance=select(max(d,minStep),d*sp.hint.z,d>threshold*4.0);
        if(advance>remaining){break;}position+=direction*advance;remaining-=advance;
    }
    if(sp.cache.x>0){sdfHits[index]=vec4<f32>(length(position-origin),0,0,3);}
}
