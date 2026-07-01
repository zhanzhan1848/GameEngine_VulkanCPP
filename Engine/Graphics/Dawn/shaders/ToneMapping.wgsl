// ToneMapping.wgsl — ACES tone mapping with optional bloom, AO, and SSGI

// Set to 1 to visualize velocity buffer (debug only). Set to 0 for normal rendering.
const DEBUG_VELOCITY: u32 = 0u;

// SSGI intensity = albedo proxy. Trace outputs <Li>_cosine_weighted which
// assumes albedo=1. Real diffuse surfaces have albedo 0.3-0.5, so we scale
// down here. Critical to keep SSGI values < ~1.5 so ACES preserves the
// warm color ratio — values >2 get crushed to white regardless of hue.
const SSGI_INTENSITY: f32 = 0.4;

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

    // NaN/Inf guard (diagnostic + defensive). x != x catches NaN; abs > 3.4e38
    // catches ±Inf. If this guard fires (screen turns black where garbage
    // color was), the NaN originates upstream in sceneColor — SSGI/SSR/TAA
    // chain. If it doesn't fire and "pure color" persists, the source is the
    // GBuffer/DeferredLighting writing the same finite-but-wrong value.
    let nanMask = color != color;
    let infMask = abs(color) > vec3<f32>(3.4e38);
    if (any(nanMask) || any(infMask)) {
        color = vec3<f32>(0.0);
    }

    // SSAO: darken occluded areas
    let ao = textureSample(aoTexture, texSampler, uv);
    color *= ao.r;

    // SSGI: add indirect lighting (additive, intensity-tuned)
    let ssgi = textureSample(ssgiTexture, texSampler, uv).rgb;
    color += ssgi * SSGI_INTENSITY;

    // Add bloom (simple additive)
    let bloom = textureSample(bloomTexture, texSampler, uv).rgb;
    var result = color + bloom;

    // Tone mapping
    result = ACESFilm(result);

    // Gamma correction
    result = pow(result, vec3<f32>(1.0 / 2.2));

    return vec4<f32>(result, 1.0);
}
