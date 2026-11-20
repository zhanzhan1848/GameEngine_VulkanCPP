// Native deferred direct-light producer. Quantized GBuffer is authoritative.
struct Params { inverseVP:mat4x4<f32>,camera:vec4<f32>,lightDirection:vec4<f32>,lightColor:vec4<f32> }
@group(0) @binding(0) var depth:texture_depth_2d;
@group(0) @binding(1) var normals:texture_2d<f32>;
@group(0) @binding(2) var orm:texture_2d<f32>;
@group(0) @binding(3) var albedo:texture_2d<f32>;
@group(0) @binding(4) var shadow:texture_2d<f32>;
@group(0) @binding(5) var linearClamp:sampler;
@group(0) @binding(6) var outputColor:texture_storage_2d<rgba16float,write>;
@group(0) @binding(7) var<uniform> params:Params;
fn fresnelSchlick(cosTheta: f32, F0: vec3<f32>) -> vec3<f32> {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

fn distributionGGX(N: vec3<f32>, H: vec3<f32>, a: f32) -> f32 {
    let a2 = a * a * a * a;
    let NdotH = max(dot(N, H), 0.0);
    let d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

fn geometrySchlickGGX(NdotV: f32, a: f32) -> f32 {
    // Frozen Vulkan uses roughness itself as k (its remapped local is unused).
    let k = a;
    return NdotV / (NdotV * (1.0 - k) + k);
}

fn geometrySmith(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>, a: f32) -> f32 {
    return geometrySchlickGGX(max(dot(N, V), 0.0), a) *
           geometrySchlickGGX(max(dot(N, L), 0.0), a);
}

const PI: f32 = 3.141592653589793;

// Frozen DeferredLighting.frag direct-sun contract. Radiance is the
// host light color/intensity (3 for the Sponza reference), before the shared
// direct/Surface Cache scale of 50. Keep this separate from display exposure.
fn directSun(albedo: vec3<f32>, roughnessInput: f32, metallic: f32,
             N: vec3<f32>, V: vec3<f32>, L: vec3<f32>,
             radiance: vec3<f32>, visibility: f32) -> vec3<f32> {
    let roughness = max(roughnessInput, 0.04);
    let H = normalize(V + L);
    let NdotV = max(dot(N, V), 0.0);
    let NdotL = max(dot(N, L), 0.0);
    let F0 = mix(vec3<f32>(0.04), albedo, metallic);
    let F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    let specular = distributionGGX(N, H, roughness) * geometrySmith(N, V, L, roughness) * F
        / (4.0 * NdotV * NdotL + 0.001);
    let diffuse = (vec3<f32>(1.0) - F) * (1.0 - metallic) * albedo / PI;
    return (diffuse + specular) * radiance * NdotL * visibility * 50.0;
}


fn measuredSky(direction:vec3<f32>,sun:vec3<f32>,color:vec3<f32>)->vec3<f32>{
 let sky=mix(vec3<f32>(4.40,4.20,3.80),vec3<f32>(5.50,5.76,6.00),pow(clamp(direction.y,0.,1.),.45));
 let lum=dot(color,vec3<f32>(.2126,.7152,.0722));let reference=vec3<f32>(1.,.85,.6);
 var halo=reference;if(lum>1e-4){halo=color*(dot(reference,vec3<f32>(.2126,.7152,.0722))/lum);}
 return sky+halo*pow(clamp(dot(direction,sun),0.,1.),16.)*1.2;
}
@compute @workgroup_size(8,8)
fn measured_deferred_direct(@builtin(global_invocation_id) gid:vec3<u32>){
 let size=textureDimensions(outputColor);if(any(gid.xy>=size)){return;}
 let pixel=vec2<i32>(gid.xy);let uv=(vec2<f32>(gid.xy)+.5)/vec2<f32>(size);let z=textureLoad(depth,pixel,0);
 let world=params.inverseVP*vec4<f32>(uv.x*2.-1.,1.-uv.y*2.,z,1.);let position=world.xyz/max(world.w,1e-6);let light=normalize(-params.lightDirection.xyz);
 if(z>=1.){textureStore(outputColor,pixel,vec4<f32>(measuredSky(normalize(position-params.camera.xyz),light,params.lightColor.rgb*params.lightDirection.w),1));return;}
 let n=normalize(textureLoad(normals,pixel,0).xyz*2.-1.);let v=normalize(params.camera.xyz-position);let material=textureLoad(orm,pixel,0).rgb;let base=textureLoad(albedo,pixel,0).rgb;
 let visibility=textureSampleLevel(shadow,linearClamp,uv,0).r;
 textureStore(outputColor,pixel,vec4<f32>(directSun(base,material.g,material.b,n,v,light,params.lightColor.rgb*params.lightDirection.w,visibility),1));
}
