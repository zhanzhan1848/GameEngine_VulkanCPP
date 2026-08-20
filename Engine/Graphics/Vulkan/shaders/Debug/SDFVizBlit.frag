#version 450 core
// SDFVizBlit.frag — simple passthrough blit (no tonemapping).
// Used by SDFVisualizationModule to copy the compute-shader output
// image to the backbuffer. Unlike Blit.frag, this does NOT apply ACES
// tonemapping because the SDF viz output is already in LDR [0,1].

layout(set = 0, binding = 0) uniform texture2D inputTex;
layout(set = 0, binding = 1) uniform sampler inputSamp;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = texture(sampler2D(inputTex, inputSamp), inUv);
    outColor = c;
}
