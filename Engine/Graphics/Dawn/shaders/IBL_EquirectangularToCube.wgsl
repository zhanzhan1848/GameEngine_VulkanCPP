@group(0) @binding(0) var equirectMap: texture_2d<f32>;
@group(0) @binding(1) var outputCube: texture_storage_2d_array<rgba16float, write>;

const invAtan = vec2<f32>(0.1591, 0.3183);

fn sampleSphericalMap(v: vec3<f32>) -> vec2<f32> {
    var uv = vec2<f32>(atan2(v.z, v.x), asin(v.y));
    uv = uv * invAtan + 0.5;
    return uv;
}

@compute @workgroup_size(16, 16, 1)
fn cs_main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let dims = vec2<u32>(textureDimensions(equirectMap));
    let faceSize = dims.x / 4u;
    if (gid.x >= faceSize || gid.y >= faceSize || gid.z >= 6u) { return; }

    let localUV = (vec2<f32>(gid.xy) + 0.5) / vec2<f32>(f32(faceSize), f32(faceSize)) * 2.0 - 1.0;

    var dir: vec3<f32>;
    switch (gid.z) {
        case 0u: { dir = normalize(vec3<f32>(1.0, -localUV.y, -localUV.x)); }
        case 1u: { dir = normalize(vec3<f32>(-1.0, -localUV.y, localUV.x)); }
        case 2u: { dir = normalize(vec3<f32>(localUV.x, 1.0, localUV.y)); }
        case 3u: { dir = normalize(vec3<f32>(localUV.x, -1.0, -localUV.y)); }
        case 4u: { dir = normalize(vec3<f32>(localUV.x, -localUV.y, 1.0)); }
        case 5u: { dir = normalize(vec3<f32>(-localUV.x, -localUV.y, -1.0)); }
        default: { dir = vec3<f32>(0.0, 0.0, 1.0); }
    }

    let sphereUV = sampleSphericalMap(dir);
    let iuv = clamp(vec2<u32>(sphereUV * vec2<f32>(dims)), vec2<u32>(0u), dims - 1u);
    let color = textureLoad(equirectMap, iuv, 0);
    textureStore(outputCube, gid.xy, gid.z, color);
}
