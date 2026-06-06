// ToneMapping.wgsl — ACES tone mapping with optional bloom, AO, and SSGI

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var sceneTexture: texture_2d<f32>;
@group(0) @binding(1) var bloomTexture: texture_2d<f32>;
@group(0) @binding(2) var texSampler: sampler;
@group(0) @binding(3) var aoTexture: texture_2d<f32>;
@group(0) @binding(4) var ssgiTexture: texture_2d<f32>;

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
