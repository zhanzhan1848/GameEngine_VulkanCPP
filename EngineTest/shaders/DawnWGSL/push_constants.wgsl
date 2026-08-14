// Push constant emulation via uniform buffer for Dawn WebGPU backend testing
// Binding 30 in group 0 is reserved for push constant emulation

struct PushConstants {
    data: array<u32, 64>,  // 256 bytes
}

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) color: vec3f,
}

@group(1) @binding(0) var<uniform> pushConstants: PushConstants;

@vertex fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    var pos = array<vec2f, 3>(
        vec2f( 0.0,  0.5),
        vec2f(-0.5, -0.5),
        vec2f( 0.5, -0.5)
    );
    var output: VertexOutput;
    output.position = vec4f(pos[vi], 0.0, 1.0);
    // Read color from push constant buffer (first 3 floats)
    let r = f32(pushConstants.data[0]);
    let g = f32(pushConstants.data[1]);
    let b = f32(pushConstants.data[2]);
    output.color = vec3f(r, g, b);
    return output;
}

@fragment fn fs_main(input: VertexOutput) -> @location(0) vec4f {
    return vec4f(input.color, 1.0);
}
