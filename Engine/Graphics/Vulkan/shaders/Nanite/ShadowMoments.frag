#version 450 core
// ShadowMoments.frag — VSM moment writer (hand-written GLSL).
// Entry point: main (Metal: shadow_moments_fs in EngineTest/shaders/ShadowMoments.metal).
//
// Pairs with the existing ShadowDepth vertex shader (shadow_depth_vs, vertex
// pulling, 8 bindings) — this fragment only encodes the light-space depth as
// variance shadow map moments:
//     moments = vec2(z, z*z),  z = gl_FragCoord.z ∈ [0,1]
// Output target: RG32_Float (cleared to (1,1) = fully lit, matching Metal's
// ForwardRenderer VSM convention).

layout(location = 0) out vec2 outMoments;

void main() {
    float z = gl_FragCoord.z;
    outMoments = vec2(z, z * z);
}
