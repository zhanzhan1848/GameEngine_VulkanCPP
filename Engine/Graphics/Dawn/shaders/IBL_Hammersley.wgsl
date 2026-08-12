const PI: f32 = 3.141592653589793;

fn hammersley(i: u32, n: u32) -> vec2<f32> {
    var bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    let r1 = f32(bits) * 2.3283064365386963e-10;
    return vec2<f32>(f32(i) / f32(n), r1);
}

fn importanceSampleGGX(xi: vec2<f32>, n: vec3<f32>, roughness: f32) -> vec3<f32> {
    let a = roughness * roughness;
    let phi = 2.0 * PI * xi.x;
    let cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    let sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    return vec3<f32>(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

fn distributionGGX(n: vec3<f32>, h: vec3<f32>, a: f32) -> f32 {
    let a2 = a * a;
    let NdotH = max(dot(n, h), 0.0);
    let d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + 0.0001);
}

fn geometrySchlickGGX_IBL(NdotV: f32, a: f32) -> f32 {
    let k = a * a / 2.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

fn geometrySmith_IBL(n: vec3<f32>, v: vec3<f32>, l: vec3<f32>, a: f32) -> f32 {
    return geometrySchlickGGX_IBL(max(dot(n, v), 0.0), a) *
           geometrySchlickGGX_IBL(max(dot(n, l), 0.0), a);
}
