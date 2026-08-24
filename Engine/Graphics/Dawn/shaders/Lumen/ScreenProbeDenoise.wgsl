// ScreenProbeDenoise.wgsl — Dawn port of ScreenProbeDenoise.comp (Vulkan GLSL)
//
// Vulkan Lumen Screen Probe GI — Confidence-weighted bilateral denoise.
//
// Light bilateral filter on GI output, using confidence from Gather:
//   - High confidence pixels are trusted (less filtered)
//   - Low confidence pixels borrow from high-confidence neighbors
//   - Geometry constraints (depth + normal) prevent bleeding across edges
//   Weights: depth * normal * spatial (Metal version uses geometry weights
//   only; the confidence channel is passed through for downstream temporal use).
//
// Dispatch: 2D, workgroups of (8, 8, 1) threads.
//
// === Binding layout (flat Vulkan numbering, textures + buffers share the
//     same number space — WebGPU has no Metal-style overlapping namespaces) ===
//
//   binding 0  texture_2d<f32>                    giInput        (sampled, manual bilinear)
//   binding 1  texture_2d<f32>                    normalTexture  (sampled, manual bilinear)
//   binding 2  texture_2d<f32>                    depthTexture   (SampledDepthImage,
//                                                                 texelFetch nearest)
//   binding 3  texture_storage_2d<rgba16float, write> giOutput   (GLSL writeonly
//                                  rgba16f image2D — WGSL has no write-only
//                                  texture_2d, storage texture is the exact
//                                  equivalent)
//   binding 4  uniform                            DenoiseParams  (32B)

// ============================================================================
// Struct definitions — must match the GLSL std140 block DenoiseParamsBlock
// field-for-field (32B: 3 float + pad, then 3 uint + pad).
// ============================================================================

struct DenoiseParams {
    sigma_depth:    f32,   //  0: Depth weight sharpness
    sigma_normal:   f32,   //  4: Normal weight power
    sigma_spatial:  f32,   //  8: Gaussian spatial falloff
    pad1:           f32,   // 12
    kernel_radius:  u32,   // 16
    render_width:   u32,   // 20
    render_height:  u32,   // 24
    pad2:           u32,   // 28
};

// ============================================================================
// Bindings — see layout table at file top
// ============================================================================

@group(0) @binding(0) var giInput: texture_2d<f32>;
@group(0) @binding(1) var normalTexture: texture_2d<f32>;
@group(0) @binding(2) var depthTexture: texture_2d<f32>;
@group(0) @binding(3) var giOutput: texture_storage_2d<rgba16float, write>;
@group(0) @binding(4) var<uniform> params: DenoiseParams;

// ============================================================================
// Helpers
// ============================================================================

fn decodeNormal(encoded: vec4<f32>) -> vec3<f32> {
    return normalize(encoded.xyz * 2.0 - 1.0);
}

// Manual bilinear RGB sample replicating Metal linear sampler (clamp_to_edge).
fn sampleBilinearRGB(tex: texture_2d<f32>, uv: vec2<f32>) -> vec3<f32> {
    let size = vec2<f32>(textureDimensions(tex));
    let px = uv * size - 0.5;
    let p0 = vec2<i32>(floor(px));
    let f = fract(px);

    let maxCoord = vec2<i32>(size) - vec2<i32>(1);
    let c00 = clamp(p0,                     vec2<i32>(0), maxCoord);
    let c10 = clamp(p0 + vec2<i32>(1, 0),   vec2<i32>(0), maxCoord);
    let c01 = clamp(p0 + vec2<i32>(0, 1),   vec2<i32>(0), maxCoord);
    let c11 = clamp(p0 + vec2<i32>(1, 1),   vec2<i32>(0), maxCoord);

    let s00 = textureLoad(tex, c00, 0).rgb;
    let s10 = textureLoad(tex, c10, 0).rgb;
    let s01 = textureLoad(tex, c01, 0).rgb;
    let s11 = textureLoad(tex, c11, 0).rgb;

    let a = mix(s00, s10, f.x);
    let b = mix(s01, s11, f.x);
    return mix(a, b, f.y);
}

// Manual bilinear RGBA (used for giInput to preserve the alpha confidence channel).
fn sampleBilinearRGBA(tex: texture_2d<f32>, uv: vec2<f32>) -> vec4<f32> {
    let size = vec2<f32>(textureDimensions(tex));
    let px = uv * size - 0.5;
    let p0 = vec2<i32>(floor(px));
    let f = fract(px);

    let maxCoord = vec2<i32>(size) - vec2<i32>(1);
    let c00 = clamp(p0,                     vec2<i32>(0), maxCoord);
    let c10 = clamp(p0 + vec2<i32>(1, 0),   vec2<i32>(0), maxCoord);
    let c01 = clamp(p0 + vec2<i32>(0, 1),   vec2<i32>(0), maxCoord);
    let c11 = clamp(p0 + vec2<i32>(1, 1),   vec2<i32>(0), maxCoord);

    let s00 = textureLoad(tex, c00, 0);
    let s10 = textureLoad(tex, c10, 0);
    let s01 = textureLoad(tex, c01, 0);
    let s11 = textureLoad(tex, c11, 0);

    let a = mix(s00, s10, f.x);
    let b = mix(s01, s11, f.x);
    return mix(a, b, f.y);
}

// ============================================================================
// Main Kernel: screen_probe_denoise
// ============================================================================

@compute @workgroup_size(8, 8, 1)
fn screen_probe_denoise(@builtin(global_invocation_id) gid_vec: vec3<u32>) {
    let pixel_pos: vec2<u32> = gid_vec.xy;

    let width: u32 = params.render_width;
    let height: u32 = params.render_height;

    if (pixel_pos.x >= width || pixel_pos.y >= height) {
        return;
    }

    let invRes = 1.0 / vec2<f32>(f32(width), f32(height));
    let centerUV = (vec2<f32>(f32(pixel_pos.x), f32(pixel_pos.y)) + 0.5) * invRes;

    let centerGI = sampleBilinearRGBA(giInput, centerUV);
    let centerDepth = textureLoad(depthTexture, vec2<i32>(pixel_pos), 0).r;
    let centerNormal = decodeNormal(vec4<f32>(sampleBilinearRGB(normalTexture, centerUV), 0.0));

    // Early out for sky pixels
    if (centerDepth <= 0.0001 || centerDepth >= 0.999) {
        textureStore(giOutput, vec2<i32>(pixel_pos), vec4<f32>(0.0, 0.0, 0.0, 0.0));
        return;
    }

    // Early out for zero GI — pass through without filtering
    let centerLum = dot(centerGI.rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
    if (centerLum < 0.0001) {
        textureStore(giOutput, vec2<i32>(pixel_pos), centerGI);
        return;
    }

    // Light bilateral filter — geometry weights only, no confidence influence
    let radius: u32 = min(params.kernel_radius, 2u);

    var filteredGI = vec3<f32>(0.0);
    var totalWeight: f32 = 0.0;

    for (var dy: i32 = -i32(radius); dy <= i32(radius); dy++) {
        for (var dx: i32 = -i32(radius); dx <= i32(radius); dx++) {
            let samplePos = vec2<i32>(pixel_pos) + vec2<i32>(dx, dy);

            if (samplePos.x < 0 || samplePos.y < 0 ||
                samplePos.x >= i32(width) || samplePos.y >= i32(height)) {
                continue;
            }

            let sampleUV = (vec2<f32>(f32(samplePos.x), f32(samplePos.y)) + 0.5) * invRes;
            let sampleGI = sampleBilinearRGBA(giInput, sampleUV);

            let sampleDepth = textureLoad(depthTexture, samplePos, 0).r;
            let sampleNormal = decodeNormal(vec4<f32>(sampleBilinearRGB(normalTexture, sampleUV), 0.0));

            // Depth weight
            let depthDiff = abs(centerDepth - sampleDepth);
            let w_depth = exp(-depthDiff * params.sigma_depth);

            // Normal weight
            let NdotN = max(0.0, dot(centerNormal, sampleNormal));
            let w_normal = pow(NdotN, params.sigma_normal);

            // Gaussian spatial weight
            let spatialDist2 = f32(dx * dx + dy * dy);
            let w_spatial = exp(-spatialDist2 / (2.0 * params.sigma_spatial * params.sigma_spatial));

            let weight = w_depth * w_normal * w_spatial;

            filteredGI += sampleGI.rgb * weight;
            totalWeight += weight;
        }
    }

    if (totalWeight > 1e-6) {
        filteredGI /= totalWeight;
    } else {
        filteredGI = centerGI.rgb;
    }

    // Pass through confidence from input (for temporal use downstream)
    textureStore(giOutput, vec2<i32>(pixel_pos), vec4<f32>(filteredGI, centerGI.a));
}
