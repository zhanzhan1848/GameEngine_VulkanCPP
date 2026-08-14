// FullScreenTriangle.wgsl — Full-screen triangle vertex shader

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn fullscreen_triangle_vs(@builtin(vertex_index) vertexID: u32) -> VertexOutput {
    var output: VertexOutput;
    output.uv = vec2<f32>(f32((vertexID << 1u) & 2u), f32(vertexID & 2u));
    output.position = vec4<f32>(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}
