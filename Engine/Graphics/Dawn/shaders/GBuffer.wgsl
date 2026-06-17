// GBuffer.wgsl — Geometry pass for Deferred rendering (Phase 3b).
//
// Writes 5 MRT G-Buffer targets:
//   RT0: WorldPos (xyz) + unused (w)             RGBA16F
//   RT1: Normal (xyz)*0.5+0.5 + linearDepth (w)   RGBA16F
//   RT2: Albedo (rgb) + metallic (a)              RGBA8_sRGB
//   RT3: ORM (r=ao, g=roughness, b=0, a=0)        RGBA8
//   RT4: Velocity (rg) + 0,0                      RG16F
//
// 3b-2: albedo/ORM are constant defaults (no material textures yet).
// 3c deferred lighting will sample material textures and overwrite these.
//
// Pipeline layout mirrors DawnDepthPrepass: single descriptor set with one
// uniform buffer binding (per-object) using dynamic offset.
//
// Phase 3c-2: group 1 holds per-material textures (albedo/normal/ORM + sampler)
// matching the forward path's group 2 layout. Bound per-draw from the proxy's
// MaterialInstance descriptor set.

struct GBufferPerObject {
    world: mat4x4<f32>,                   // offset 0
    worldViewProjection: mat4x4<f32>,     // offset 64
    prevWorldViewProjection: mat4x4<f32>, // offset 128
};

@group(0) @binding(0) var<uniform> perObject: GBufferPerObject;

// Group 1 — material textures. Mirrors ForwardPBR.wgsl:88-91 (group 2 there).
// The MaterialInstance descriptor set is bound per-draw before each mesh.
@group(1) @binding(0) var albedoMap: texture_2d<f32>;
@group(1) @binding(1) var normalMap: texture_2d<f32>;
@group(1) @binding(2) var ormMap: texture_2d<f32>;
@group(1) @binding(3) var matSampler: sampler;

struct VSInput {
    @location(0) position: vec3<f32>,
    @location(1) color_t_sign: u32,
    @location(2) packed_normal: u32,
    @location(3) packed_tangent: u32,
    @location(4) uv: vec2<f32>,
};

struct VSOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) worldPos: vec3<f32>,
    @location(1) normal: vec3<f32>,
    @location(2) uv: vec2<f32>,
    @location(3) curClip: vec4<f32>,
    @location(4) prevClip: vec4<f32>,
    @location(5) tangent: vec3<f32>,
    @location(6) bitangent: vec3<f32>,
};

// Same unpack logic as ForwardPBR.wgsl:115-126.
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

// Same tangent unpack as ForwardPBR.wgsl:128-141 — needed for normal mapping
// (TBN basis) so deferred can apply tangent-space normal maps.
fn unpackTangent(packed: u32, colorTSign: u32) -> vec3<f32> {
    let tx = f32(packed & 0xFFFFu);
    let ty = f32((packed >> 16u) & 0xFFFFu);
    let inv = 2.0 / 65535.0;
    let txy = vec2<f32>(tx * inv - 1.0, ty * inv - 1.0);
    let d = dot(txy, txy);
    if (d > 1.0) { return vec3<f32>(1.0, 0.0, 0.0); }
    let z = sqrt(1.0 - d);
    let signs = (colorTSign >> 24u) & 0xFFu;
    let tSign = f32(signs & 0x01u) - 1.0;
    return normalize(vec3<f32>(txy.x, txy.y, z * tSign));
}

@vertex
fn gbuffer_vs(input: VSInput) -> VSOutput {
    var output: VSOutput;
    let worldPos = perObject.world * vec4<f32>(input.position, 1.0);
    let clip = perObject.worldViewProjection * vec4<f32>(input.position, 1.0);
    output.position = clip;
    output.worldPos = worldPos.xyz;
    output.normal = unpackNormal(input.packed_normal, input.color_t_sign);
    // ForwardPBR convention: V-flip UV for WebGPU sampler coordinate system.
    output.uv = vec2<f32>(input.uv.x, 1.0 - input.uv.y);
    output.curClip = clip;
    output.prevClip = perObject.prevWorldViewProjection * vec4<f32>(input.position, 1.0);

    // Tangent space for normal mapping (TBN). world transforms the tangent
    // and bitangent; normal will be transformed implicitly (deferred uses
    // world-space normal which the geometry normal already is).
    let T = normalize(perObject.world * vec4<f32>(unpackTangent(input.packed_tangent, input.color_t_sign), 0.0)).xyz;
    let N = output.normal;
    output.tangent = T;
    output.bitangent = normalize(cross(N, T));
    return output;
}

struct FSOutput {
    @location(0) rt0: vec4<f32>,
    @location(1) rt1: vec4<f32>,
    @location(2) rt2: vec4<f32>,
    @location(3) rt3: vec4<f32>,
    @location(4) rt4: vec4<f32>,
};

@fragment
fn gbuffer_fs(input: VSOutput) -> FSOutput {
    var output: FSOutput;

    output.rt0 = vec4<f32>(input.worldPos, 1.0);

    // Sample material textures — same flow as ForwardPBR.wgsl:240-245.
    let albedoTex = textureSample(albedoMap, matSampler, input.uv).rgb;
    let normalTex = textureSample(normalMap, matSampler, input.uv).rgb;
    let ormTex = textureSample(ormMap, matSampler, input.uv);

    // Tangent-space → world-space normal (matches ForwardPBR:249-255).
    let Nt = normalTex * 2.0 - 1.0;
    let worldNormal = normalize(Nt.x * input.tangent + Nt.y * input.bitangent + Nt.z * input.normal);

    // Encode world normal [-1,1] → [0,1] for storage. w stores linear view
    // depth (clip.w for perspective = -viewZ, the natural linear depth).
    output.rt1 = vec4<f32>(worldNormal * 0.5 + 0.5, input.curClip.w);

    // Albedo + metallic. Sponza ORM convention: R=occlusion, G=roughness,
    // B=metallic (matches ForwardPBR:244 unpacking).
    output.rt2 = vec4<f32>(albedoTex, ormTex.b);

    // RT3: occlusion (r), roughness (g). B/A unused (reserved for emissive).
    output.rt3 = vec4<f32>(ormTex.r, ormTex.g, 0.0, 0.0);

    // Velocity in NDC: (cur - prev) * 0.5. curClip/prevClip interpolation is
    // perspective-correct, so dividing in fragment gives accurate NDC. Matches
    // ForwardPBR.wgsl:362-376 velocity convention.
    let curNDC = input.curClip.xy / max(input.curClip.w, 0.0001);
    let prevNDC = input.prevClip.xy / max(input.prevClip.w, 0.0001);
    output.rt4 = vec4<f32>((curNDC - prevNDC) * 0.5, 0.0, 0.0);

    return output;
}
