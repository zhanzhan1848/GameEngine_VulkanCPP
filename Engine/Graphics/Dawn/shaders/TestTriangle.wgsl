// TestTriangle.wgsl — Simple test shader for Dawn rendering pipeline validation

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn test_vs(@builtin(vertex_index) vertexID: u32) -> VertexOutput {
    var output: VertexOutput;
    output.uv = vec2<f32>(f32((vertexID << 1u) & 2u), f32(vertexID & 2u));
    output.position = vec4<f32>(output.uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

@fragment
fn test_fs(input: VertexOutput) -> @location(0) vec4<f32> {
    // Gradient test pattern
    let r = input.uv.x;
    let g = input.uv.y;
    let b = 0.2;
    return vec4<f32>(r, g, b, 1.0);
}
