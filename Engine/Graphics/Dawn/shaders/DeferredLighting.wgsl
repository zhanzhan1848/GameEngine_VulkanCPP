// DeferredLighting.wgsl — PBR deferred lighting pass

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var gbuffer0: texture_2d<f32>; // Position + depth
@group(0) @binding(1) var gbuffer1: texture_2d<f32>; // Normal + roughness
@group(0) @binding(2) var gbuffer2: texture_2d<f32>; // Albedo + metallic
@group(0) @binding(3) var gbuffer3: texture_2d<f32>; // Emissive + AO
@group(0) @binding(4) var depthTex: texture_2d<f32>;
@group(0) @binding(5) var sampler0: sampler;

struct LightData {
    direction: vec3<f32>,
    color: vec3<f32>,
    intensity: f32,
};

@group(0) @binding(6) var<uniform> lightData: LightData;
@group(0) @binding(7) var<uniform> globalData: GlobalShaderData;

const PI: f32 = 3.14159265358979323846;

fn DistributionGGX(N: vec3<f32>, H: vec3<f32>, roughness: f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let NdotH = max(dot(N, H), 0.0);
    let NdotH2 = NdotH * NdotH;
    let denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom + 0.0000001);
}

fn GeometrySchlickGGX(NdotV: f32, k: f32) -> f32 {
    return NdotV / (NdotV * (1.0 - k) + k);
}

fn GeometrySmith(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>, roughness: f32) -> f32 {
    let r = roughness + 1.0;
    let k = (r * r) / 8.0;
    let NdotV = max(dot(N, V), 0.0);
    let NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

fn FresnelSchlick(cosTheta: f32, F0: vec3<f32>) -> vec3<f32> {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

@fragment
fn deferred_lighting_fs(input: VertexOutput) -> @location(0) vec4<f32> {
    let texelSize = vec2<f32>(1.0 / f32(textureDimensions(gbuffer0).x), 1.0 / f32(textureDimensions(gbuffer0).y));

    let g0 = textureSample(gbuffer0, sampler0, input.uv);
    let g1 = textureSample(gbuffer1, sampler0, input.uv);
    let g2 = textureSample(gbuffer2, sampler0, input.uv);

    let worldPos = g0.xyz;
    let depth = g0.w;
    let normal = normalize(g1.xyz * 2.0 - 1.0);
    let roughness = g1.w;
    let albedo = g2.xyz;
    let metallic = g2.w;

    let viewDir = normalize(globalData.CameraPositionAndViewWidth.xyz - worldPos);

    let F0 = mix(vec3<f32>(0.04), albedo, metallic);

    let L = normalize(-lightData.direction);
    let H = normalize(viewDir + L);
    let NdotL = max(dot(normal, L), 0.0);

    // Specular BRDF
    let D = DistributionGGX(normal, H, roughness);
    let G = GeometrySmith(normal, viewDir, L, roughness);
    let F = FresnelSchlick(max(dot(H, viewDir), 0.0), F0);

    let kS = F;
    let kD = (vec3<f32>(1.0) - kS) * (1.0 - metallic);

    let NdotV = max(dot(normal, viewDir), 0.0);
    let numerator = D * G * F;
    let denominator = 4.0 * NdotV * NdotL + 0.001;
    let specular = numerator / denominator;

    let Lo = (kD * albedo / PI + specular) * lightData.color * lightData.intensity * NdotL;

    // Ambient
    let ambient = vec3<f32>(0.03) * albedo;

    var color = ambient + Lo;

    // Simple tone mapping (reinhard)
    color = color / (color + vec3<f32>(1.0));

    // Gamma correction
    color = pow(color, vec3<f32>(1.0 / 2.2));

    return vec4<f32>(color, 1.0);
}
