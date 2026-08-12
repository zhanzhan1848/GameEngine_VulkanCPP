// Blit.wgsl — Fullscreen triangle blit shader for mipmap generation and texture copies

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn blit_vs(@builtin(vertex_index) vertexID: u32) -> VertexOutput {
    var positions = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0),
        vec2<f32>( 3.0, -1.0),
        vec2<f32>(-1.0,  3.0)
    );
    var uvs = array<vec2<f32>, 3>(
        vec2<f32>(0.0, 1.0),
        vec2<f32>(2.0, 1.0),
        vec2<f32>(0.0, -1.0)
    );

    var output: VertexOutput;
    output.position = vec4<f32>(positions[vertexID], 0.0, 1.0);
    output.uv = uvs[vertexID];
    return output;
}

@group(0) @binding(0) var srcTexture: texture_2d<f32>;
@group(0) @binding(1) var srcSampler: sampler;

@fragment
fn blit_fs(input: VertexOutput) -> @location(0) vec4<f32> {
    return textureSample(srcTexture, srcSampler, input.uv);
}
