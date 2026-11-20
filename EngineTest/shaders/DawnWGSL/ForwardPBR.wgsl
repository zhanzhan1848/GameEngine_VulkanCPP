// ForwardPBR.wgsl — Forward PBR shader for Dawn/WebGPU Sponza

struct ViewData {
    viewMatrix: mat4x4<f32>,
    projectionMatrix: mat4x4<f32>,
    invViewProjection: mat4x4<f32>,
    cameraPos: vec4<f32>,
    rasterJitter: vec4<f32>,
};

struct LightData {
    direction: vec4<f32>,
    color: vec4<f32>,
};

struct ModelData {
    model: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> viewData: ViewData;
@group(0) @binding(1) var shadowVisibility: texture_2d<f32>;
@group(0) @binding(2) var shadowSampler: sampler;
@group(0) @binding(5) var<uniform> lightData: LightData;

@group(1) @binding(1) var diffuseMap: texture_2d<f32>;
@group(1) @binding(2) var normalMap: texture_2d<f32>;
@group(1) @binding(3) var ormMap: texture_2d<f32>;
@group(1) @binding(4) var matSampler: sampler;

@group(2) @binding(0) var<uniform> modelData: ModelData;

struct VSInput {
    @location(0) position: vec3<f32>,
    @location(1) color_t_sign: u32,
    @location(2) packed_normal: u32,
    @location(3) packed_tangent: u32,
    @location(4) uv: vec2<f32>,
};

struct VSOutput {
    @builtin(position) clipPos: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) worldPos: vec3<f32>,
    @location(2) normal: vec3<f32>,
    @location(3) tangent: vec3<f32>,
    @location(4) bitangent: vec3<f32>,
};

fn unpackNormal(packed: u32, colorTSign: u32) -> vec3<f32> {
    // RenderMesh copies raw little-endian u16[2]; unlike Nanite it does not
    // repack X into the high half. Keep this vertex-fetch contract explicit.
    let nx = f32(packed & 0xFFFFu);
    let ny = f32((packed >> 16u) & 0xFFFFu);
    let inv = 2.0 / 65535.0;
    let nxy = vec2<f32>(nx * inv - 1.0, ny * inv - 1.0);
    let d = dot(nxy, nxy);
    if (d > 1.0) { return vec3<f32>(0.0, 0.0, 1.0); }
    let z = sqrt(1.0 - d);
    let signs = (colorTSign >> 24u) & 0xFFu;
    let nSign = f32(signs & 0x02u) - 1.0;
    return vec3<f32>(nxy.x, nxy.y, z * nSign);
}

// Tangent Z uses bit 2; bit 1 belongs to the normal and bit 0 to handedness.
fn unpackTangent(packed: u32, colorTSign: u32) -> vec3<f32> {
    let xy = vec2<f32>(f32(packed & 65535u), f32(packed >> 16u)) * (2.0 / 65535.0) - vec2<f32>(1.0);
    // Preserve the in-plane direction at the quantized unit-disk boundary.
    if (dot(xy, xy) > 1.0) { return normalize(vec3<f32>(xy, 0.0)); }
    return unpackNormal(packed, colorTSign >> 1u);
}

@vertex
fn forward_pbr_vs(input: VSInput) -> VSOutput {
    var output: VSOutput;
    let worldPos = modelData.model * vec4<f32>(input.position, 1.0);
    // Preserve the frozen vertex shader's world -> view -> projection order.
    let viewPos=viewData.viewMatrix * worldPos;
    let clip=viewData.projectionMatrix * viewPos;
    output.clipPos = applyRasterJitter(clip,viewData.rasterJitter.xy);
    output.uv = vec2<f32>(input.uv.x, 1.0 - input.uv.y);
    output.worldPos = worldPos.xyz;
    // The Sponza host supplies identity model transforms (baked world vertices).
    // Orthogonalize before interpolation, as in the frozen GPU-driven vertex path.
    let N = normalize(unpackNormal(input.packed_normal, input.color_t_sign));
    var T = normalize(unpackTangent(input.packed_tangent, input.color_t_sign));
    if (abs(dot(T, N)) > 0.999) {
        let up = select(vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(1.0, 0.0, 0.0), abs(N.y) < 0.999);
        T = normalize(cross(up, N));
    } else {
        T = normalize(T - dot(T, N) * N);
    }
    let frameSign = select(1.0, -1.0, (input.color_t_sign & 0x01000000u) != 0u);
    output.normal = N;
    output.tangent = T;
    output.bitangent = cross(N, T) * frameSign;
    return output;
}

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

// Sponza uses a repeat sampler; match frozen UV wrapping without differentiating fract.
fn materialAlbedo(uv: vec2<f32>) -> vec4<f32> {
    let dx = dpdx(uv);
    let dy = dpdy(uv);
    return textureSampleGrad(diffuseMap, matSampler, fract(uv), dx, dy);
}

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

fn materialNormal(input: VSOutput) -> vec3<f32> {
    // Frozen tangent/albedo-mip path keeps normal and ORM at LOD 0.
    let normalTex = textureSampleLevel(normalMap, matSampler, input.uv, 0.0).rgb;
    // Match the frozen visible-side orientation, independent of mesh winding.
    let geomN = normalize(cross(dpdy(input.worldPos), dpdx(input.worldPos)));
    var N = normalize(input.normal);
    if (dot(N, geomN) < 0.0) { N = -N; }
    let T = normalize(input.tangent);
    let frameSign = select(-1.0, 1.0, dot(cross(normalize(input.normal), T), input.bitangent) >= 0.0);
    let B = normalize(cross(N, T)) * frameSign;

    // Frozen normal sampling rejects near-black texels instead of treating
    // them as a (-1,-1,-1) tangent-space direction.
    if (length(normalTex) > 0.1) {
        let tangentNormal = normalTex * 2.0 - 1.0;
        return normalize(mat3x3<f32>(T, B, N) * tangentNormal);
    }
    return N;

}

@fragment
fn normal_prepass(input: VSOutput) -> @location(0) vec4<f32> {
    return vec4<f32>(materialNormal(input) * 0.5 + vec3<f32>(0.5), 1.0);
}

@fragment
fn forward_pbr_fs(input: VSOutput) -> @location(0) vec4<f32> {
    let albedo = materialAlbedo(input.uv).rgb;
    let orm = textureSampleLevel(ormMap, matSampler, input.uv, 0.0);
    let ao = orm.r;
    let roughness = max(orm.g, 0.04);
    let metallic = orm.b;

    let N = materialNormal(input);

    let V = normalize(viewData.cameraPos.xyz - input.worldPos);
    let L = normalize(-lightData.direction.xyz);
    let radiance = lightData.color.rgb * lightData.direction.w;
    let shadowUV = input.clipPos.xy / (vec2<f32>(textureDimensions(shadowVisibility)) * 2.0);
    let visibility = textureSampleLevel(shadowVisibility, shadowSampler, shadowUV, 0).r;
    let Lo = directSun(albedo, roughness, metallic, N, V, L, radiance, visibility);

    // Ambient with occlusion
    let ambient = vec3<f32>(0.03, 0.03, 0.03) * albedo * ao;
    // Linear HDR survives in RGBA16F; exposure/ACES/gamma run once in the
    // native post-process pass after the scene has finished rendering.
    return vec4<f32>(ambient + Lo, 1.0);
}

// Screen measured DDGI has a standalone entry and a full SPGI/environment
// composition entry. No constant ambient in either measured path.
@group(0) @binding(3) var screenDiffuseGI: texture_2d<f32>;
@group(0) @binding(4) var screenSpecularGI: texture_2d<f32>;
struct GatherPrepassOutput { @location(0) normal:vec4<f32>, @location(1) orm:vec4<f32> }
@fragment fn gather_prepass(input:VSOutput)->GatherPrepassOutput {
    return GatherPrepassOutput(vec4<f32>(materialNormal(input)*.5+.5,1),textureSampleLevel(ormMap,matSampler,input.uv,0));
}
struct EnvironmentPrepassOutput { @location(0) normal:vec4<f32>, @location(1) orm:vec4<f32>, @location(2) albedo:vec4<f32> }
@fragment fn environment_prepass(input:VSOutput)->EnvironmentPrepassOutput {
 return EnvironmentPrepassOutput(vec4<f32>(materialNormal(input)*.5+.5,1),textureSampleLevel(ormMap,matSampler,input.uv,0),vec4<f32>(clamp(materialAlbedo(input.uv).rgb,vec3<f32>(0),vec3<f32>(1)),1));
}
@group(0) @binding(7) var screenEnvironment:texture_2d<f32>;
@group(0) @binding(8) var screenEnvironmentVisibility:texture_2d<f32>;
@group(0) @binding(9) var<uniform> environmentPolicy:vec4<f32>;
@fragment fn forward_gi_fs(input:VSOutput)->@location(0) vec4<f32> {
    let albedo=clamp(materialAlbedo(input.uv).rgb,vec3<f32>(0),vec3<f32>(1));
    let orm=textureSampleLevel(ormMap,matSampler,input.uv,0);let roughness=clamp(orm.g,.04,1.0);let metallic=orm.b;
    let n=materialNormal(input);let v=normalize(viewData.cameraPos.xyz-input.worldPos);let l=normalize(-lightData.direction.xyz);
    let uv=input.clipPos.xy/(vec2<f32>(textureDimensions(shadowVisibility))*2.0);
    let visibility=textureSampleLevel(shadowVisibility,shadowSampler,uv,0).r;
    let direct=directSun(albedo,roughness,metallic,n,v,l,lightData.color.rgb*lightData.direction.w,visibility);
    let diffuse=textureSampleLevel(screenDiffuseGI,shadowSampler,uv,0).rgb;
    let glossy=textureSampleLevel(screenSpecularGI,shadowSampler,uv,0);
    return vec4<f32>(composeMeasuredGI(direct,albedo,roughness,metallic,diffuse,glossy),1);
}

// Frozen FusionIndirect mode 0/1: confidence replaces irradiance before albedo/PI.
@group(0) @binding(6) var screenProbeGI:texture_2d<f32>;
@group(0) @binding(12) var deferredORM:texture_2d<f32>;
@group(0) @binding(13) var deferredAlbedo:texture_2d<f32>;
@group(0) @binding(14) var deferredDirect:texture_2d<f32>;
@fragment fn forward_spgi_fs(input:VSOutput)->@location(0) vec4<f32> {
    let pixel=vec2<i32>(input.clipPos.xy);let size=textureDimensions(deferredDirect);
    let albedo=textureLoad(deferredAlbedo,pixel,0).rgb;let orm=textureLoad(deferredORM,pixel,0);
    let roughness=clamp(orm.g,.04,1.0);let metallic=orm.b;
    let uv=input.clipPos.xy/vec2<f32>(size);
    let direct=textureLoad(deferredDirect,pixel,0).rgb;
    let ddgi=textureSampleLevel(screenDiffuseGI,shadowSampler,uv,0).rgb;
    let spgi=textureSampleLevel(screenProbeGI,shadowSampler,uv,0);
    let diffuse=blendProbeIrradiance(ddgi,spgi);
    let glossy=textureSampleLevel(screenSpecularGI,shadowSampler,uv,0);
    return vec4<f32>(composeMeasuredEnvironmentGI(direct,albedo,roughness,metallic,diffuse,glossy,textureSampleLevel(screenEnvironment,shadowSampler,uv,0),textureSampleLevel(screenEnvironmentVisibility,shadowSampler,uv,0).r,environmentPolicy),1);
}

// Frozen FusionIndirect + FusionComposite equations with unsupported screen
// sources absent; this helper is shared by the scene and numerical fixture.
fn composeMeasuredGI(direct:vec3<f32>,albedo:vec3<f32>,roughness:f32,metallic:f32,diffuse:vec3<f32>,glossy:vec4<f32>)->vec3<f32> {
    let noV=clamp(glossy.a,.05,1.0);let f0=mix(vec3<f32>(.04),albedo,metallic);
    let f=f0+(1.0-f0)*pow(1.0-noV,5.0);
    let r=roughness*vec4<f32>(-1,-.0275,-.572,.022)+vec4<f32>(1,.0425,1.04,-.04);
    let a004=min(r.x*r.x,exp2(-9.28*noV))*r.x+r.y;
    let ab=vec2<f32>(-1.04,1.04)*a004+r.zw;
    let reflection=glossy.rgb*(f0*ab.x+ab.y)*(1.0-roughness*roughness)*.75;
    let indirect=albedo*diffuse*.31830988;
    return clamp(direct+indirect*(1.0-metallic)*(1.0-f)+reflection,vec3<f32>(0),vec3<f32>(64));
}

// Unit-preserving source replacement, including darker valid SPGI estimates.
fn blendProbeIrradiance(ddgi:vec3<f32>,spgi:vec4<f32>)->vec3<f32>{return mix(ddgi,spgi.rgb,clamp(spgi.a,0.0,.9));}

// Match frozen raster ordering: project first, then add jitter times clip.w.
fn applyRasterJitter(clip:vec4<f32>,jitter:vec2<f32>)->vec4<f32>{return vec4<f32>(clip.xy+jitter*clip.w,clip.zw);}

fn composeMeasuredEnvironmentGI(direct:vec3<f32>,albedo:vec3<f32>,roughness:f32,metallic:f32,diffuse:vec3<f32>,glossy:vec4<f32>,environment:vec4<f32>,visibility:f32,policy:vec4<f32>)->vec3<f32>{
 let noV=clamp(glossy.a,.05,1.0);let f0=mix(vec3<f32>(.04),albedo,metallic);let f=f0+(1.0-f0)*pow(1.0-noV,5.0);
 let r=roughness*vec4<f32>(-1,-.0275,-.572,.022)+vec4<f32>(1,.0425,1.04,-.04);let a004=min(r.x*r.x,exp2(-9.28*noV))*r.x+r.y;let ab=vec2<f32>(-1.04,1.04)*a004+r.zw;
 let glossyReflection=glossy.rgb*(f0*ab.x+ab.y)*(1.0-roughness*roughness)*.75;
 let ownership=policy.x*clamp(environment.a,0.,1.);let geometryVisibility=select(1.,clamp(visibility,0.,1.),policy.w>.5);let measured=environment.rgb*geometryVisibility*policy.y;
 var reflection=mix(glossyReflection,measured,ownership);var indirect=albedo*diffuse*.31830988;
 if(policy.z>.5){indirect=vec3<f32>(0);reflection=measured*ownership;}
 return clamp(direct+indirect*(1.0-metallic)*(1.0-f)+reflection,vec3<f32>(0),vec3<f32>(64));
}
