// Blit.wgsl — ForwardScene blit pipeline (HDR → tonemapped backbuffer).
//
// Kept separate from DeferredLighting.wgsl because the blit pipeline layout
// has only one binding (SampledImage at group(0) binding(0)) while the
// lighting pipeline has ViewData/SceneData UBs at the same slot. WGSL
// forbids two module-scope vars at the same group/binding with different
// types, so the entry points live in their own files.
//
// Pipeline layout (Dawn/WGSL):
//   @group(0) @binding(0)  var blitInput: texture_2d<f32>
//
// No sampler in the layout — fragmentBlit uses textureLoad with integer
// coordinates derived from the screen UV.

@group(0) @binding(0) var blitInput: texture_2d<f32>;

// === Tone mapping (matches DeferredLighting.metal:65-78) ===

fn ACESFilm(x: vec3<f32>) -> vec3<f32> {
    let a = 2.51;
    let b = 0.03;
    let c = 2.43;
    let d = 0.59;
    let e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), vec3<f32>(0.0), vec3<f32>(1.0));
}

fn toneMap(color: vec3<f32>) -> vec3<f32> {
    var c = color * 1.2;  // exposure
    c = ACESFilm(c);
    return pow(c, vec3<f32>(1.0 / 2.2));
}

// === Full-screen triangle vertex shader ===

struct VSOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vertexMain(@builtin(vertex_index) vid: u32) -> VSOut {
    var positions: array<vec4<f32>, 3>;
    positions[0] = vec4<f32>(-1.0, -1.0, 0.0, 1.0);
    positions[1] = vec4<f32>(-1.0,  3.0, 0.0, 1.0);
    positions[2] = vec4<f32>( 3.0, -1.0, 0.0, 1.0);

    var uvs: array<vec2<f32>, 3>;
    uvs[0] = vec2<f32>(0.0, 1.0);
    uvs[1] = vec2<f32>(0.0, -1.0);
    uvs[2] = vec2<f32>(2.0, 1.0);

    var out: VSOut;
    out.position = positions[vid];
    out.uv = uvs[vid];
    return out;
}

// === Blit fragment shader ===

struct BlitOut {
    @location(0) color: vec4<f32>,
};

@fragment
fn fragmentBlit(in: VSOut) -> BlitOut {
    let size = textureDimensions(blitInput);
    let fuv = clamp(vec2<f32>(size) * in.uv, vec2<f32>(0.0),
                    vec2<f32>(vec2<i32>(size) - 1));
    let iuv = vec2<i32>(fuv);
    let color = textureLoad(blitInput, iuv, 0).rgb;
    var out: BlitOut;
    out.color = vec4<f32>(toneMap(color), 1.0);
    return out;
}
