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
    _pad1: u32,
    _pad2: u32,
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
    @location(5) prevClip: vec4<f32>,  // Pass UN-divided clip pos for correct perspective interpolation
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

    // Velocity MRT: reconstruct current NDC from @builtin(position), compute delta from prevNDC
    // NOTE: WebGPU @builtin(position).xy is in framebuffer space (Y DOWN, origin top-left).
    // prevClip.w interpolation is perspective-correct, so dividing in fragment gives accurate prevNDC.
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
