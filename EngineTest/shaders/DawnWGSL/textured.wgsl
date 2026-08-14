// Textured triangle with uniform buffer for Dawn WebGPU backend testing

struct Uniforms {
    color: vec4f,
}

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var texSampler: sampler;
@group(0) @binding(2) var texTexture: texture_2d<f32>;

struct VertexInput {
    @location(0) position: vec2f,
    @location(1) texCoord: vec2f,
}

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) texCoord: vec2f,
}

@vertex fn vs_main(input: VertexInput) -> VertexOutput {
    var output: VertexOutput;
    output.position = vec4f(input.position, 0.0, 1.0);
    output.texCoord = input.texCoord;
    return output;
}

@fragment fn fs_main(input: VertexOutput) -> @location(0) vec4f {
    let texColor = textureSample(texTexture, texSampler, input.texCoord);
    return texColor * uniforms.color;
}
