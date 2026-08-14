#version 450 core

// T4.6.5 part 3 — Vulkan port of Forward/DeferredLighting.metal fragmentBlit
//
// Tone-map (ACES + exposure + gamma) the HDR lighting output to LDR.
// Entry point: main
//
// Descriptor layout (matches blit_set_layout_, cpp:562-565):
//   set 0 binding 0 = SampledImage (HDR lighting_output_)
//   set 0 binding 1 = Sampler (default_sampler_)
//
// Engine convention: separate SampledImage + Sampler descriptors combined
// via sampler2D(tex, samp) idiom (matches Skybox.frag).

#extension GL_EXT_samplerless_texture_functions : enable

layout(set = 0, binding = 0) uniform texture2D inputTex;
layout(set = 0, binding = 1) uniform sampler inputSamp;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

vec3 ACESFilm(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 toneMap(vec3 color) {
    float exposure = 1.2;
    color *= exposure;
    color = ACESFilm(color);
    color = pow(color, vec3(1.0 / 2.2));
    return color;
}

void main() {
    vec3 color = texture(sampler2D(inputTex, inputSamp), inUv).rgb;
    color = toneMap(color);
    outColor = vec4(color, 1.0);
}
