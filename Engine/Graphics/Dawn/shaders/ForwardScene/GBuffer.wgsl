// GBuffer.wgsl — ForwardScene variant of Forward/GBuffer.metal.
//
// Pipeline layout (Dawn/WGSL):
//   @group(0) @binding(0)  var<uniform> viewData: ViewData
//   @group(0) @binding(1)  var<uniform> sceneData: SceneData
//   @group(1)              material set (albedo/normal/ORM + sampler)
//   @group(2) @binding(0)  var<uniform> pushConsts: PCGPushConsts
//
// Push constants are routed by Dawn to a dedicated bind group at index
// `setLayoutCount_`; for the GBuffer pipeline the layout has sets [global(0..1),
// material(0..3)], so N=2 → pushConsts lands at group(2) binding(0).
//
// Vertex input (slot 0, 32B stride, per-vertex):
//   @location(0) position        : vec3<f32>
//   @location(1) color_t_sign    : u32
//   @location(2) packed_normal   : u32 (low16=x, high16=y, +Z reconstructed)
//   @location(3) packed_tangent  : u32 (same encoding as normal)
//   @location(4) uv              : vec2<f32>
//
// Instance input (slot 1, 96B stride, per-instance):
//   @location(5..8)  transform columns (mat4x4 reconstructed from 4 vec4s)
//   @location(9)     baseColor (vec4)
//   @location(10)    roughness/metallic/alphaCutoff/pad (vec4)
//
// MRT output:
//   @location(0) albedo   : BGRA8_UNorm
//   @location(1) normal   : RGBA16_Float (world-space, encoded *0.5+0.5)
//   @location(2) orm      : BGRA8_UNorm  (R=ao, G=roughness, B=metallic)
//   @location(3) velocity : RG16_Float   (NDC delta, jitter removed)

struct ViewData {
    viewProjection: mat4x4<f32>,
    invViewProjection: mat4x4<f32>,
    previousViewProjection: mat4x4<f32>,
};

struct SceneData {
    model: mat4x4<f32>,
    lightPos: vec4<f32>,
    lightColor: vec4<f32>,
    reflectionPlane: vec4<f32>,
    reflectionPlane2: vec4<f32>,
    reflectionPlane3: vec4<f32>,
    previousModel: mat4x4<f32>,
    jitter: vec2<f32>,
    previousJitter: vec2<f32>,
    time: f32,
    _timePad: f32,
    viewPos: vec4<f32>,
    shadowMatrix0: mat4x4<f32>,
    shadowMatrix1: mat4x4<f32>,
};

struct PCGPushConsts {
    transform: mat4x4<f32>,
    use_instances: u32,
    _pad0: u32,
    _pad1: u32,
    _pad2: u32,
};

@group(0) @binding(0) var<uniform> viewData: ViewData;
@group(0) @binding(1) var<uniform> sceneData: SceneData;
@group(2) @binding(0) var<uniform> pushConsts: PCGPushConsts;

@group(1) @binding(0) var albedoMap: texture_2d<f32>;
@group(1) @binding(1) var normalMap: texture_2d<f32>;
@group(1) @binding(2) var ormMap: texture_2d<f32>;
@group(1) @binding(3) var matSampler: sampler;

struct VSOut {
    @builtin(position) position: vec4<f32>,
    @location(0) worldPos: vec3<f32>,
    @location(1) worldNormal: vec3<f32>,
    @location(2) worldTangent: vec3<f32>,
    @location(3) uv: vec2<f32>,
    @location(4) currentClip: vec4<f32>,
    @location(5) previousClip: vec4<f32>,
    @location(6) instanceBaseColor: vec4<f32>,
    @location(7) instanceRoughness: f32,
    @location(8) instanceMetallic: f32,
};

// Unpack packed u16 normal/tangent. Mirrors ForwardPBR.wgsl:115-126.
// Little-endian: low 16 bits = x, high 16 bits = y. Z reconstructed positive
// with sign flip from colorTSign byte (bit 1 of high byte) — matches engine
// content pipeline convention.
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
fn vertexMain(
    @location(0) position: vec3<f32>,
    @location(1) color_t_sign: u32,
    @location(2) packed_normal: u32,
    @location(3) packed_tangent: u32,
    @location(4) uv: vec2<f32>,
    @location(5) t_col0: vec4<f32>,
    @location(6) t_col1: vec4<f32>,
    @location(7) t_col2: vec4<f32>,
    @location(8) t_col3: vec4<f32>,
    @location(9) baseColor: vec4<f32>,
    @location(10) rma: vec4<f32>,
) -> VSOut {
    var out: VSOut;

    // Select instance transform vs push-constant fallback (matches GBuffer.metal:99-110).
    // WGSL select() doesn't support mat4x4 — manual if/else for the matrix.
    let instanceModel = mat4x4<f32>(t_col0, t_col1, t_col2, t_col3);
    var model: mat4x4<f32>;
    var baseColorOut: vec4<f32>;
    var roughnessOut: f32;
    var metallicOut: f32;
    if (pushConsts.use_instances != 0u) {
        model = instanceModel;
        baseColorOut = baseColor;
        roughnessOut = rma.x;
        metallicOut  = rma.y;
    } else {
        model = pushConsts.transform;
        baseColorOut = vec4<f32>(1.0, 1.0, 1.0, 1.0);
        roughnessOut = 0.5;
        metallicOut  = 0.0;
    }
    out.instanceBaseColor = baseColorOut;
    out.instanceRoughness = roughnessOut;
    out.instanceMetallic  = metallicOut;

    let worldPos = model * vec4<f32>(position, 1.0);
    out.worldPos = worldPos.xyz;

    // Upper-3x3 of model transforms normals/tangents to world space.
    // WFC tiles have non-identity transforms (rotation per tile), so this is required.
    let nT = normalize(unpackNormal(packed_normal, color_t_sign));
    let nT_tangent = normalize(unpackTangent(packed_tangent));
    let normalMatrix = mat3x3<f32>(
        vec3<f32>(model[0].xyz),
        vec3<f32>(model[1].xyz),
        vec3<f32>(model[2].xyz),
    );
    out.worldNormal  = normalize(normalMatrix * nT);
    out.worldTangent = normalize(normalMatrix * nT_tangent);

    // V-flip UV to match Metal sampler convention (ForwardPBR.wgsl:150).
    out.uv = vec2<f32>(uv.x, 1.0 - uv.y);

    let clip = viewData.viewProjection * worldPos;
    out.position = clip;
    out.currentClip = clip;

    // Previous-frame clip position. For static WFC tiles, instance transform is
    // constant across frames, so we reuse `model`. SceneData.previousModel is
    // reserved for the global animated geometry (not used by WFC demo).
    let prevWorldPos = model * vec4<f32>(position, 1.0);
    out.previousClip = viewData.previousViewProjection * prevWorldPos;

    return out;
}

struct FSOut {
    @location(0) albedo: vec4<f32>,
    @location(1) normal: vec4<f32>,
    @location(2) orm: vec4<f32>,
    @location(3) velocity: vec2<f32>,
};

@fragment
fn fragmentMain(in: VSOut) -> FSOut {
    var out: FSOut;

    // Sample material textures.
    let albedoSample = textureSample(albedoMap, matSampler, in.uv);
    out.albedo = albedoSample * in.instanceBaseColor;

    // ORM with fallback for missing/zero maps (matches GBuffer.metal:209-220).
    let ormSample = textureSample(ormMap, matSampler, in.uv);
    var ormValue: vec4<f32>;
    if (length(ormSample.rgb) < 0.01) {
        ormValue = vec4<f32>(1.0, in.instanceRoughness, in.instanceMetallic, 1.0);
    } else {
        ormValue = vec4<f32>(
            max(ormSample.r, 0.1),
            ormSample.g * in.instanceRoughness,
            ormSample.b * in.instanceMetallic,
            1.0
        );
    }
    out.orm = ormValue;

    // Normal mapping with fallback (matches GBuffer.metal:222-237).
    let normalSample = textureSample(normalMap, matSampler, in.uv).rgb;
    var encodedNormal: vec3<f32>;
    if (length(normalSample) > 0.1) {
        let tangentNormal = normalSample * 2.0 - 1.0;
        let N = normalize(in.worldNormal);
        let T = normalize(in.worldTangent);
        let B = cross(N, T);
        let TBN = mat3x3<f32>(T, B, N);
        encodedNormal = normalize(TBN * tangentNormal);
    } else {
        encodedNormal = normalize(in.worldNormal);
    }
    out.normal = vec4<f32>(encodedNormal * 0.5 + 0.5, 1.0);

    // Velocity: remove TAA jitter so static objects have zero motion.
    let currentNDC = in.currentClip.xy / max(in.currentClip.w, 0.0001);
    let previousNDC = in.previousClip.xy / max(in.previousClip.w, 0.0001);
    out.velocity = ((currentNDC - sceneData.jitter)
                   - (previousNDC - sceneData.previousJitter)) * 0.5;

    return out;
}
