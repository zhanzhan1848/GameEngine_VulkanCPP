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
