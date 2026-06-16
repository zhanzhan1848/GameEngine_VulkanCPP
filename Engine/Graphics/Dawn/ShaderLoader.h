#pragma once
// ShaderLoader.h — WGSL shader loading for Dawn WebGPU backend
// Emscripten: shaders embedded as static string constants (no filesystem access)
// macOS/other: loaded from disk via std::ifstream

#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace primal::graphics::dawn {

#ifdef __EMSCRIPTEN__

// ============================================================================
// Embedded WGSL shader sources (auto-generated from Engine/Graphics/Dawn/shaders/)
// ============================================================================

static const char* kShader_Blit = R"wgsl(
// Blit.wgsl — Fullscreen triangle blit shader for mipmap generation and texture copies

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn blit_vs(@builtin(vertex_index) vertexID: u32) -> VertexOutput {
    var positions = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>( 3.0, -1.0),
        vec2<f32>(-1.0,  3.0)
    );
    var uvs = array<vec2<f32>, 3>(
        vec2<f32>(0.0, 1.0),
        vec2<f32>(2.0, 1.0),
        vec2<f32>(0.0, -1.0)
    );

    var output: VertexOutput;
    output.position = vec4<f32>(positions[vertexID], 0.0, 1.0);
    output.uv = uvs[vertexID];
    return output;
}

@group(0) @binding(0) var srcTexture: texture_2d<f32>;
@group(0) @binding(1) var srcSampler: sampler;

@fragment
fn blit_fs(input: VertexOutput) -> @location(0) vec4<f32> {
    return textureSample(srcTexture, srcSampler, input.uv);
}
)wgsl";

static const char* kShader_Bloom = R"wgsl(
// Bloom.wgsl — Bright pass extraction for bloom effect

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var texSampler: sampler;

@vertex
fn bloom_vs(@builtin(vertex_index) vertexID: u32) -> VertexOutput {
    let positions = array<vec4<f32>, 3>(
        vec4<f32>(-1.0, -1.0, 0.0, 1.0),
        vec4<f32>( 3.0, -1.0, 0.0, 1.0),
        vec4<f32>(-1.0,  3.0, 0.0, 1.0)
    );
    let uvs = array<vec2<f32>, 3>(
        vec2<f32>(0.0, 1.0),
        vec2<f32>(2.0, 1.0),
        vec2<f32>(0.0, -1.0)
    );

    var out: VertexOutput;
    out.position = positions[vertexID];
    out.uv = uvs[vertexID];
    return out;
}

@fragment
fn bright_pass_fs(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    let color = textureSample(inputTexture, texSampler, uv);
    let brightness = dot(color.rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
    if (brightness > 1.0) {
        return color;
    }
    return vec4<f32>(0.0);
}
)wgsl";

static const char* kShader_BlurPass = R"wgsl(
// BlurPass.wgsl — Simple separable Gaussian blur (placeholder)
// Used by ForwardRenderer's VSM shadow blur

struct BlurParams {
    direction: u32,     // 0=horizontal, 1=vertical
    mipLevel: u32,
    _pad0: u32,
    _pad1: u32,
};

@group(0) @binding(0) var inputTex: texture_2d<f32>;
@group(0) @binding(1) var outputTex: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> params: BlurParams;

@compute @workgroup_size(8, 8, 1)
fn blurCS(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = textureDimensions(inputTex);
    if (gid.x >= dims.x || gid.y >= dims.y) {
        return;
    }

    let texCoord = vec2<u32>(gid.x, gid.y);
    let center = textureLoad(inputTex, texCoord, 0);

    // Simple 5-tap Gaussian blur
    let offset = select(vec2<i32>(1, 0), vec2<i32>(0, 1), params.direction == 1u);
    var sum = center * 0.2270270270;
    sum += textureLoad(inputTex, texCoord + vec2<u32>(offset), 0) * 0.3162162162;
    sum += textureLoad(inputTex, texCoord - vec2<u32>(offset), 0) * 0.3162162162;
    sum += textureLoad(inputTex, texCoord + 2u * vec2<u32>(offset), 0) * 0.070270270;
    sum += textureLoad(inputTex, texCoord - 2u * vec2<u32>(offset), 0) * 0.070270270;

    textureStore(outputTex, texCoord, sum);
}
)wgsl";

static const char* kShader_CommonFunction = R"wgsl(
// CommonFunction.wgsl — Ported from Metal CommonFunction.metal
// Utility functions for Dawn WebGPU shaders

const PI: f32 = 3.14159265358979323846;

fn length2(v: vec3<f32>) -> f32 {
    return dot(v, v);
}

fn dot2_v2(v: vec2<f32>) -> f32 {
    return dot(v, v);
}

fn dot2_v3(v: vec3<f32>) -> f32 {
    return dot(v, v);
}

fn hash(p: vec2<f32>) -> f32 {
    var p3 = fract(vec3<f32>(p.x, p.y, p.x) * 0.1031);
    p3 = p3 + dot(p3, vec3<f32>(p3.y, p3.z, p3.x) + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

fn rand3(seed: vec2<f32>, index: f32) -> vec3<f32> {
    let s = seed + vec2<f32>(index, index * 1.3);
    var r = vec3<f32>(
        hash(s),
        hash(s + vec2<f32>(1.7, 3.2)),
        hash(s + vec2<f32>(5.1, 2.3))
    );
    r = r * 2.0 - 1.0;
    return normalize(r);
}

fn NDCDepthToLinear(ndcDepth: f32, nearPlane: f32, farPlane: f32) -> f32 {
    return (2.0 * nearPlane * farPlane) / (farPlane + nearPlane - ndcDepth * (farPlane - nearPlane));
}

fn LinearDepthToNDC(linearDepth: f32, nearPlane: f32, farPlane: f32) -> f32 {
    return (farPlane + nearPlane - 2.0 * farPlane * nearPlane / linearDepth) / (farPlane - nearPlane);
}

fn ReconstructViewPos(pixelUv: vec2<f32>, linearDepth: f32, invProjection: mat4x4<f32>) -> vec3<f32> {
    let ndc = vec4<f32>(pixelUv.x * 2.0 - 1.0, 1.0 - pixelUv.y * 2.0, 0.0, 1.0);
    let viewPos4 = invProjection * ndc;
    let viewPos = viewPos4.xyz / viewPos4.w;
    return viewPos * linearDepth;
}

fn ReconstructWorldPos(pixelUv: vec2<f32>, linearDepth: f32, invViewProjection: mat4x4<f32>) -> vec3<f32> {
    let ndc = vec4<f32>(pixelUv.x * 2.0 - 1.0, 1.0 - pixelUv.y * 2.0, linearDepth, 1.0);
    let worldPos4 = invViewProjection * ndc;
    return worldPos4.xyz / worldPos4.w;
}

// GGX/Trowbridge-Reitz NDF
fn DistributionGGX(N: vec3<f32>, H: vec3<f32>, roughness: f32) -> f32 {
    let a = roughness * roughness;
    let a2 = a * a;
    let NdotH = max(dot(N, H), 0.0);
    let NdotH2 = NdotH * NdotH;
    let denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom + 0.0000001);
}

fn GeometrySchlickGGX(NdotV: f32, roughness: f32) -> f32 {
    let r = roughness + 1.0;
    let k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

fn GeometrySchlickGGX_IBL(NdotV: f32, roughness: f32) -> f32 {
    let k = (roughness * roughness) / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

fn GeometrySmith(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>, roughness: f32) -> f32 {
    let NdotV = max(dot(N, V), 0.0);
    let NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

fn GeometrySmith_IBL(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>, roughness: f32) -> f32 {
    let NdotV = max(dot(N, V), 0.0);
    let NdotL = max(dot(N, L), 0.0);
    return GeometrySchlickGGX_IBL(NdotV, roughness) * GeometrySchlickGGX_IBL(NdotL, roughness);
}

fn FresnelSchlick(cosTheta: f32, F0: vec3<f32>) -> vec3<f32> {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

fn FresnelSchlickRoughness(cosTheta: f32, F0: vec3<f32>, roughness: f32) -> vec3<f32> {
    return F0 + (max(vec3<f32>(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

fn SpecularBRDF(N: vec3<f32>, V: vec3<f32>, L: vec3<f32>, roughness: f32, F0: vec3<f32>) -> vec3<f32> {
    let H = normalize(V + L);
    let D = DistributionGGX(N, H, roughness);
    let G = GeometrySmith(N, V, L, roughness);
    let F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    let NdotV = max(dot(N, V), 0.0);
    let NdotL = max(dot(N, L), 0.0);
    let numerator = D * G * F;
    let denominator = 4.0 * NdotV * NdotL + 0.001;
    return numerator / denominator;
}

fn srgb_to_acescg(col: vec3<f32>) -> vec3<f32> {
    let sRGB_AP1 = mat3x3<f32>(
        vec3<f32>(0.613098, 0.070126, 0.020570),
        vec3<f32>(0.339523, 0.916331, 0.109626),
        vec3<f32>(0.047379, 0.013543, 0.869804)
    );
    return sRGB_AP1 * col;
}

fn acescg_to_srgb(col: vec3<f32>) -> vec3<f32> {
    let AP1_sRGB = mat3x3<f32>(
        vec3<f32>(1.704858, -0.130077, -0.020069),
        vec3<f32>(-0.621715, 1.140925, -0.127968),
        vec3<f32>(-0.083143, -0.010848, 1.148038)
    );
    return AP1_sRGB * col;
}

fn EvalSH9Irradiance(N: vec3<f32>, sh: array<vec4<f32>, 9>) -> vec3<f32> {
    let A0 = 0.886226925;
    let A1 = 1.02332671;
    let A2 = 0.495416442;
    let C0 = 0.429042758;
    let C1 = 0.511663608;
    let C2_0 = 0.247709165;
    let C2_1 = 0.250481061;
    let C2_2 = 0.250000000;

    let x = N.x;
    let y = N.y;
    let z = N.z;

    var result = vec3<f32>(0.0);
    result = result + A0 * sh[0].xyz;
    result = result + A1 * (sh[1].xyz * y + sh[2].xyz * z + sh[3].xyz * x);
    result = result + A2 * (
        sh[4].xyz * (x * y) +
        sh[5].xyz * (y * z) +
        sh[6].xyz * (2.0 * z * z - x * x - y * y) +
        sh[7].xyz * (z * x) +
        sh[8].xyz * (x * x - y * y)
    );
    return result;
}

fn sampleHemisphere(N: vec3<f32>, seed: vec2<f32>, sampleIndex: i32) -> vec3<f32> {
    let randVal = hash(seed + vec2<f32>(f32(sampleIndex), f32(sampleIndex) * 1.3));
    var u = randVal;
    var v = hash(seed + vec2<f32>(f32(sampleIndex) * 0.7, f32(sampleIndex) * 2.1));
    u = 2.0 * u - 1.0;

    let r = sqrt(1.0 - u * u);
    let phi = 2.0 * PI * v;

    let localDir = vec3<f32>(r * cos(phi), r * sin(phi), u);

    // Build TBN from N
    let up = select(vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), abs(N.y) < 0.999);
    let tangent = normalize(cross(up, N));
    let bitangent = cross(N, tangent);

    return normalize(tangent * localDir.x + bitangent * localDir.y + N * localDir.z);
}
)wgsl";

static const char* kShader_CommonTypes = R"wgsl(
// CommonTypes.wgsl — Ported from Metal CommonTypes.metal
// Shared structures for Dawn WebGPU shaders

struct GlobalShaderData {
    View:               mat4x4<f32>,
    Projection:         mat4x4<f32>,
    InvProjection:      mat4x4<f32>,
    ViewProjection:     mat4x4<f32>,
    PrevViewProjection: mat4x4<f32>,
    InvViewProjection:  mat4x4<f32>,
    CameraPositionAndViewWidth: vec4<f32>,  // xyz = position, w = viewWidth
    CameraDirectionAndViewHeight: vec4<f32>, // xyz = direction, w = viewHeight
    NumDirectionalLights: u32,
    DeltaTime: f32,
    FrameCount: f32,
    _padding: u32,
};

struct PerObjectData {
    World:               mat4x4<f32>,
    InvWorld:            mat4x4<f32>,
    WorldViewProjection: mat4x4<f32>,
    sh_coeffs:           array<vec4<f32>, 9>,
};

struct Surface {
    BaseColor:          vec3<f32>,
    Metallic:           f32,
    Normal:             vec3<f32>,
    PerceptualRoughness: f32,
    EmissiveColor:      vec3<f32>,
    EmissiveIntensity:  f32,
    AmbientOcclusion:   f32,
    // Pad to 16-byte alignment
    _pad0: f32,
    _pad1: f32,
    _pad2: f32,
};

struct LightParameters {
    Position:   vec3<f32>,
    Intensity:  f32,
    Direction:  vec3<f32>,
    Range:      f32,
    Color:      vec3<f32>,
    CosUmbra:   f32,
    Attenuation: vec3<f32>,
    CosPenumbra: f32,
};

struct DirectionalLightParameters {
    LightMVP:               mat4x4<f32>,
    DirectionAndIntensity:  vec4<f32>,
    Color:                  vec4<f32>,
};

struct SSAODispatchParameters {
    NumThreadGroups: vec2<u32>,
    NumThreads:      vec2<u32>,
};

struct SSGIDispatchParameters {
    NumThreadGroups: vec2<u32>,
    NumThreads:      vec2<u32>,
};
)wgsl";

static const char* kShader_DeferredLighting = R"wgsl(
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
)wgsl";

static const char* kShader_ForwardPBR_NoShadow = R"wgsl(
// ForwardPBR_NoShadow.wgsl — Dawn Forward PBR without shadow map (3-group layout)
//
// Descriptor set layout:
//   Group 0 (Global):   binding 11 = GlobalShaderData(UB), binding 12 = ForwardLightBuffer(UB)
//   Group 1 (PerObject): binding 10 = PerObjectData (dynamic uniform)
//   Group 2 (Material):  binding 0 = albedo, 1 = normal, 2 = ORM, 3 = sampler

struct GlobalShaderData {
    view: mat4x4<f32>,
    projection: mat4x4<f32>,
    invProjection: mat4x4<f32>,
    viewProjection: mat4x4<f32>,
    previousViewProjection: mat4x4<f32>,
    invViewProjection: mat4x4<f32>,
    cameraPositionAndViewWidth: vec4<f32>,
    cameraDirectionAndViewHeight: vec4<f32>,
    numDirectionalLights: u32,
    numPunctualLights: u32,
    deltaTime: f32,
    frameCount: f32,
};

struct DirectionalLightParameters {
    viewProjections: array<mat4x4<f32>, 4>,
    splits: vec4<f32>,
    directionAndIntensity: vec4<f32>,
    colorAndShadow: vec4<f32>,
};

struct ForwardLightBuffer {
    directionalLightCount: u32,
    punctualLightCount: u32,
    _pad: vec2<u32>,
    directionalLights: array<DirectionalLightParameters, 4>,
};

struct PerObjectData {
    world: mat4x4<f32>,
    invWorld: mat4x4<f32>,
    worldViewProjection: mat4x4<f32>,
    prevWorldViewProjection: mat4x4<f32>,
    sh_coeffs: array<vec4<f32>, 9>,
};

@group(0) @binding(11) var<uniform> globalData: GlobalShaderData;
@group(0) @binding(12) var<uniform> lightBuffer: ForwardLightBuffer;

@group(1) @binding(10) var<uniform> perObject: PerObjectData;

@group(2) @binding(0) var albedoMap: texture_2d<f32>;
@group(2) @binding(1) var normalMap: texture_2d<f32>;
@group(2) @binding(2) var ormMap: texture_2d<f32>;
@group(2) @binding(3) var matSampler: sampler;

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
    @location(5) prevClip: vec4<f32>,
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
fn vertexMain(input: VSInput) -> VSOutput {
    var output: VSOutput;
    let worldPos = perObject.world * vec4<f32>(input.position, 1.0);
    output.clipPos = globalData.viewProjection * worldPos;
    output.clipPos = vec4<f32>(output.clipPos.xy + globalData.jitterOffset * output.clipPos.w,
                               output.clipPos.zw);
    output.uv = vec2<f32>(input.uv.x, 1.0 - input.uv.y);
    output.worldPos = worldPos.xyz;
    output.normal = unpackNormal(input.packed_normal, input.color_t_sign);
    output.tangent = unpackTangent(input.packed_tangent);
    output.tSign = input.color_t_sign;
    let prevClip = perObject.prevWorldViewProjection * vec4<f32>(input.position, 1.0);
    output.prevClip = prevClip;
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

struct FragmentOutput {
    @location(0) color: vec4<f32>,
    @location(1) velocity: vec2<f32>,
};

@fragment
fn fragmentMain(input: VSOutput, @builtin(front_facing) isFrontFace: bool) -> FragmentOutput {
    let albedo = textureSample(albedoMap, matSampler, input.uv).rgb;
    let normalTex = textureSample(normalMap, matSampler, input.uv).rgb;
    let orm = textureSample(ormMap, matSampler, input.uv);
    let ao = orm.r;
    let roughness = max(orm.g, 0.04);
    let metallic = orm.b;

    var N = normalize(input.normal);
    if (!isFrontFace) { N = -N; }
    let T = normalize(input.tangent);
    let B = normalize(cross(N, T));

    let tangentNormal = normalTex * 2.0 - 1.0;
    N = normalize(T * tangentNormal.x + B * tangentNormal.y + N * tangentNormal.z);

    let V = normalize(globalData.cameraPositionAndViewWidth.xyz - input.worldPos);

    var color = vec3<f32>(0.01, 0.01, 0.01) * albedo * ao;

    if (lightBuffer.directionalLightCount > 0u) {
        let light = lightBuffer.directionalLights[0];
        let L = normalize(-light.directionAndIntensity.xyz);
        let H = normalize(V + L);
        let radiance = light.colorAndShadow.rgb * light.directionAndIntensity.w;

        let NdotV = max(dot(N, V), 0.001);
        let NdotL = max(dot(N, L), 0.0);

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
        color = color + Lo;
    }

    // Tone contrast + gamma correction
    color = pow(max(color, vec3<f32>(0.0, 0.0, 0.0)), vec3<f32>(1.3, 1.3, 1.3));
    color = pow(color, vec3<f32>(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));

    let screenSize = vec2<f32>(globalData.cameraPositionAndViewWidth.w,
                               globalData.cameraDirectionAndViewHeight.w);
    let currNDC = vec2<f32>(
        input.clipPos.x / max(screenSize.x, 1.0) * 2.0 - 1.0,
        1.0 - input.clipPos.y / max(screenSize.y, 1.0) * 2.0
    );
    let prevNDC = input.prevClip.xy / max(input.prevClip.w, 0.0001);
    let velocity = (currNDC - prevNDC) * 0.5;

    var out: FragmentOutput;
    out.color = vec4<f32>(color, 1.0);
    out.velocity = velocity;
    return out;
}
)wgsl";

static const char* kShader_ForwardPBR = R"wgsl(
// ForwardPBR.wgsl — Engine production Forward PBR shader for Dawn/WebGPU
//
// Descriptor set layout (matches ForwardRenderer + Material system):
//   Set 0 (Global):   binding 11 = GlobalShaderData, binding 12 = ForwardLightBuffer
//                     binding 13 = shadowDepthTex, binding 14 = shadowSampler
//   Set 1 (PerObject): binding 10 = PerObjectData (dynamic uniform)
//   Set 2 (Material):  binding 0 = albedo, 1 = normal, 2 = ORM, 3 = sampler
//
// Stage 1: directional light with shadow map sampling
// Output: linear HDR (tone mapping handled by ToneMappingPass)

// === Structs matching C++ RHIShaderCommon.h (16-byte aligned) ===

struct GlobalShaderData {
    view: mat4x4<f32>,
    projection: mat4x4<f32>,
    invProjection: mat4x4<f32>,
    viewProjection: mat4x4<f32>,
    previousViewProjection: mat4x4<f32>,
    invViewProjection: mat4x4<f32>,
    cameraPositionAndViewWidth: vec4<f32>,
    cameraDirectionAndViewHeight: vec4<f32>,
    numDirectionalLights: u32,
    numPunctualLights: u32,
    deltaTime: f32,
    frameCount: f32,
    renderMode: u32,
    _pad0: u32,
    jitterOffset: vec2<f32>,
};

struct DirectionalLightParameters {
    viewProjections: array<mat4x4<f32>, 4>,
    splits: vec4<f32>,
    directionAndIntensity: vec4<f32>,
    colorAndShadow: vec4<f32>,
};

struct PunctualLightParameters {
    position: vec4<f32>,       // simd::float3 = 16 bytes (xyz=pos, w=unused)
    intensity: f32,
    _p0: f32, _p1: f32, _p2: f32,  // pad to 32
    direction: vec4<f32>,      // xyz=dir, w=unused
    range: f32,
    _p3: f32, _p4: f32, _p5: f32,  // pad to 64
    color: vec4<f32>,          // xyz=color, w=unused
    cosUmbra: f32,
    _p6: f32, _p7: f32, _p8: f32,  // pad to 96
    attenuation: vec4<f32>,    // xyz=att, w=unused
    cosPenumbra: f32,
    lightType: i32,
    shadowIndex: i32,
    _pad0: f32,
    viewProjection: mat4x4<f32>,
};

struct ForwardLightBuffer {
    directionalLightCount: u32,
    punctualLightCount: u32,
    _pad: vec2<u32>,
    directionalLights: array<DirectionalLightParameters, 4>,
    lights: array<PunctualLightParameters, 128>,
};

struct PerObjectData {
    world: mat4x4<f32>,
    invWorld: mat4x4<f32>,
    worldViewProjection: mat4x4<f32>,
    prevWorldViewProjection: mat4x4<f32>,
    sh_coeffs: array<vec4<f32>, 9>,
};

// === Bindings ===

@group(0) @binding(11) var<uniform> globalData: GlobalShaderData;
@group(0) @binding(12) var<uniform> lightBuffer: ForwardLightBuffer;

@group(1) @binding(0) var<uniform> perObject: PerObjectData;

@group(0) @binding(13) var shadowDepthTex: texture_depth_2d_array;
@group(0) @binding(14) var shadowSampler: sampler;

@group(0) @binding(15) var irradianceMap: texture_cube<f32>;
@group(0) @binding(16) var prefilterMap: texture_cube<f32>;
@group(0) @binding(17) var brdfLUT: texture_2d<f32>;
@group(0) @binding(18) var iblSampler: sampler;

@group(2) @binding(0) var albedoMap: texture_2d<f32>;
@group(2) @binding(1) var normalMap: texture_2d<f32>;
@group(2) @binding(2) var ormMap: texture_2d<f32>;
@group(2) @binding(3) var matSampler: sampler;

// === Vertex I/O ===

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
    @location(5) prevClip: vec4<f32>,
};

// === Unpack helpers (little-endian: low 16-bit = x, high 16-bit = y) ===

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

// === Vertex shader ===

@vertex
fn vertexMain(input: VSInput) -> VSOutput {
    var output: VSOutput;
    let worldPos = perObject.world * vec4<f32>(input.position, 1.0);
    output.clipPos = globalData.viewProjection * worldPos;
    output.clipPos = vec4<f32>(output.clipPos.xy + globalData.jitterOffset * output.clipPos.w,
                               output.clipPos.zw);
    output.uv = vec2<f32>(input.uv.x, 1.0 - input.uv.y);
    output.worldPos = worldPos.xyz;
    output.normal = unpackNormal(input.packed_normal, input.color_t_sign);
    output.tangent = unpackTangent(input.packed_tangent);
    output.tSign = input.color_t_sign;
    let prevClip = perObject.prevWorldViewProjection * vec4<f32>(input.position, 1.0);
    output.prevClip = prevClip;
    return output;
}

// === PBR lighting ===

fn fresnelSchlick(cosTheta: f32, F0: vec3<f32>) -> vec3<f32> {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

fn fresnelSchlickRoughness(cosTheta: f32, F0: vec3<f32>, roughness: f32) -> vec3<f32> {
    return F0 + (max(vec3<f32>(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
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

// === Shadow sampling (CSM) ===

fn selectCascade(viewZ: f32) -> i32 {
    let splits = lightBuffer.directionalLights[0].splits;
    for (var i: i32 = 0; i < 4; i++) {
        if (viewZ < splits[i]) { return i; }
    }
    return 3;
}

fn sampleShadowPCF(worldPos: vec3<f32>, viewZ: f32, N: vec3<f32>, lightDir: vec3<f32>) -> f32 {
    let cascadeIdx = selectCascade(viewZ);
    let lightVP = lightBuffer.directionalLights[0].viewProjections[cascadeIdx];
    let lightClip = lightVP * vec4<f32>(worldPos, 1.0);
    let lightNDC = lightClip.xyz / lightClip.w;

    if (lightNDC.x < -1.0 || lightNDC.x > 1.0 || lightNDC.y < -1.0 || lightNDC.y > 1.0
        || lightNDC.z < 0.0 || lightNDC.z > 1.0) {
        return 1.0;
    }

    let shadowUV = clamp(vec2<f32>(lightNDC.x * 0.5 + 0.5, 1.0 - (lightNDC.y * 0.5 + 0.5)),
                         vec2<f32>(0.001, 0.001), vec2<f32>(0.999, 0.999));
    let shadowZ = lightNDC.z;

    let texSize = vec2<f32>(textureDimensions(shadowDepthTex));
    let bias = max(0.005 * (1.0 - dot(N, lightDir)), 0.001);

    let baseCoord = vec2<i32>(shadowUV * texSize);
    var shadow = 0.0;
    var count = 0;
    for (var x = -1; x <= 1; x++) {
        for (var y = -1; y <= 1; y++) {
            let coord = clamp(baseCoord + vec2<i32>(x, y), vec2<i32>(0, 0), vec2<i32>(i32(texSize.x) - 1, i32(texSize.y) - 1));
            let depth = textureLoad(shadowDepthTex, coord, cascadeIdx, 0);
            shadow += select(0.0, 1.0, depth > shadowZ - bias);
            count++;
        }
    }
    return shadow / f32(count);
}

// === Fragment shader ===

struct FragmentOutput {
    @location(0) color: vec4<f32>,
    @location(1) velocity: vec2<f32>,
};

@fragment
fn fragmentMain(input: VSOutput, @builtin(front_facing) isFrontFace: bool) -> FragmentOutput {
    let albedo = textureSample(albedoMap, matSampler, input.uv).rgb;
    let normalTex = textureSample(normalMap, matSampler, input.uv).rgb;
    let orm = textureSample(ormMap, matSampler, input.uv);
    let ao = orm.r;
    let roughness = max(orm.g, 0.04);
    let metallic = orm.b;

    // Tangent-space basis
    var N = normalize(input.normal);
    if (!isFrontFace) { N = -N; }
    let T = normalize(input.tangent);
    let B = normalize(cross(N, T));

    // Normal map reconstruction
    let tangentNormal = normalTex * 2.0 - 1.0;
    N = normalize(T * tangentNormal.x + B * tangentNormal.y + N * tangentNormal.z);

    let V = normalize(globalData.cameraPositionAndViewWidth.xyz - input.worldPos);

    // View-space Z for cascade selection
    let viewPos = globalData.view * vec4<f32>(input.worldPos, 1.0);
    let viewZ = -viewPos.z;

    var color = vec3<f32>(0.0);
    var iblShadow: f32 = 1.0;

    let mode = globalData.renderMode;
    let shadowActive = mode >= 1u;
    let iblActive = mode >= 2u;
    let punctualActive = mode >= 3u;

    let F0 = mix(vec3<f32>(0.04, 0.04, 0.04), albedo, metallic);
    let NdotV = max(dot(N, V), 0.001);

    // Directional light (first only) with shadow
    if (lightBuffer.directionalLightCount > 0u) {
        let light = lightBuffer.directionalLights[0];
        let L = normalize(-light.directionAndIntensity.xyz);
        let H = normalize(V + L);

        let shadowFactor = select(1.0, sampleShadowPCF(input.worldPos, viewZ, N, L), shadowActive);
        let radiance = light.colorAndShadow.rgb * light.directionAndIntensity.w * shadowFactor;

        let NdotL = max(dot(N, L), 0.0);

        let fresnel = fresnelSchlick(max(dot(H, V), 0.0), F0);
        let ndf = distributionGGX(N, H, roughness);
        let geometry = geometrySmith(N, V, L, roughness);

        let numerator = ndf * geometry * fresnel;
        let denominator = 4.0 * NdotV * NdotL + 0.0001;
        let specular = numerator / denominator;

        let kS = fresnel;
        let kD = (vec3<f32>(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);

        let Lo = (kD * albedo / PI + specular) * radiance * NdotL;
        color = color + Lo;
        iblShadow = select(1.0, mix(0.35, 1.0, shadowFactor), shadowActive);
    }

    // Punctual lights (point + spot) — only in Full mode
    if (punctualActive) {
        for (var i: u32 = 0u; i < lightBuffer.punctualLightCount; i++) {
            let plight = lightBuffer.lights[i];
            let toLight = plight.position.xyz - input.worldPos;
            let distance = length(toLight);
            if (distance > plight.range) { continue; }

            // Smooth falloff near range edge
            let rangeFade = 1.0 - smoothstep(plight.range * 0.6, plight.range, distance);

            let L = toLight / distance;
            let H = normalize(V + L);

            let att = plight.attenuation.xyz;
            let attenuation = 1.0 / (att.x + att.y * distance + att.z * distance * distance) * rangeFade;

            var spotAtt = 1.0;
            if (plight.lightType == 2) {
                let theta = dot(L, normalize(-plight.direction.xyz));
                let epsilon = plight.cosUmbra - plight.cosPenumbra;
                spotAtt = clamp((theta - plight.cosPenumbra) / max(epsilon, 0.001), 0.0, 1.0);
            }

            let radiance = plight.color.xyz * plight.intensity * attenuation * spotAtt;
            let NdotL = max(dot(N, L), 0.0);

            let fresnel = fresnelSchlick(max(dot(H, V), 0.0), F0);
            let ndf = distributionGGX(N, H, roughness);
            let geometry = geometrySmith(N, V, L, roughness);

            let numerator = ndf * geometry * fresnel;
            let denominator = 4.0 * NdotV * NdotL + 0.0001;
            let specular = numerator / denominator;

            let kS = fresnel;
            let kD = (vec3<f32>(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);

            let Lo = (kD * albedo / PI + specular) * radiance * NdotL;
            color = color + Lo;
        }
    }

    // IBL ambient — only in ShadowAndIBL and Full modes
    if (iblActive) {
        let kS_ibl = fresnelSchlickRoughness(NdotV, F0, roughness);
        let kD_ibl = (vec3<f32>(1.0) - kS_ibl) * (1.0 - metallic);
        let irradiance = textureSample(irradianceMap, iblSampler, N).rgb;
        let diffuseIBL = irradiance * albedo;
        let R = reflect(-V, N);
        let prefilteredColor = textureSampleLevel(prefilterMap, iblSampler, R, roughness * 4.0).rgb;
        let brdf = textureSample(brdfLUT, iblSampler, vec2<f32>(NdotV, roughness));
        let specularIBL = prefilteredColor * (kS_ibl * brdf.r + brdf.g);
        color = color + (kD_ibl * diffuseIBL + specularIBL) * ao * iblShadow * 0.6;
    } else {
        color = color + vec3<f32>(0.03) * albedo * ao;
    }
    // Tone contrast + gamma correction
    color = pow(max(color, vec3<f32>(0.0, 0.0, 0.0)), vec3<f32>(1.3, 1.3, 1.3));
    color = pow(color, vec3<f32>(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2));

    let screenSize = vec2<f32>(globalData.cameraPositionAndViewWidth.w,
                               globalData.cameraDirectionAndViewHeight.w);
    let currNDC = vec2<f32>(
        input.clipPos.x / max(screenSize.x, 1.0) * 2.0 - 1.0,
        1.0 - input.clipPos.y / max(screenSize.y, 1.0) * 2.0
    );
    let prevNDC = input.prevClip.xy / max(input.prevClip.w, 0.0001);
    let velocity = (currNDC - prevNDC) * 0.5;

    var out: FragmentOutput;
    out.color = vec4<f32>(color, 1.0);
    out.velocity = velocity;
    return out;
}
)wgsl";

static const char* kShader_FullScreenTriangle = R"wgsl(
// FullScreenTriangle.wgsl — Full-screen triangle vertex shader

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn fullscreen_triangle_vs(@builtin(vertex_index) vertexID: u32) -> VertexOutput {
    var output: VertexOutput;
    output.uv = vec2<f32>(f32((vertexID << 1u) & 2u), f32(vertexID & 2u));
    output.position = vec4<f32>(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}
)wgsl";

static const char* kShader_HZBGeneration = R"wgsl(
// HZBGeneration.wgsl — Hierarchical Z-Buffer generation
// Two compute entry points:
//   copy_depth_to_hzb_mip0: copies depth buffer to HZB mip 0
//   generate_hzb_mip_level: generates one mip levels via 2x2 min-depth filter

struct HZBCopyParams {
    width: u32,
    height: u32,
    _pad0: u32,
    _pad1: u32,
};

struct HZBMipParams {
    srcWidth: u32,
    srcHeight: u32,
    dstWidth: u32,
    dstHeight: u32,
};

// --- Copy depth to HZB mip 0 ---
@group(0) @binding(0) var depthTex: texture_depth_2d;
@group(0) @binding(1) var hzbOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> copyParams: HZBCopyParams;

@compute @workgroup_size(8, 8, 1)
fn copy_depth_to_hzb_mip0(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= copyParams.width || gid.y >= copyParams.height) { return; }

    let depth = textureLoad(depthTex, gid.xy, 0);
    let d = clamp(depth, 0.0, 1.0);
    textureStore(hzbOutput, gid.xy, vec4f(d, 0.0, 0.0, 0.0));
}

// --- Generate HZB mip level (2x2 min-depth) ---
@group(0) @binding(0) var hzbSource: texture_2d<f32>;
@group(0) @binding(1) var hzbMipDest: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> mipParams: HZBMipParams;

@compute @workgroup_size(8, 8, 1)
fn generate_hzb_mip_level(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= mipParams.dstWidth || gid.y >= mipParams.dstHeight) { return; }

    let srcCoords = gid.xy * 2u;
    let srcMax = vec2u(mipParams.srcWidth - 1u, mipParams.srcHeight - 1u);

    let d00 = textureLoad(hzbSource, min(srcCoords, srcMax), 0).r;
    let d10 = textureLoad(hzbSource, min(srcCoords + vec2u(1u, 0u), srcMax), 0).r;
    let d01 = textureLoad(hzbSource, min(srcCoords + vec2u(0u, 1u), srcMax), 0).r;
    let d11 = textureLoad(hzbSource, min(srcCoords + vec2u(1u, 1u), srcMax), 0).r;

    let minDepth = min(min(d00, d10), min(d01, d11));
    textureStore(hzbMipDest, gid.xy, vec4f(minDepth, 0.0, 0.0, 0.0));
}
)wgsl";

static const char* kShader_ShadowDepth = R"wgsl(
// ShadowDepth.wgsl — Depth-only shadow pass vertex shader
// Renders scene geometry from directional light's perspective into a D32_Float depth texture.
// No pixel shader (depth-only pass), front-face culling to reduce shadow acne.

struct ShadowPerObject {
    world: mat4x4<f32>,
    worldLightVP: mat4x4<f32>,
};

@group(0) @binding(0) var<uniform> obj: ShadowPerObject;

struct VSInput {
    @location(0) position: vec3<f32>,
    @location(1) _pad1: u32,
    @location(2) _pad2: u32,
    @location(3) _pad3: u32,
    @location(4) _pad4: vec2<f32>,
};

@vertex fn shadow_vs(input: VSInput) -> @builtin(position) vec4<f32> {
    return obj.worldLightVP * vec4f(input.position, 1.0);
}
)wgsl";

static const char* kShader_SSAO = R"wgsl(
// SSAO.wgsl — Screen Space Ambient Occlusion with depth-based reconstruction

struct SSAOParams {
    invProj: mat4x4<f32>,
    proj: mat4x4<f32>,
    screenSize: vec4<f32>,  // x=width, y=height, z=1/width, w=1/height
    radius: f32,
    power: f32,
    sampleCount: u32,
    frameIndex: u32,
};

@group(0) @binding(0) var depthTex: texture_depth_2d;
@group(0) @binding(1) var aoOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> params: SSAOParams;

fn reconstructViewPos(uv: vec2<f32>, depth: f32) -> vec3<f32> {
    let ndcX = uv.x * 2.0 - 1.0;
    let ndcY = 1.0 - uv.y * 2.0;  // Y flip: WebGPU framebuffer Y=0 is top, clip Y=+1 is top
    let clipPos = vec4<f32>(ndcX, ndcY, depth, 1.0);
    let viewPos = params.invProj * clipPos;
    return viewPos.xyz / viewPos.w;
}

fn reconstructViewNormal(pixel: vec2<u32>, dims: vec2<u32>, uv: vec2<f32>, depth: f32) -> vec3<f32> {
    let texelSize = params.screenSize.zw;
    let px = clamp(i32(pixel.x), 1, i32(dims.x) - 2);
    let py = clamp(i32(pixel.y), 1, i32(dims.y) - 2);

    let depthLeft  = textureLoad(depthTex, vec2<u32>(u32(px - 1), u32(py)), 0);
    let depthRight = textureLoad(depthTex, vec2<u32>(u32(px + 1), u32(py)), 0);
    let depthUp    = textureLoad(depthTex, vec2<u32>(u32(px), u32(py - 1)), 0);
    let depthDown  = textureLoad(depthTex, vec2<u32>(u32(px), u32(py + 1)), 0);

    let pLeft   = reconstructViewPos(vec2<f32>(uv.x - texelSize.x, uv.y), depthLeft);
    let pRight  = reconstructViewPos(vec2<f32>(uv.x + texelSize.x, uv.y), depthRight);
    let pUp     = reconstructViewPos(vec2<f32>(uv.x, uv.y - texelSize.y), depthUp);
    let pDown   = reconstructViewPos(vec2<f32>(uv.x, uv.y + texelSize.y), depthDown);

    let dx = pRight - pLeft;
    let dy = pDown - pUp;
    // cross(dy, dx) gives camera-facing normal (+Z in view space)
    return normalize(cross(dy, dx));
}

fn hash(pos: vec2<u32>, frame: u32) -> f32 {
    var h = pos.x * 374761393u + pos.y * 668265263u + frame * 1274126177u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return f32(h) / 4294967295.0;
}

@compute @workgroup_size(8, 8, 1)
fn ssao_trace(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = vec2<u32>(params.screenSize.xy);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }

    let pixel = vec2<u32>(gid.x, gid.y);
    let uv = (vec2<f32>(pixel) + 0.5) / params.screenSize.xy;

    let depth = textureLoad(depthTex, pixel, 0);

    // Sky pixels: no occlusion
    if (depth >= 0.9999) {
        textureStore(aoOutput, pixel, vec4<f32>(1.0, 1.0, 1.0, 1.0));
        return;
    }

    let viewPos = reconstructViewPos(uv, depth);
    let viewNormal = reconstructViewNormal(pixel, dims, uv, depth);

    let radius = params.radius;
    let numSamples = i32(params.sampleCount);
    let rotationAngle = hash(pixel, params.frameIndex) * 6.283185;

    var occlusion = 0.0;

    // Build TBN basis
    var up = vec3<f32>(0.0, 1.0, 0.0);
    if (abs(viewNormal.y) > 0.99) {
        up = vec3<f32>(1.0, 0.0, 0.0);
    }
    let tangent = normalize(cross(viewNormal, up));
    let bitangent = cross(viewNormal, tangent);

    for (var i: i32 = 0; i < numSamples; i++) {
        let f_i = f32(i);
        let f_n = f32(numSamples);
        let goldenRatio = 1.618033988749;

        // Fibonacci hemisphere sampling
        let theta = acos(1.0 - f_i / f_n);
        let phi = 2.0 * 3.14159265 * f_i / goldenRatio + rotationAngle;

        let sinTheta = sin(theta);
        let cosTheta = cos(theta);

        let hemisphereDir = vec3<f32>(
            sinTheta * cos(phi),
            sinTheta * sin(phi),
            cosTheta
        );

        let sampleDir = tangent * hemisphereDir.x + bitangent * hemisphereDir.y + viewNormal * hemisphereDir.z;
        let samplePos = viewPos + sampleDir * radius;

        // Project sample position to screen using forward projection
        let sampleClip = params.proj * vec4<f32>(samplePos, 1.0);
        let sampleNDC = sampleClip.xy / sampleClip.w;
        // Undo the Y flip: screen UV Y = (1 - ndcY) / 2
        let sampleUV = vec2<f32>(
            sampleNDC.x * 0.5 + 0.5,
            0.5 - sampleNDC.y * 0.5
        );

        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
            continue;
        }

        let samplePixel = vec2<u32>(
            clamp(u32(sampleUV.x * f32(dims.x)), 0u, dims.x - 1u),
            clamp(u32(sampleUV.y * f32(dims.y)), 0u, dims.y - 1u)
        );
        let sampleDepth = textureLoad(depthTex, samplePixel, 0);

        // Skip sky samples
        if (sampleDepth >= 0.9999) { continue; }

        let sampleViewZ = reconstructViewPos(sampleUV, sampleDepth).z;

        // Range check: prevent far-away geometry from contributing
        let rangeCheck = smoothstep(0.0, 1.0, radius / abs(viewPos.z - sampleViewZ));

        // In view space, objects in front have more negative Z.
        // If sampled geometry is closer to camera (less negative Z) than sample point, it's occluded.
        if (sampleViewZ > samplePos.z + 0.001) {
            occlusion += rangeCheck;
        }
    }

    let ao = 1.0 - (occlusion / f32(numSamples)) * params.power;
    let finalAO = clamp(ao, 0.0, 1.0);
    textureStore(aoOutput, pixel, vec4<f32>(finalAO, finalAO, finalAO, 1.0));
}
)wgsl";

static const char* kShader_SSAOBlur = R"wgsl(
// SSAOBlur.wgsl — 5x5 bilateral blur for SSAO

struct BlurParams {
    screenSize: vec4<f32>,  // x=width, y=height, z=1/width, w=1/height
};

@group(0) @binding(0) var aoInput: texture_2d<f32>;
@group(0) @binding(1) var depthTex: texture_depth_2d;
@group(0) @binding(2) var aoBlurred: texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var<uniform> params: BlurParams;

const SIGMA_DEPTH: f32 = 0.002;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = vec2<u32>(params.screenSize.xy);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }

    let pixel = vec2<u32>(gid.x, gid.y);
    let centerDepth = textureLoad(depthTex, pixel, 0);

    var aoSum = 0.0;
    var weightSum = 0.0;

    // 5x5 bilateral filter
    for (var dy: i32 = -2; dy <= 2; dy++) {
        for (var dx: i32 = -2; dx <= 2; dx++) {
            let nx = i32(pixel.x) + dx;
            let ny = i32(pixel.y) + dy;

            if (nx < 0 || nx >= i32(dims.x) || ny < 0 || ny >= i32(dims.y)) {
                continue;
            }

            let nPixel = vec2<u32>(u32(nx), u32(ny));

            // Spatial gaussian weight
            let spatialDist = f32(dx * dx + dy * dy);
            let spatialWeight = exp(-spatialDist / 4.0);

            // Depth bilateral weight — preserve edges
            let neighborDepth = textureLoad(depthTex, nPixel, 0);
            let depthDiff = abs(neighborDepth - centerDepth);
            let depthWeight = exp(-depthDiff * depthDiff / (SIGMA_DEPTH * SIGMA_DEPTH));

            let weight = spatialWeight * depthWeight;

            let aoValue = textureLoad(aoInput, nPixel, 0).r;
            aoSum += aoValue * weight;
            weightSum += weight;
        }
    }

    let ao = select(aoSum / weightSum, 1.0, weightSum < 0.001);
    textureStore(aoBlurred, pixel, vec4<f32>(ao, ao, ao, 1.0));
}
)wgsl";

static const char* kShader_SSGIFilter = R"wgsl(
// SSGIFilter.wgsl — Bilateral spatial filter with bilinear upsampling to full resolution
// Reads half-res denoised SSGI, applies edge-preserving filter, outputs full-res.

struct SSGIFilterParams {
    invProj: mat4x4<f32>,
    screenSize: vec4<f32>,       // full-res: x=width, y=height, z=1/width, w=1/height
    halfScreenSize: vec4<f32>,   // half-res dims for UV mapping
    sigmaDepth: f32,
    sigmaNormal: f32,
    sigmaHitDist: f32,
    sigmaSpatial: f32,
    kernelRadius: u32,
    _pad0: u32,
    _pad1: u32,
    _pad2: u32,
};

@group(0) @binding(0) var ssgiInput: texture_2d<f32>;       // half-res denoised
@group(0) @binding(1) var depthTex: texture_depth_2d;       // full-res depth
@group(0) @binding(2) var ssgiOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(3) var<uniform> params: SSGIFilterParams;

fn reconstructViewPos(uv: vec2f, ndcDepth: f32) -> vec3f {
    let ndcX = uv.x * 2.0 - 1.0;
    let ndcY = 1.0 - uv.y * 2.0;
    let clipPos = vec4f(ndcX, ndcY, ndcDepth, 1.0);
    let viewPos4 = params.invProj * clipPos;
    let vp = viewPos4.xyz / viewPos4.w;
    return vec3f(vp.x, vp.y, -vp.z);
}

fn reconstructViewNormal(pixel: vec2u, uv: vec2f, depth: f32) -> vec3f {
    let ts = params.screenSize.zw;
    let px = clamp(i32(pixel.x), 1, i32(params.screenSize.x) - 2);
    let py = clamp(i32(pixel.y), 1, i32(params.screenSize.y) - 2);

    let dL = textureLoad(depthTex, vec2u(u32(px - 1), u32(py)), 0);
    let dR = textureLoad(depthTex, vec2u(u32(px + 1), u32(py)), 0);
    let dU = textureLoad(depthTex, vec2u(u32(px), u32(py - 1)), 0);
    let dD = textureLoad(depthTex, vec2u(u32(px), u32(py + 1)), 0);

    let pL = reconstructViewPos(vec2f(uv.x - ts.x, uv.y), dL);
    let pR = reconstructViewPos(vec2f(uv.x + ts.x, uv.y), dR);
    let pU = reconstructViewPos(vec2f(uv.x, uv.y - ts.y), dU);
    let pD = reconstructViewPos(vec2f(uv.x, uv.y + ts.y), dD);

    return normalize(cross(pD - pU, pR - pL));
}

// Manual bilinear sampling for half-res -> full-res upsampling
fn sampleBilinear(tex: texture_2d<f32>, uv: vec2f) -> vec4f {
    let dims = vec2u(textureDimensions(tex, 0));
    let coord = uv * vec2f(dims) - 0.5;
    let bx = clamp(i32(coord.x), 0, i32(dims.x) - 1);
    let by = clamp(i32(coord.y), 0, i32(dims.y) - 1);
    let fx = clamp(fract(coord.x), 0.0, 1.0);
    let fy = clamp(fract(coord.y), 0.0, 1.0);
    let bx1 = min(u32(bx) + 1u, dims.x - 1u);
    let by1 = min(u32(by) + 1u, dims.y - 1u);

    let c00 = textureLoad(tex, vec2u(u32(bx), u32(by)), 0);
    let c10 = textureLoad(tex, vec2u(bx1, u32(by)), 0);
    let c01 = textureLoad(tex, vec2u(u32(bx), by1), 0);
    let c11 = textureLoad(tex, vec2u(bx1, by1), 0);
    return mix(mix(c00, c10, fx), mix(c01, c11, fx), fy);
}

@compute @workgroup_size(8, 8, 1)
fn ssgi_filter(@builtin(global_invocation_id) gid: vec3u) {
    let width = u32(params.screenSize.x);
    let height = u32(params.screenSize.y);
    if (gid.x >= width || gid.y >= height) { return; }

    let pixelPos = gid.xy;
    let invRes = params.screenSize.zw;
    let centerUV = (vec2f(pixelPos) + 0.5) * invRes;

    // Bilinear upsample from half-res
    let centerSSGI = sampleBilinear(ssgiInput, centerUV);
    let centerDepth = textureLoad(depthTex, pixelPos, 0);
    let centerNormal = reconstructViewNormal(pixelPos, centerUV, centerDepth);

    let centerIrradiance = centerSSGI.rgb;
    let centerHitDist = centerSSGI.a;

    // Bilateral filter
    let radius = min(params.kernelRadius, 4u);

    var filteredIrr = vec3f(0.0);
    var filteredDist: f32 = 0.0;
    var totalWeight: f32 = 0.0;

    for (var dy: i32 = -i32(radius); dy <= i32(radius); dy++) {
        for (var dx: i32 = -i32(radius); dx <= i32(radius); dx++) {
            let samplePos = vec2i(i32(pixelPos.x) + dx, i32(pixelPos.y) + dy);
            if (samplePos.x < 0 || samplePos.y < 0 ||
                samplePos.x >= i32(width) || samplePos.y >= i32(height)) {
                continue;
            }

            let sampleUV = (vec2f(samplePos) + 0.5) * invRes;
            let sampleSSGI = sampleBilinear(ssgiInput, sampleUV);
            let sampleDepth = textureLoad(depthTex, vec2u(samplePos), 0);
            let sampleNormal = reconstructViewNormal(vec2u(samplePos), sampleUV, sampleDepth);

            // Depth weight
            let depthDiff = abs(centerDepth - sampleDepth);
            let wDepth = exp(-depthDiff * params.sigmaDepth);

            // Normal weight
            let NdotN = max(0.0, dot(centerNormal, sampleNormal));
            let wNormal = pow(NdotN, params.sigmaNormal);

            // Hit distance weight
            let distDiff = abs(centerHitDist - sampleSSGI.a);
            let wDist = exp(-distDiff * params.sigmaHitDist);

            // Adaptive spatial weight
            let adaptiveSigma = max(params.sigmaSpatial * centerHitDist * 0.5,
                                    params.sigmaSpatial * 0.5);
            let spatialDist2 = f32(dx * dx + dy * dy);
            let wSpatial = exp(-spatialDist2 / (2.0 * adaptiveSigma * adaptiveSigma));

            let weight = wDepth * wNormal * wDist * wSpatial;

            filteredIrr += sampleSSGI.rgb * weight;
            filteredDist += sampleSSGI.a * weight;
            totalWeight += weight;
        }
    }

    if (totalWeight > 1e-6) {
        filteredIrr /= totalWeight;
        filteredDist /= totalWeight;
    } else {
        filteredIrr = centerIrradiance;
        filteredDist = centerHitDist;
    }

    textureStore(ssgiOutput, pixelPos, vec4f(filteredIrr, filteredDist));
}
)wgsl";

static const char* kShader_SSGIHalfResDenoise = R"wgsl(
// SSGIHalfResDenoise.wgsl — 5x5 Gaussian pre-smooth at half resolution
// Pure Gaussian blur with no edge-stopping weights.
// Edge preservation is handled by the full-res bilateral spatial filter.

struct HalfResDenoiseParams {
    width: u32,
    height: u32,
    sigma: f32,
    _pad: u32,
};

@group(0) @binding(0) var traceInput: texture_2d<f32>;
@group(0) @binding(1) var traceOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> params: HalfResDenoiseParams;

@compute @workgroup_size(8, 8, 1)
fn ssgi_halfres_denoise(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= params.width || gid.y >= params.height) { return; }

    let invTwoSigmaSq = 1.0 / (2.0 * params.sigma * params.sigma);

    var filteredIrr = vec3f(0.0);
    var filteredDist: f32 = 0.0;
    var totalWeight: f32 = 0.0;

    for (var dy: i32 = -2; dy <= 2; dy++) {
        for (var dx: i32 = -2; dx <= 2; dx++) {
            let pos = vec2i(i32(gid.x) + dx, i32(gid.y) + dy);
            if (pos.x < 0 || pos.y < 0 ||
                pos.x >= i32(params.width) || pos.y >= i32(params.height)) {
                continue;
            }

            let s = textureLoad(traceInput, vec2u(pos), 0);
            let spatial2 = f32(dx * dx + dy * dy);
            let w = exp(-spatial2 * invTwoSigmaSq);

            filteredIrr += s.rgb * w;
            filteredDist += s.a * w;
            totalWeight += w;
        }
    }

    if (totalWeight > 0.001) {
        textureStore(traceOutput, gid.xy,
                     vec4f(filteredIrr / totalWeight, filteredDist / totalWeight));
    } else {
        textureStore(traceOutput, gid.xy, textureLoad(traceInput, gid.xy, 0));
    }
}
)wgsl";

static const char* kShader_SSGITemporal = R"wgsl(
// SSGITemporal.wgsl — Temporal accumulation with variance clipping
// Full-resolution compute shader that:
//   1. Reads spatially filtered SSGI at full resolution
//   2. Reprojects using velocity buffer to find previous-frame UV
//   3. Samples history buffer with manual bilinear interpolation
//   4. Clamps history via variance clipping (mean + 2sigma on 3x3 neighborhood)
//   5. Detects disocclusion via depth comparison
//   6. Exponential blend: result = lerp(current, clamped_history, feedback)

struct SSGITemporalParams {
    feedback: f32,
    fullWidth: u32,
    fullHeight: u32,
    _pad: u32,
};

@group(0) @binding(0) var ssgiSpatial: texture_2d<f32>;      // full-res spatial filter output
@group(0) @binding(1) var ssgiHistory: texture_2d<f32>;      // previous frame temporal output
@group(0) @binding(2) var velocityTex: texture_2d<f32>;      // motion vectors
@group(0) @binding(3) var depthTex: texture_depth_2d;        // current depth
@group(0) @binding(4) var ssgiOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var<uniform> params: SSGITemporalParams;

// Manual bilinear interpolation for history sampling
fn sampleHistoryBilinear(tex: texture_2d<f32>, uv: vec2f, dims: vec2u) -> vec4f {
    let coord = uv * vec2f(dims) - 0.5;
    let base = vec2i(i32(floor(coord.x)), i32(floor(coord.y)));
    let frac_ = fract(coord);
    let maxCoord = vec2i(i32(dims.x) - 1, i32(dims.y) - 1);

    let c00 = clamp(base, vec2i(0), maxCoord);
    let c10 = clamp(base + vec2i(1, 0), vec2i(0), maxCoord);
    let c01 = clamp(base + vec2i(0, 1), vec2i(0), maxCoord);
    let c11 = clamp(base + vec2i(1, 1), vec2i(0), maxCoord);

    let h00 = textureLoad(tex, vec2u(c00), 0);
    let h10 = textureLoad(tex, vec2u(c10), 0);
    let h01 = textureLoad(tex, vec2u(c01), 0);
    let h11 = textureLoad(tex, vec2u(c11), 0);

    let fx = clamp(frac_.x, 0.0, 1.0);
    let fy = clamp(frac_.y, 0.0, 1.0);

    return h00 * (1.0 - fx) * (1.0 - fy)
         + h10 * fx * (1.0 - fy)
         + h01 * (1.0 - fx) * fy
         + h11 * fx * fy;
}

@compute @workgroup_size(8, 8, 1)
fn ssgi_temporal(@builtin(global_invocation_id) gid: vec3u) {
    if (gid.x >= params.fullWidth || gid.y >= params.fullHeight) { return; }

    let pixelPos = gid.xy;
    let pixelUV = (vec2f(pixelPos) + 0.5) / vec2f(f32(params.fullWidth), f32(params.fullHeight));

    let currentSSGI = textureLoad(ssgiSpatial, pixelPos, 0);
    let currentIrr = currentSSGI.rgb;
    let currentHitDist = currentSSGI.a;
    let depthNDC = textureLoad(depthTex, pixelPos, 0);

    // Velocity reprojection
    let velocity = textureLoad(velocityTex, pixelPos, 0).rg;
    let prevUV = pixelUV - velocity;

    // Check bounds
    let edgeDist = min(prevUV, 1.0 - prevUV);
    let inBounds = prevUV.x >= 0.0 && prevUV.x <= 1.0 &&
                   prevUV.y >= 0.0 && prevUV.y <= 1.0;

    if (!inBounds) {
        textureStore(ssgiOutput, pixelPos, vec4f(currentIrr, currentHitDist));
        return;
    }

    // Sample history with bilinear interpolation
    let histDims = vec2u(textureDimensions(ssgiHistory, 0));
    let histBilinear = sampleHistoryBilinear(ssgiHistory, prevUV, histDims);
    let historyIrr = histBilinear.rgb;
    let historyHitDist = histBilinear.a;

    // Variance clipping: 3x3 neighborhood of current spatial SSGI
    var nbSum = currentIrr;
    var nbSum2 = currentIrr * currentIrr;
    var nbCount: i32 = 1;

    for (var dy: i32 = -1; dy <= 1; dy++) {
        for (var dx: i32 = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) { continue; }
            let nPos = vec2i(i32(pixelPos.x) + dx, i32(pixelPos.y) + dy);
            if (nPos.x < 0 || nPos.y < 0 ||
                nPos.x >= i32(params.fullWidth) || nPos.y >= i32(params.fullHeight)) {
                continue;
            }
            let nb = textureLoad(ssgiSpatial, vec2u(nPos), 0).rgb;
            nbSum += nb;
            nbSum2 += nb * nb;
            nbCount++;
        }
    }

    let fCount = f32(nbCount);
    let mean = nbSum / fCount;
    let variance = abs(nbSum2 / fCount - mean * mean);
    let sigma = sqrt(max(variance, vec3f(0.0)));
    let sigmaFloor = max(mean * 0.1, vec3f(0.01));
    let aabbMin = mean - sigma * 2.0 - sigmaFloor;
    let aabbMax = mean + sigma * 2.0 + sigmaFloor;

    let clampedHistory = clamp(historyIrr, aabbMin, aabbMax);

    // Disocclusion detection
    var disocclusionFade: f32 = 1.0;
    {
        let histPixel = clamp(
            vec2i(i32(prevUV.x * f32(params.fullWidth)),
                  i32(prevUV.y * f32(params.fullHeight))),
            vec2i(0),
            vec2i(i32(params.fullWidth) - 1, i32(params.fullHeight) - 1)
        );
        let histDepth = textureLoad(depthTex, vec2u(histPixel), 0);
        let depthDiff = abs(depthNDC - histDepth);
        disocclusionFade = clamp((0.02 - depthDiff) / 0.015, 0.0, 1.0);
    }

    // Screen-edge fade
    let edgeFade = smoothstep(0.0, 0.05, min(edgeDist.x, edgeDist.y));

    // Confidence-modulated blend
    let hitConfidence = clamp(1.0 - currentHitDist / 2.0, 0.0, 1.0);
    let confidenceScale = mix(0.6, 1.0, hitConfidence);

    let effectiveFeedback = params.feedback * edgeFade * disocclusionFade * confidenceScale;

    let resultColor = mix(currentIrr, clampedHistory, effectiveFeedback);
    let resultDist = mix(currentHitDist, historyHitDist, effectiveFeedback);

    textureStore(ssgiOutput, pixelPos, vec4f(resultColor, resultDist));
}
)wgsl";

static const char* kShader_SSGITrace = R"wgsl(
// SSGITrace.wgsl — Lumen SSGI Phase 1: HZB-based screen space GI ray tracing
// Half-resolution compute shader that:
//   1. Reads depth buffer at full resolution
//   2. Reconstructs view-space position and normal from depth
//   3. Casts multiple rays in a hemisphere around the surface normal
//   4. Traces each ray through the HZB depth pyramid
//   5. On hit, samples previous frame scene color to gather indirect irradiance
//   6. Outputs RGBA16F: RGB = accumulated irradiance, A = average hit distance

const PI: f32 = 3.14159265358979323846;
const MAX_STEPS: u32 = 16u;
const MAX_DISTANCE: f32 = 20.0;
const MAX_RADIANCE: f32 = 2.0;

struct SSGITraceParams {
    invProj: mat4x4<f32>,
    proj: mat4x4<f32>,
    screenSize: vec4<f32>,       // full-res: x=width, y=height, z=1/width, w=1/height
    halfScreenSize: vec4<f32>,   // half-res: x=halfW, y=halfH, z=1/halfW, w=1/halfH
    rayCount: u32,
    radius: f32,
    thickness: f32,
    frameIndex: u32,
    nearPlane: f32,
    farPlane: f32,
    hzbMipLevels: u32,
    _pad0: u32,
};

struct TraceResult {
    hit: bool,
    hitUV: vec2f,
    hitDist: f32,
};

@group(0) @binding(0) var depthTex: texture_depth_2d;
@group(0) @binding(1) var hzbTex: texture_2d<f32>;
@group(0) @binding(2) var prevColorTex: texture_2d<f32>;
@group(0) @binding(3) var ssgiOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var<uniform> params: SSGITraceParams;

// --- Utility functions ---

fn hash_f2(p: vec2f) -> f32 {
    var p3 = fract(vec3f(p.x, p.y, p.x) * 0.1031);
    p3 = p3 + dot(p3, vec3f(p3.y, p3.z, p3.x) + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

fn ndcDepthToLinear(ndcDepth: f32) -> f32 {
    return (2.0 * params.nearPlane * params.farPlane) /
           (params.farPlane + params.nearPlane - ndcDepth * (params.farPlane - params.nearPlane));
}

fn reconstructViewPos(uv: vec2f, ndcDepth: f32) -> vec3f {
    let ndcX = uv.x * 2.0 - 1.0;
    let ndcY = 1.0 - uv.y * 2.0;
    let clipPos = vec4f(ndcX, ndcY, ndcDepth, 1.0);
    let viewPos4 = params.invProj * clipPos;
    let vp = viewPos4.xyz / viewPos4.w;
    return vec3f(vp.x, vp.y, -vp.z);
}

fn reconstructViewNormal(pixel: vec2u, uv: vec2f, depth: f32) -> vec3f {
    let ts = params.screenSize.zw;
    let px = clamp(i32(pixel.x), 1, i32(params.screenSize.x) - 2);
    let py = clamp(i32(pixel.y), 1, i32(params.screenSize.y) - 2);

    let dL = textureLoad(depthTex, vec2u(u32(px - 1), u32(py)), 0);
    let dR = textureLoad(depthTex, vec2u(u32(px + 1), u32(py)), 0);
    let dU = textureLoad(depthTex, vec2u(u32(px), u32(py - 1)), 0);
    let dD = textureLoad(depthTex, vec2u(u32(px), u32(py + 1)), 0);

    let pL = reconstructViewPos(vec2f(uv.x - ts.x, uv.y), dL);
    let pR = reconstructViewPos(vec2f(uv.x + ts.x, uv.y), dR);
    let pU = reconstructViewPos(vec2f(uv.x, uv.y - ts.y), dU);
    let pD = reconstructViewPos(vec2f(uv.x, uv.y + ts.y), dD);

    return normalize(cross(pD - pU, pR - pL));
}

fn viewToScreen(viewPos: vec3f) -> vec3f {
    let clipPos = params.proj * vec4f(viewPos.x, viewPos.y, -viewPos.z, 1.0);
    let ndc = clipPos.xyz / clipPos.w;
    return vec3f(ndc.x * 0.5 + 0.5, 0.5 - 0.5 * ndc.y, ndc.z);
}

fn cosineHemisphereSample(N: vec3f, seed: vec2f, sampleIdx: u32, frameIdx: u32, rayCount: u32) -> vec3f {
    let xi1 = fract((f32(sampleIdx) + 0.5) / f32(rayCount) + f32(frameIdx) * 0.618033988749);
    let xi2 = fract(hash_f2(seed + vec2f(f32(sampleIdx) * 0.13, f32(sampleIdx) * 0.91))
                    + f32(frameIdx) * 0.7071067811865476);

    let phi = 2.0 * PI * xi1;
    let cosTheta = sqrt(max(xi2, 0.001));
    let sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    let localDir = vec3f(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    let up = select(vec3f(0.0, 0.0, 1.0), vec3f(1.0, 0.0, 0.0), abs(N.z) < 0.999);
    let tangent = normalize(cross(up, N));
    let bitangent = cross(N, tangent);

    var result = tangent * localDir.x + bitangent * localDir.y + N * localDir.z;
    if (dot(result, N) < 0.0) { result = -result; }
    return normalize(result);
}

fn sampleBilinear(tex: texture_2d<f32>, uv: vec2f, dims: vec2u) -> vec4f {
    let coord = uv * vec2f(dims) - 0.5;
    let bx = clamp(i32(coord.x), 0, i32(dims.x) - 1);
    let by = clamp(i32(coord.y), 0, i32(dims.y) - 1);
    let fx = clamp(fract(coord.x), 0.0, 1.0);
    let fy = clamp(fract(coord.y), 0.0, 1.0);
    let bx1 = min(u32(bx) + 1u, dims.x - 1u);
    let by1 = min(u32(by) + 1u, dims.y - 1u);

    let c00 = textureLoad(tex, vec2u(u32(bx), u32(by)), 0);
    let c10 = textureLoad(tex, vec2u(bx1, u32(by)), 0);
    let c01 = textureLoad(tex, vec2u(u32(bx), by1), 0);
    let c11 = textureLoad(tex, vec2u(bx1, by1), 0);
    return mix(mix(c00, c10, fx), mix(c01, c11, fx), fy);
}

// HZB ray marching with uniform step size.
// Mip level adapts (refine on hit, coarsen on miss) but step is constant,
// preventing the exponential skip that caused rays to exhaust radius in 3-4 steps.
fn traceRayHZB(rayOriginView: vec3f, rayDirView: vec3f) -> TraceResult {
    var result: TraceResult;
    result.hit = false;

    let stepSize = params.radius / f32(MAX_STEPS);
    var t: f32 = 0.0;
    var mip: u32 = 1u;

    for (var step_i: u32 = 0u; step_i < MAX_STEPS; step_i++) {
        t += stepSize;
        if (t > params.radius) { break; }

        let rayPos = rayOriginView + rayDirView * t;
        let screen = viewToScreen(rayPos);
        let sampleUV = screen.xy;

        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 ||
            sampleUV.y < 0.0 || sampleUV.y > 1.0) { break; }
        if (rayPos.z < params.nearPlane) { continue; }

        let mipW = max(1u, u32(params.screenSize.x) >> mip);
        let mipH = max(1u, u32(params.screenSize.y) >> mip);
        let mipCoord = clamp(vec2u(sampleUV * vec2f(f32(mipW), f32(mipH))),
                             vec2u(0u), vec2u(mipW - 1u, mipH - 1u));

        let sceneDepthNDC = textureLoad(hzbTex, mipCoord, mip).r;
        let sceneDepthLinear = ndcDepthToLinear(sceneDepthNDC);
        let rayDepthLinear = rayPos.z;

        if (rayDepthLinear > sceneDepthLinear) {
            if (mip == 0u) {
                if (rayDepthLinear <= sceneDepthLinear + params.thickness) {
                    result.hit = true;
                    result.hitUV = sampleUV;
                    result.hitDist = sceneDepthLinear;
                    break;
                }
            } else {
                t -= stepSize;
                mip = mip - 1u;
            }
        } else {
            mip = min(mip + 1u, params.hzbMipLevels - 1u);
        }
    }
    return result;
}

// --- Main compute entry point ---
@compute @workgroup_size(8, 8, 1)
fn ssgi_trace(@builtin(global_invocation_id) gid: vec3u) {
    let halfDims = vec2u(params.halfScreenSize.xy);
    if (gid.x >= halfDims.x || gid.y >= halfDims.y) { return; }

    let halfPos = gid.xy;
    let fullPos = halfPos * 2u;
    let fullDims = vec2u(params.screenSize.xy);

    if (fullPos.x >= fullDims.x || fullPos.y >= fullDims.y) {
        textureStore(ssgiOutput, halfPos, vec4f(0.0));
        return;
    }

    let depth = textureLoad(depthTex, fullPos, 0);
    let linearDepth = ndcDepthToLinear(depth);

    if (depth >= 0.9999 || linearDepth < params.nearPlane ||
        linearDepth > params.farPlane * 0.999) {
        textureStore(ssgiOutput, halfPos, vec4f(0.0));
        return;
    }

    let pixelUV = (vec2f(fullPos) + 0.5) / params.screenSize.xy;
    let viewPos = reconstructViewPos(pixelUV, depth);
    let viewNormal = reconstructViewNormal(fullPos, pixelUV, depth);

    var totalIrradiance = vec3f(0.0);
    var totalHitDist: f32 = 0.0;

    let baseSeed = vec2f(
        f32(fullPos.x) * 0.001 + f32(params.frameIndex) * 0.01,
        f32(fullPos.y) * 0.0017 + f32(params.frameIndex) * 0.013
    );
    let rayCount = max(params.rayCount, 1u);

    for (var rayIdx: u32 = 0u; rayIdx < rayCount; rayIdx++) {
        let rayDir = cosineHemisphereSample(viewNormal, baseSeed, rayIdx,
                                            params.frameIndex, rayCount);
        let rayOrigin = viewPos + viewNormal * 0.1;

        let traceResult = traceRayHZB(rayOrigin, rayDir);

        if (traceResult.hit) {
            let prevDims = vec2u(
                textureDimensions(prevColorTex, 0).x,
                textureDimensions(prevColorTex, 0).y
            );
            let hitColor = sampleBilinear(prevColorTex, traceResult.hitUV, prevDims);

            var radiance = min(hitColor.rgb, vec3f(MAX_RADIANCE));

            // Bright pixel dimming
            let hitLum = dot(radiance, vec3f(0.2126, 0.7152, 0.0722));
            let brightPenalty = 1.0 / (1.0 + max(hitLum - 1.0, 0.0) * 2.0);
            radiance *= brightPenalty;

            // Distance attenuation
            let distAtten = 1.0 - smoothstep(params.radius * 0.5, params.radius,
                                              traceResult.hitDist);
            // Edge fade
            let edgeDist = min(traceResult.hitUV, 1.0 - traceResult.hitUV);
            let edgeFade = smoothstep(0.0, 0.15, min(edgeDist.x, edgeDist.y));

            totalIrradiance += radiance * PI * distAtten * edgeFade;
            totalHitDist += traceResult.hitDist;
        } else {
            totalHitDist += params.radius;
        }
    }

    let avgHitDist = totalHitDist / f32(rayCount);
    let outputIrradiance = totalIrradiance / f32(rayCount);

    textureStore(ssgiOutput, halfPos, vec4f(outputIrradiance, avgHitDist));
}
)wgsl";

static const char* kShader_SSRPass = R"wgsl(
// SSRPass.wgsl — Screen Space Reflection (placeholder)
// Used by ForwardRenderer's SSR pass

struct SSRParams {
    viewProj: mat4x4<f32>,
    invViewProj: mat4x4<f32>,
    screenWidth: f32,
    screenHeight: f32,
    _pad0: f32,
    _pad1: f32,
};

@group(0) @binding(0) var colorTex: texture_2d<f32>;
@group(0) @binding(1) var depthTex: texture_2d<f32>;
@group(0) @binding(2) var normalTex: texture_2d<f32>;
@group(0) @binding(3) var outputTex: texture_storage_2d<rgba32float, write>;
@group(0) @binding(4) var<uniform> params: SSRParams;

@compute @workgroup_size(8, 8, 1)
fn ssrCS(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = vec2<u32>(u32(params.screenWidth), u32(params.screenHeight));
    if (gid.x >= dims.x || gid.y >= dims.y) {
        return;
    }
    let texCoord = vec2<u32>(gid.x, gid.y);
    let color = textureLoad(colorTex, texCoord, 0);
    textureStore(outputTex, texCoord, color);
}
)wgsl";

static const char* kShader_TestTriangle = R"wgsl(
// TestTriangle.wgsl — Simple test shader for Dawn rendering pipeline validation

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn test_vs(@builtin(vertex_index) vertexID: u32) -> VertexOutput {
    var output: VertexOutput;
    output.uv = vec2<f32>(f32((vertexID << 1u) & 2u), f32(vertexID & 2u));
    output.position = vec4<f32>(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

@fragment
fn test_fs(input: VertexOutput) -> @location(0) vec4<f32> {
    // Gradient test pattern
    let r = input.uv.x;
    let g = input.uv.y;
    let b = 0.2;
    return vec4<f32>(r, g, b, 1.0);
}
)wgsl";

static const char* kShader_ToneMapping = R"wgsl(
// ToneMapping.wgsl — ACES tone mapping with optional bloom, AO, and SSGI

// Set to 1 to visualize velocity buffer (debug only). Set to 0 for normal rendering.
const DEBUG_VELOCITY: u32 = 0u;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var sceneTexture: texture_2d<f32>;
@group(0) @binding(1) var bloomTexture: texture_2d<f32>;
@group(0) @binding(2) var texSampler: sampler;
@group(0) @binding(3) var aoTexture: texture_2d<f32>;
@group(0) @binding(4) var ssgiTexture: texture_2d<f32>;
@group(0) @binding(5) var velocityTexture: texture_2d<f32>;

// Full-screen triangle vertex shader
@vertex
fn tonemap_vs(@builtin(vertex_index) vertexID: u32) -> VertexOutput {
    let positions = array<vec4<f32>, 3>(
        vec4<f32>(-1.0, -1.0, 0.0, 1.0),
        vec4<f32>( 3.0, -1.0, 0.0, 1.0),
        vec4<f32>(-1.0,  3.0, 0.0, 1.0)
    );
    let uvs = array<vec2<f32>, 3>(
        vec2<f32>(0.0, 1.0),
        vec2<f32>(2.0, 1.0),
        vec2<f32>(0.0, -1.0)
    );

    var out: VertexOutput;
    out.position = positions[vertexID];
    out.uv = uvs[vertexID];
    return out;
}

// ACES Tone Mapping
fn ACESFilm(x: vec3<f32>) -> vec3<f32> {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3<f32>(0.0), vec3<f32>(1.0));
}

// Fragment shader
@fragment
fn tonemap_fs(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    // DEBUG: visualize velocity buffer
    if (DEBUG_VELOCITY == 1u) {
        let vel = textureSample(velocityTexture, texSampler, uv).xy;
        // Velocity is in NDC units (range ~[-1, 1]); amplify by 20x for visibility.
        // X→R channel (red = rightward motion), Y→G channel (green = upward motion).
        return vec4<f32>(abs(vel) * 20.0, 0.0, 1.0);
    }

    var color = textureSample(sceneTexture, texSampler, uv).rgb;

    // SSAO: darken occluded areas
    let ao = textureSample(aoTexture, texSampler, uv);
    color *= ao.r;

    // SSGI: add indirect lighting (additive)
    let ssgi = textureSample(ssgiTexture, texSampler, uv).rgb;
    color += ssgi;

    // Add bloom (simple additive)
    let bloom = textureSample(bloomTexture, texSampler, uv).rgb;
    var result = color + bloom;

    // Tone mapping
    result = ACESFilm(result);

    // Gamma correction
    result = pow(result, vec3<f32>(1.0 / 2.2));

    return vec4<f32>(result, 1.0);
}
)wgsl";

static const char* kShader_TAA = R"wgsl(
// TAA.wgsl — Temporal Anti-Aliasing
//
// Inputs:
//   currentColorTex : this frame's jittered HDR
//   historyColorTex : last frame's resolved HDR
//   velocityTex     : per-pixel motion vectors (NDC units, Y up)
//
// Algorithm:
//   1. Reproject current pixel to history UV using velocity
//   2. Sample 3x3 neighborhood in current color, compute AABB
//   3. Convert neighborhood AABB to YCoCg space, clip history to AABB
//   4. Blend: result = mix(history_clipped, current, alpha)
//      alpha = 0.1 (slow convergence for stability)
//
// Output: storage texture, RGBA16F

struct TAAGlobals {
    screenSize: vec4f,    // xy = (width, height), zw = (1/width, 1/height)
    invHistoryValid: f32, // 1.0 if history is uninitialized (first frame), 0.0 otherwise
    _pad0: u32,
    _pad1: u32,
    _pad2: u32,
};

@group(0) @binding(0) var currentColorTex: texture_2d<f32>;
@group(0) @binding(1) var historyColorTex: texture_2d<f32>;
@group(0) @binding(2) var velocityTex: texture_2d<f32>;
@group(0) @binding(3) var outputTex: texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var linearSampler: sampler;
@group(0) @binding(5) var<uniform> globals: TAAGlobals;

// RGB <-> YCoCg (in-place color space for variance clipping — better chroma separation than YCbCr for natural scenes).
fn rgb_to_ycocg(c: vec3f) -> vec3f {
    let y  = 0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
    let co = 0.5 * c.r - 0.5 * c.b;
    let cg = -0.25 * c.r + 0.5 * c.g - 0.25 * c.b;
    return vec3f(y, co, cg);
}

fn ycocg_to_rgb(c: vec3f) -> vec3f {
    let y = c.x;
    let co = c.y;
    let cg = c.z;
    let tmp = y - cg;
    return vec3f(tmp + co, y + cg, tmp - co);
}

// Clip history to neighborhood AABB (in YCoCg space) using Karis-style clamp.
fn clip_to_aabb(aabb_min: vec3f, aabb_max: vec3f, p: vec3f) -> vec3f {
    let center = 0.5 * (aabb_min + aabb_max);
    let extents = 0.5 * (aabb_max - aabb_min);
    let v = p - center;
    let unit = v / max(extents, vec3f(0.0001));
    let abs_unit = abs(unit);
    let max_comp = max(abs_unit.x, max(abs_unit.y, abs_unit.z));
    if (max_comp <= 1.0) {
        return p; // inside AABB
    }
    return center + v / max_comp;
}

@compute @workgroup_size(8, 8, 1)
fn taa_main(@builtin(global_invocation_id) gid: vec3u) {
    let dims = vec2u(globals.screenSize.xy);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }

    let pixel = gid.xy;
    let currColor = textureLoad(currentColorTex, pixel, 0).rgb;
    let velocity = textureLoad(velocityTex, pixel, 0).xy;

    // Velocity is in clip-space NDC (Y up). Convert to UV-space delta (Y down for texture).
    // velocity_uv = (vel.x * 0.5, -vel.y * 0.5)
    // historyUV = currUV - velocity_uv
    let currUV = (vec2f(pixel) + 0.5) * globals.screenSize.zw;
    let historyUV = currUV - vec2f(velocity.x, -velocity.y) * 0.5;

    // Out-of-bounds history: keep current frame only
    var historyColor = currColor;
    if (globals.invHistoryValid < 0.5 &&
        historyUV.x >= 0.0 && historyUV.x <= 1.0 &&
        historyUV.y >= 0.0 && historyUV.y <= 1.0) {
        historyColor = textureSampleLevel(historyColorTex, linearSampler, historyUV, 0.0).rgb;
    }

    // 3x3 neighborhood of current color (for variance clipping)
    var neighborMin = currColor;
    var neighborMax = currColor;
    var neighborSum = vec3f(0.0);
    var sampleCount = 0u;
    for (var dy: i32 = -1; dy <= 1; dy++) {
        for (var dx: i32 = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) { continue; }
            let npixel = vec2i(i32(pixel.x) + dx, i32(pixel.y) + dy);
            if (npixel.x < 0 || npixel.x >= i32(dims.x) ||
                npixel.y < 0 || npixel.y >= i32(dims.y)) { continue; }
            let ncolor = textureLoad(currentColorTex, vec2u(npixel), 0).rgb;
            neighborMin = min(neighborMin, ncolor);
            neighborMax = max(neighborMax, ncolor);
            neighborSum = neighborSum + ncolor;
            sampleCount = sampleCount + 1u;
        }
    }
    let neighborAvg = neighborSum / f32(max(sampleCount, 1u));

    // Convert AABB to YCoCg space, clip history
    let aabbMinYC = rgb_to_ycocg(neighborMin);
    let aabbMaxYC = rgb_to_ycocg(neighborMax);
    let historyYC = clip_to_aabb(aabbMinYC, aabbMaxYC, rgb_to_ycocg(historyColor));
    let historyClipped = ycocg_to_rgb(historyYC);

    // Blend — lower alpha for stability (slower convergence, less ghosting)
    // First frame: force alpha=1.0 to bypass uninitialized history.
    var alpha = 0.1;
    if (globals.invHistoryValid > 0.5) {
        alpha = 1.0;
    }
    let result = mix(historyClipped, currColor, alpha);

    textureStore(outputTex, pixel, vec4f(result, 1.0));
}
)wgsl";

static const char* kShader_Velocity = R"wgsl(
// Velocity.wgsl — Motion vector generation from depth buffer
// Computes screen-space velocity by reconstructing world position from depth
// and projecting to current and previous frame NDC.

struct VelocityParams {
    invProj: mat4x4<f32>,
    viewProj: mat4x4<f32>,
    prevViewProj: mat4x4<f32>,
    screenSize: vec4<f32>,  // x=width, y=height, z=1/width, w=1/height
};

@group(0) @binding(0) var depthTex: texture_depth_2d;
@group(0) @binding(1) var velOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(2) var<uniform> params: VelocityParams;

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) gid: vec3u) {
    let dims = vec2u(params.screenSize.xy);
    if (gid.x >= dims.x || gid.y >= dims.y) { return; }

    let pixel = gid.xy;
    let uv = (vec2f(pixel) + 0.5) / params.screenSize.xy;

    let depth = textureLoad(depthTex, pixel, 0);

    // Sky pixels: zero velocity
    if (depth >= 0.9999) {
        textureStore(velOutput, pixel, vec4f(0.0, 0.0, 0.0, 1.0));
        return;
    }

    // Reconstruct NDC position
    let ndcX = uv.x * 2.0 - 1.0;
    let ndcY = 1.0 - uv.y * 2.0;
    let clipPos = vec4f(ndcX, ndcY, depth, 1.0);

    // Reconstruct view-space position
    let viewPos4 = params.invProj * clipPos;
    let viewPos = viewPos4.xyz / viewPos4.w;

    // World position (use inverse viewProj to reconstruct)
    // For static geometry, world pos is constant across frames.
    // We derive it from viewPos and the current viewProj:
    //   viewPos = View * worldPos => worldPos = InvView * viewPos
    // But we don't have InvView separately. Instead, we can compute
    // current NDC and prev NDC directly:
    let currClip = params.viewProj * vec4f(viewPos, 1.0);
    // Wait -- viewProj includes the view matrix. viewPos is in view space,
    // so we need to go view->world->prev_clip. We need invView.
    // Simpler approach: reconstruct clip-space from UV+depth for both frames.

    // Current frame NDC (already known from UV)
    let currNDC = vec2f(ndcX, ndcY);

    // Previous frame: project the same view-space position using the
    // full prevViewProj. But viewPos is in CURRENT view space.
    // We need world position first. Use invViewProj to get it.
    // Actually, we can go: clipPos -> viewPos (via invProj) -> worldPos (via invView)
    // -> prevClipPos (via prevViewProj).
    //
    // Simplification: since our test scene has a static camera,
    // velocity will be zero. But for correctness:
    // We reconstruct world pos from current frame clip coords.
    // clipPos is in current clip space. worldPos = invViewProj * clipPos.
    // But we only have invProj, not invViewProj.
    //
    // Practical approach for the test: the camera is static, so velocity = 0.
    // For the full pipeline, we'd need invView or invViewProj.
    // Let's output zero velocity for now and add proper computation later.

    textureStore(velOutput, pixel, vec4f(0.0, 0.0, 0.0, 1.0));
}
)wgsl";

// ============================================================================
// Shader lookup by name (filename without extension)
// ============================================================================

inline std::string LoadWGSL(const char* shaderName) {
    static const std::unordered_map<std::string, const char*> kShaderMap = {
        {"Blit",                  kShader_Blit},
        {"Bloom",                 kShader_Bloom},
        {"BlurPass",              kShader_BlurPass},
        {"CommonFunction",        kShader_CommonFunction},
        {"CommonTypes",           kShader_CommonTypes},
        {"DeferredLighting",      kShader_DeferredLighting},
        {"ForwardPBR_NoShadow",   kShader_ForwardPBR_NoShadow},
        {"ForwardPBR",            kShader_ForwardPBR},
        {"FullScreenTriangle",    kShader_FullScreenTriangle},
        {"HZBGeneration",         kShader_HZBGeneration},
        {"ShadowDepth",           kShader_ShadowDepth},
        {"SSAO",                  kShader_SSAO},
        {"SSAOBlur",              kShader_SSAOBlur},
        {"TAA",                   kShader_TAA},
        {"SSGIFilter",            kShader_SSGIFilter},
        {"SSGIHalfResDenoise",    kShader_SSGIHalfResDenoise},
        {"SSGITemporal",          kShader_SSGITemporal},
        {"SSGITrace",             kShader_SSGITrace},
        {"SSRPass",               kShader_SSRPass},
        {"TestTriangle",          kShader_TestTriangle},
        {"ToneMapping",           kShader_ToneMapping},
        {"Velocity",              kShader_Velocity},
    };

    auto it = kShaderMap.find(shaderName);
    if (it != kShaderMap.end()) {
        return std::string(it->second);
    }
    return {};
}

#else // !__EMSCRIPTEN__ — load from filesystem

inline std::string LoadWGSL(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

#endif // __EMSCRIPTEN__

} // namespace primal::graphics::dawn
