#version 450 core

// T4.6.5 part 3 — Vulkan port of Forward/DeferredLighting.metal fragmentBlit
//
// Copy the tone-mapped LDR output to the swapchain.
//
// ToneMappingPass is the sole HDR -> LDR conversion in StandardRenderPipeline.
// Applying exposure, ACES, and gamma here as well double-tonemaps the image and
// lifts most midtones close to white.
// Entry point: main
//
// Descriptor layout (matches blit_set_layout_, cpp:562-565):
//   set 0 binding 0 = SampledImage (LDR ToneMapping_Output)
//   set 0 binding 1 = Sampler (default_sampler_)
//
// Engine convention: separate SampledImage + Sampler descriptors combined
// via sampler2D(tex, samp) idiom (matches Skybox.frag).

#extension GL_EXT_samplerless_texture_functions : enable

layout(set = 0, binding = 0) uniform texture2D inputTex;
layout(set = 0, binding = 1) uniform sampler inputSamp;

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = texture(sampler2D(inputTex, inputSamp), inUv).rgb;
    outColor = vec4(color, 1.0);
}
