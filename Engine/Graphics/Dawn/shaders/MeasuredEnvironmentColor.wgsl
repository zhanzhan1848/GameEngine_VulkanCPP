// Frozen deferred measured-environment branch, separate from direct GI input.
struct EnvironmentColorParams {inverseVP:mat4x4<f32>,camera:vec4<f32>}
@group(0) @binding(0) var depth:texture_depth_2d;
@group(0) @binding(1) var normals:texture_2d<f32>;
@group(0) @binding(2) var orm:texture_2d<f32>;
@group(0) @binding(3) var albedo:texture_2d<f32>;
@group(0) @binding(4) var prefilter:texture_cube<f32>;
@group(0) @binding(5) var brdf:texture_2d<f32>;
@group(0) @binding(6) var linearClamp:sampler;
@group(0) @binding(7) var outputColor:texture_storage_2d<rgba16float,write>;
@group(0) @binding(8) var<uniform> params:EnvironmentColorParams;
fn measuredEnvironmentColor(base:vec3<f32>,material:vec3<f32>,n:vec3<f32>,v:vec3<f32>,prefiltered:vec3<f32>,integral:vec2<f32>)->vec3<f32>{
 let roughness=max(material.g,.04);let noV=max(dot(n,v),0.);let f0=mix(vec3<f32>(.04),base,material.b);
 let f=f0+(max(vec3<f32>(1.-roughness),f0)-f0)*pow(clamp(1.-noV,0.,1.),5.);
 return prefiltered*(f*integral.x+integral.y)*.35*clamp(material.r,0.,1.);
}
@compute @workgroup_size(8,8)
fn measured_environment_color(@builtin(global_invocation_id) gid:vec3<u32>){
 let size=textureDimensions(outputColor);if(any(gid.xy>=size)){return;}let pixel=vec2<i32>(gid.xy);let z=textureLoad(depth,pixel,0);
 if(z>=1.){textureStore(outputColor,pixel,vec4<f32>(0));return;}
 let uv=(vec2<f32>(gid.xy)+.5)/vec2<f32>(size);let world=params.inverseVP*vec4<f32>(uv.x*2.-1.,1.-uv.y*2.,z,1.);
 let v=normalize(params.camera.xyz-world.xyz/max(world.w,1e-6));let n=normalize(textureLoad(normals,pixel,0).xyz*2.-1.);let m=textureLoad(orm,pixel,0).rgb;let base=textureLoad(albedo,pixel,0).rgb;
 let roughness=max(m.g,.04);let noV=max(dot(n,v),0.);let lod=clamp(roughness,0.,1.)*f32(textureNumLevels(prefilter)-1u);
 let radiance=textureSampleLevel(prefilter,linearClamp,reflect(-v,n),lod).rgb;let integral=textureSampleLevel(brdf,linearClamp,vec2<f32>(noV,roughness),0).rg;
 textureStore(outputColor,pixel,vec4<f32>(measuredEnvironmentColor(base,m,n,v,radiance,integral),1));
}
