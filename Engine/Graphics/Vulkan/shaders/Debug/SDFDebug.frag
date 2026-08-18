#version 450 core
// SDFDebug.frag — hand-written GLSL port of SDFDebug.metal (sdf_slice_debug_fs).
// Samples the 3D SDF texture at the slice depth; blue-cyan inside, red-yellow
// outside, white isoline at the zero crossing. Vulkan uses a separate
// sampler binding (2) — Vulkan forbids sampling a descriptor-type
// SampledImage without one; Metal uses a constexpr sampler.

layout(set = 0, binding = 0) uniform texture3D sdfTexture;
layout(set = 0, binding = 2) uniform sampler linearSampler;

layout(std140, set = 0, binding = 1) uniform DebugUniforms {
    mat4 viewProjection;
    mat4 model;
    float slice_depth;
    float mode;
    vec4 _pad0;
};

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 uvw = vec3(vUV.x, vUV.y, slice_depth);
    float dist = texture(sampler3D(sdfTexture, linearSampler), uvw).r;

    vec3 color;
    float d = dist * 10.0;  // scale for visualization

    if (d < 0.0) {
        // Inside: blue to cyan
        color = mix(vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), 1.0 + d);
        if (d < -1.0) color = vec3(0.0, 0.0, 1.0);
    } else {
        // Outside: red to yellow
        color = mix(vec3(1.0, 0.0, 0.0), vec3(1.0, 1.0, 0.0), d);
        if (d > 1.0) color = vec3(1.0, 1.0, 0.0);
    }

    // Isoline at 0
    if (abs(dist) < 0.01) color = vec3(1.0, 1.0, 1.0);

    outColor = vec4(color, 0.8);
}
