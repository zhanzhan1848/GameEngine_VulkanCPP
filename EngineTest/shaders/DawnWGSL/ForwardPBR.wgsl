// ForwardPBR.wgsl — Forward PBR shader for Dawn/WebGPU Sponza

struct ViewData {
    viewProjection: mat4x4<f32>,
    invViewProjection: mat4x4<f32>,
    cameraPos: vec4<f32>,
};

struct LightData {
    direction: vec4<f32>,
    color: vec4<f32>,
};

struct ModelData {
    model: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> viewData: ViewData;
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
    @location(4) @interpolate(flat) tSign: u32,
};

fn unpackNormal(packed: u32, colorTSign: u32) -> vec3<f32> {
    let nx = f32(packed & 0xFFFFu);
    let ny = f32((packed >> 16u) & 0xFFFFu);
    let inv = 2.0 / 65535.0;
    let nxy = vec2<f32>(nx * inv - 1.0, ny * inv - 1.0);
    let d = dot(nxy, nxy);
    if (d > 1.0) { return vec3<f32>(0.0, 0.0, 1.0); }
    let z = sqrt(1.0 - d);
    let signs = (colorTSign >> 24u) & 0xFFu;
    let nSign = f32(signs & 0x02u) - 1.0;
    return normalize(vec3<f32>(nxy.x, nxy.y, z * nSign));
}

fn unpackTangent(packed: u32) -> vec3<f32> {
    let tx = f32(packed & 0xFFFFu);
    let ty = f32((packed >> 16u) & 0xFFFFu);
    let inv = 2.0 / 65535.0;
    let txy = vec2<f32>(tx * inv - 1.0, ty * inv - 1.0);
    let d = dot(txy, txy);
    if (d > 1.0) { return vec3<f32>(1.0, 0.0, 0.0); }
    let z = sqrt(1.0 - d);
    return normalize(vec3<f32>(txy.x, txy.y, z));
}

@vertex
fn forward_pbr_vs(input: VSInput) -> VSOutput {
    var output: VSOutput;
    let worldPos = modelData.model * vec4<f32>(input.position, 1.0);
    output.clipPos = viewData.viewProjection * worldPos;
    output.uv = vec2<f32>(input.uv.x, 1.0 - input.uv.y);
    output.worldPos = worldPos.xyz;
    output.normal = unpackNormal(input.packed_normal, input.color_t_sign);
    output.tangent = unpackTangent(input.packed_tangent);
    output.tSign = input.color_t_sign;
    return output;
}

fn fresnelSchlick(cosTheta: f32, F0: vec3<f32>) -> vec3<f32> {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

fn distributionGGX(N: vec3<f32>, H: vec3<f32>, a: f32) -> f32 {
    let a2 = a * a;
    let NdotH = max(dot(N, H), 0.0);
    let d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * d * d + 0.0001);
}

fn geometrySchlickGGX(NdotV: f32, a: f32) -> f32 {
    let k = (a + 1.0) * (a + 1.0) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

fn geometrySmith(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>, a: f32) -> f32 {
    return geometrySchlickGGX(max(dot(N, V), 0.0), a) *
           geometrySchlickGGX(max(dot(N, L), 0.0), a);
}

const PI: f32 = 3.141592653589793;

@fragment
fn forward_pbr_fs(input: VSOutput, @builtin(front_facing) isFrontFace: bool) -> @location(0) vec4<f32> {
    let albedo = textureSample(diffuseMap, matSampler, input.uv).rgb;
    let normalTex = textureSample(normalMap, matSampler, input.uv).rgb;
    let orm = textureSample(ormMap, matSampler, input.uv);
    let ao = orm.r;
    let roughness = max(orm.g, 0.04);
    let metallic = orm.b;

    // Build tangent-space basis
    var N = normalize(input.normal);
    if (!isFrontFace) { N = -N; }
    let T = normalize(input.tangent);
    let B = normalize(cross(N, T));

    // Unpack normal map from [0,1] to [-1,1]
    let tangentNormal = normalTex * 2.0 - 1.0;
    N = normalize(T * tangentNormal.x + B * tangentNormal.y + N * tangentNormal.z);

    let V = normalize(viewData.cameraPos.xyz - input.worldPos);
    let L = normalize(-lightData.direction.xyz);
    let H = normalize(V + L);
    let radiance = lightData.color.rgb * lightData.direction.w;

    let NdotV = max(dot(N, V), 0.001);
    let NdotL = max(dot(N, L), 0.0);

    // PBR: Cook-Torrance BRDF
    let F0 = mix(vec3<f32>(0.04, 0.04, 0.04), albedo, metallic);
    let fresnel = fresnelSchlick(max(dot(H, V), 0.0), F0);
    let ndf = distributionGGX(N, H, roughness);
    let geometry = geometrySmith(N, V, L, roughness);

    let numerator = ndf * geometry * fresnel;
    let denominator = 4.0 * NdotV * NdotL + 0.0001;
    let specular = numerator / denominator;

    let kS = fresnel;
    let kD = (vec3<f32>(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);

    let Lo = (kD * albedo / PI + specular) * radiance * NdotL;

    // Ambient with occlusion
    let ambient = vec3<f32>(0.03, 0.03, 0.03) * albedo * ao;
    var color = ambient + Lo;

    // Reinhard tone mapping
    color = color / (color + vec3<f32>(1.0));
    // Gamma correction
    color = pow(color, vec3<f32>(1.0 / 2.2));

    return vec4<f32>(color, 1.0);
}
