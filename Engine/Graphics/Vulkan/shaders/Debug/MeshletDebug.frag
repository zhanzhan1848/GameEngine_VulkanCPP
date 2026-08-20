#version 450 core
// MeshletDebug.frag — hand-written GLSL port of MeshletDebug.metal (meshlet_debug_fs).
// Barycentric wireframe overlay, same smoothstep math as Metal.

layout(std140, set = 0, binding = 0) uniform DebugUniforms {
    mat4 viewProjection;
    mat4 model;
    uint mesh_id;
    float wireframe_enabled;
    vec4 _pad0;
};

layout(location = 0) in vec3 vColor;
layout(location = 1) in float vAlpha;
layout(location = 2) in vec3 vBarycentric;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = vColor;

    if (wireframe_enabled > 0.5) {
        vec3 d = fwidth(vBarycentric);
        vec3 a3 = smoothstep(vec3(0.0), d * 1.5, vBarycentric);
        float minBary = min(min(a3.x, a3.y), a3.z);
        color = mix(vec3(0.0), color, minBary);
    }

    outColor = vec4(color, vAlpha);
}
