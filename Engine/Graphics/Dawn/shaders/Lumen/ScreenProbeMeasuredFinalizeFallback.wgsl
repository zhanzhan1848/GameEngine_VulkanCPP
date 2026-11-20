// Selected measured-visibility branch of frozen SPGI Finalize. No sky revival,
// visibility floor, tetrahedral approximation or irradiance/radiance conversion.
struct SpRay { originWeight:vec4<f32>, directionCosine:vec4<f32> }
@group(0) @binding(1) var<storage,read> positions:array<vec4<f32>>;
@group(0) @binding(2) var<storage,read> normals:array<vec4<f32>>;
@group(0) @binding(3) var<storage,read> rays:array<SpRay>;
@group(0) @binding(4) var<storage,read_write> radiance:array<vec4<f32>>;
@group(0) @binding(5) var<storage,read> sh:array<vec4<f32>>;
@group(0) @binding(6) var<storage,read> depth:array<f32>;
@group(0) @binding(7) var<storage,read> origins:array<vec4<f32>>;
@group(0) @binding(8) var<storage,read_write> audit:array<vec4<u32>>;
fn spFinite(x:f32)->bool {return (bitcast<u32>(x)&0x7f800000u)!=0x7f800000u;}
fn spFinite3(x:vec3<f32>)->bool {return spFinite(x.x)&&spFinite(x.y)&&spFinite(x.z);}
fn spSH(index:u32,d:vec3<f32>)->vec3<f32> {
    let base=index*9u;
    return sh[base].xyz*.282095+sh[base+1u].xyz*(-.488603*d.y)+sh[base+2u].xyz*(.488603*d.z)
        +sh[base+3u].xyz*(-.488603*d.x)+sh[base+4u].xyz*(1.092548*d.y*d.x)
        +sh[base+5u].xyz*(-1.092548*d.y*d.z)+sh[base+6u].xyz*(.315392*(3.0*d.z*d.z-1.0))
        +sh[base+7u].xyz*(-1.092548*d.x*d.z)+sh[base+8u].xyz*(.546274*(d.x*d.x-d.y*d.y));
}
@compute @workgroup_size(64) fn measured_finalize_fallback(@builtin(global_invocation_id) id:vec3<u32>) {
    let i=id.x;if(i>=u32(sp.grid.x)*u32(sp.grid.y)*u32(sp.grid.w)||audit[i].x!=3u){return;}
    // Unknown support is excluded by Average (w=0), including outside-volume.
    radiance[i]=vec4<f32>(0);audit[i].x=5u;if(sp.ddgi[1].w<=.5){return;}
    let probe=i/u32(sp.grid.w);let pos=positions[probe].xyz;let n=normalize(normals[probe].xyz);let ray=rays[i];let dir=ray.directionCosine.xyz;
    let org=sp.ddgi[0].xyz;let spacing=max(sp.ddgi[0].w,.01);let counts=vec3<i32>(sp.ddgi[1].xyz);let gp=(pos-org)/spacing;
    if(any(gp<vec3<f32>(0))||any(gp>vec3<f32>(counts-1))){return;}
    let b0=clamp(vec3<i32>(floor(gp)),vec3<i32>(0),counts-1);let b1=min(b0+1,counts-1);let fr=clamp(fract(gp),vec3<f32>(0),vec3<f32>(1));
    var acc=vec3<f32>(0);var sum=0.0;var unoccluded=0.0;
    for(var z=0;z<2;z++){for(var y=0;y<2;y++){for(var x=0;x<2;x++){
        let corner=vec3<i32>(x,y,z);let weights=mix(1.0-fr,fr,vec3<f32>(corner));let tw=weights.x*weights.y*weights.z;if(tw<=0.0){continue;}
        let pc=select(b0,b1,corner==vec3<i32>(1));let index=u32(pc.x+counts.x*(pc.y+counts.y*pc.z));
        let toProbe=normalize(org+vec3<f32>(pc)*spacing-pos);let directional=.25+.75*clamp(dot(toProbe,dir)*.5+.5,0.0,1.0);var w=tw*directional;unoccluded+=w;
        if(index>=arrayLength(&origins)||index>=arrayLength(&depth)/128u||index>=arrayLength(&sh)/9u){continue;}
        let relocated=origins[index];let qualified=relocated.w>.25&&spFinite3(relocated.xyz)&&spFinite(relocated.w);
        let anchor=select(org+vec3<f32>(pc)*spacing,relocated.xyz,qualified);let delta=pos-anchor;let distance=length(delta);
        var direction=n;if(distance>.0001){direction=delta/max(distance,.0001);}
        var oct=direction.xy/max(dot(abs(direction),vec3<f32>(1)),.0001);
        if(direction.z<0.0){oct=(1.0-abs(oct.yx))*select(vec2<f32>(-1),vec2<f32>(1),oct>=vec2<f32>(0));}
        let texel=vec2<u32>(clamp((oct*.5+.5)*8.0,vec2<f32>(0),vec2<f32>(7)));let at=index*128u+texel.y*8u+texel.x;
        let mean=depth[at];let variance=depth[at+64u];let beyond=max(distance-mean,0.0);var visibility=0.0;
        if(qualified&&spFinite(distance)&&spFinite(mean)&&mean>=0.0&&spFinite(variance)&&variance>=0.0){
            visibility=1.0;if(beyond!=0.0){visibility=variance/(variance+beyond*beyond);}
        }
        visibility*=select(select(0.0,.5,relocated.w>0.0),1.0,relocated.w>.75);w*=visibility;
        let value=spSH(index,dir);if(!spFinite3(value)){continue;}acc+=max(value,vec3<f32>(0))*w;sum+=w;
    }}}
    if(!(sum>0.0)){return;}
    var value=acc/max(sum,.0001)*clamp(sum/max(unoccluded,.0001),0.0,1.0);
    if(!spFinite3(value)){value=vec3<f32>(0);}value=min(value,vec3<f32>(64));
    if(sp.diagnostics.x>4.5&&sp.diagnostics.x<5.5){value=vec3<f32>(0);}
    radiance[i]=vec4<f32>(value,-max(ray.directionCosine.w,0.0)*ray.originWeight.w);audit[i].x=4u;
}
