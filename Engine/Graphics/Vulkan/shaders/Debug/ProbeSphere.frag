#version 450 core
// ProbeSphere.frag — step 1 color: per-probe stable hue from the instance id
// + sun diffuse. ZERO descriptors (same class as the working bring-up test).
// Probe LIVE state coloring (L0 energy / buried red from the irradiance and
// depth SSBOs) is step 2, added only after this renders.

// Sun hardcoded during bring-up (VS-only push constants).

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vClip;
layout(location = 3) flat in uint vProbe;

layout(location = 0) out vec4 outColor;

void main() {
    // Stable per-probe hue (golden-ratio spacing around the wheel) so the
    // lattice reads as individual probes even without live data.
    // Per-probe stable hue (golden-ratio wheel) so the lattice reads as
    // individual probes.
    float h = fract(float(vProbe) * 0.6180339887);
    vec3 hue = clamp(vec3(1.5 - abs(h * 6.0 - 3.0) - 1.0,
                          1.5 - abs(h * 6.0 - 1.0) - 1.0,
                          1.5 - abs(h * 6.0 - 5.0) - 1.0),
                     vec3(0.25), vec3(1.0));
    vec3 N = normalize(vNormal);
    float ndl = max(dot(N, normalize(vec3(0.4, 0.8, 0.3))), 0.0);
    float shade = 0.35 + 0.65 * ndl;
    outColor = vec4(hue * shade, 1.0);
}
