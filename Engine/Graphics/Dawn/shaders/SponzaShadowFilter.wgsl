// Native four-cascade counterpart of the frozen Vulkan ShadowFilter.comp.
// WebGPU depth textures use explicit bilinear loads; output RGBA8 packs the
// two R8 reference outputs into R=visibility, G=transmission.
struct Params {
    inv_view_proj: mat4x4<f32>, shadow_vp: array<mat4x4<f32>,4>,
    light_dir: vec4<f32>, texel_size: vec2<f32>, depth_texel_size: vec2<f32>,
    shadow_quality: u32, render_width: u32, render_height: u32, sun_tan_half_angle: f32,
    cascade_params: array<vec4<f32>,4>,
};
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var shadow0: texture_depth_2d;
@group(0) @binding(2) var shadow1: texture_depth_2d;
@group(0) @binding(3) var visibilityOut: texture_storage_2d<rgba8unorm,write>;
@group(0) @binding(4) var normalTex: texture_2d<f32>;
@group(0) @binding(5) var depthTex: texture_depth_2d;
@group(0) @binding(6) var linearSampler: sampler;
@group(0) @binding(7) var shadow2: texture_depth_2d;
@group(0) @binding(8) var shadow3: texture_depth_2d;
const DISK = array<vec2<f32>,16>(
    vec2(-.5274373,-.7259060),vec2(.8218882,-.1694079),vec2(-.7959683,.5603005),vec2(.5697909,.7113788),
    vec2(-.1583544,-.8058264),vec2(.7826165,.5406202),vec2(-.6480213,-.1694079),vec2(.55,-.45),
    vec2(-.3,.7),vec2(.7,-.7),vec2(-.7,.3),vec2(.3,.8),vec2(-.6,-.5),vec2(.85,.4),vec2(-.2,.9),vec2(.4,-.3));

fn depthSample(tex: texture_depth_2d, uv: vec2<f32>) -> f32 {
    let size = vec2<i32>(textureDimensions(tex));
    let p = uv * vec2<f32>(size) - vec2<f32>(.5);
    let base = vec2<i32>(floor(p)); let t = fract(p);
    let hi = size - vec2<i32>(1);
    let a = textureLoad(tex, clamp(base,vec2<i32>(0),hi),0);
    let b = textureLoad(tex, clamp(base+vec2<i32>(1,0),vec2<i32>(0),hi),0);
    let c = textureLoad(tex, clamp(base+vec2<i32>(0,1),vec2<i32>(0),hi),0);
    let d = textureLoad(tex, clamp(base+vec2<i32>(1,1),vec2<i32>(0),hi),0);
    return mix(mix(a,b,t.x),mix(c,d,t.x),t.y);
}
fn cascadeSample(c: u32, uv: vec2<f32>) -> f32 {
    switch c {
        case 0u: { return depthSample(shadow0,uv); }
        case 1u: { return depthSample(shadow1,uv); }
        case 2u: { return depthSample(shadow2,uv); }
        default: { return depthSample(shadow3,uv); }
    }
}
fn project(c: u32, world: vec3<f32>) -> vec3<f32> {
    let clip = params.shadow_vp[c]*vec4(world,1);
    let sc = clip.xyz/max(clip.w,1e-6);
    return vec3(sc.xy*vec2(.5,-.5)+vec2(.5),sc.z);
}
fn contains(sc: vec3<f32>) -> bool { return all(sc>=vec3(0)) && all(sc<=vec3(1)); }
fn bias(c:u32,slope:f32,ndl:f32)->f32 {
    return params.cascade_params[c].x*(.5+2*min(slope,select(4.0,3.0,ndl>0)))/max(params.cascade_params[c].y,1e-3);
}
fn offset(c:u32,n:vec3<f32>,slope:f32,ndl:f32)->vec3<f32> {
    let k=select(1+2*min(slope,4.0),min(1+slope,4.0),ndl>0);
    return n*(params.cascade_params[c].x*k);
}
fn lightSize(c:u32,z:f32)->f32 {
    if(params.sun_tan_half_angle<=0){return .012;}
    let radius=z*max(params.cascade_params[c].y,1e-3)*params.sun_tan_half_angle/max(params.cascade_params[c].z*2,1e-3);
    return clamp(radius,params.texel_size.x,.05);
}
fn rotated(i:u32,sa:f32,ca:f32)->vec2<f32> {
    return vec2(DISK[i].x*ca-DISK[i].y*sa,DISK[i].x*sa+DISK[i].y*ca);
}
fn sampleShadow(c:u32,sc:vec3<f32>,screen:vec2<f32>,b:f32)->f32 {
    if(params.shadow_quality==0u){return select(1.0,0.0,sc.z-b>cascadeSample(c,sc.xy));}
    let noise=fract(.2659582193*fract(dot(screen,vec2(.7548776662,.5698402909))));
    let angle=noise*6.28318530718;let sa=sin(angle);let ca=cos(angle);
    var radius=2*params.texel_size.x;var taps=8u;
    if(params.shadow_quality!=1u){
        let size=lightSize(c,sc.z);var blockers=0.0;var sum=0.0;
        for(var i=0u;i<8u;i++){
            let d=cascadeSample(c,sc.xy+rotated(i,sa,ca)*size);
            if(d<sc.z-b){sum+=d;blockers+=1;}
        }
        if(blockers<.5){return 1;}
        let ratio=(sc.z-sum/blockers)/max(sc.z,1e-4);
        radius=clamp(ratio*size,params.texel_size.x*2,size*4);taps=16u;
    }
    var result=0.0;
    for(var i=0u;i<taps;i++){
        let d=cascadeSample(c,sc.xy+rotated(i,sa,ca)*radius);
        result+=select(1.0,0.0,sc.z-b>d);
    }
    return result/f32(taps);
}
fn filterWorld(world:vec3<f32>,n:vec3<f32>,screen:vec2<f32>)->vec2<f32> {
    let ndl=dot(n,-normalize(params.light_dir.xyz));
    let slope=clamp(sqrt(max(0.0,1-ndl*ndl))/max(abs(ndl),.25),0.0,4.0);
    if(params.shadow_quality>=3u && params.shadow_quality<=7u){
        let sc=project(0u,world+offset(0u,n,slope,ndl));let b=bias(0u,slope,ndl);var v=0.0;
        if(params.shadow_quality==7u){v=clamp(ndl*.5+.5,0.0,1.0);}
        else if(params.shadow_quality==4u){v=clamp(project(0u,world).z,0.0,1.0);}
        else if(contains(sc)){
            if(params.shadow_quality==5u){v=select(1.0,0.0,sc.z-b>cascadeSample(0u,sc.xy));}
            else if(params.shadow_quality==6u){
                for(var i=0u;i<8u;i++){v+=select(0.0,1.0,cascadeSample(0u,sc.xy+DISK[i]*.05)<sc.z-b);}
                v=clamp(v*.5,0.0,1.0);
            }else{v=cascadeSample(0u,sc.xy);}
        }
        return vec2(v,0);
    }
    for(var c=0u;c<4u;c++){
        let sc=project(c,world+offset(c,n,slope,ndl));
        if(contains(sc)){
            let b=bias(c,slope,ndl);let vis=sampleShadow(c,sc,screen,b);
            let thickness=max(sc.z-b-cascadeSample(c,sc.xy),0.0)*params.cascade_params[c].y;
            let transmission=exp(-thickness*1.5)*max(-ndl,0.0)*(1-vis);
            return vec2(vis,clamp(transmission,0.0,1.0));
        }
    }
    return vec2(1,0);
}
@compute @workgroup_size(8,8,1)
fn shadow_filter(@builtin(global_invocation_id) gid:vec3<u32>){
    if(any(gid.xy>=textureDimensions(visibilityOut))){return;}
    var result=vec2(1.0,0.0);
    let uv=(vec2<f32>(gid.xy)*2+vec2(1))/vec2<f32>(f32(params.render_width),f32(params.render_height));
    let depth=depthSample(depthTex,uv);
    if(params.shadow_quality!=255u && depth<1){
        let world4=params.inv_view_proj*vec4(uv*vec2(2,-2)+vec2(-1,1),depth,1);
        let world=world4.xyz/max(world4.w,1e-6);
        var n=textureSampleLevel(normalTex,linearSampler,uv,0).rgb*2-vec3(1);
        if(dot(n,n)<1e-4){n=vec3(0,-1,0);}
        result=filterWorld(world,normalize(n),vec2<f32>(gid.xy));
    }
    textureStore(visibilityOut,vec2<i32>(gid.xy),vec4(result,0,1));
}
