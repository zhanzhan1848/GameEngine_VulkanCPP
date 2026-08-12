// Bloom.wgsl — Bright pass extraction for bloom effect

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@group(0) @binding(0) var inputTexture: texture_2d<f32>;
@group(0) @binding(1) var texSampler: sampler;

@vertex
fn bloom_vs(@builtin(vertex_index) vertexID: u32) -> VertexOutput {
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

@fragment
fn bright_pass_fs(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> {
    let color = textureSample(inputTexture, texSampler, uv);
    let brightness = dot(color.rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
    if (brightness > 1.0) {
        return color;
    }
    return vec4<f32>(0.0);
}
