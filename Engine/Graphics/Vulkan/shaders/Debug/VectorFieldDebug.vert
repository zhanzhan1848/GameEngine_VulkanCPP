#version 450 core
// VectorFieldDebug.vert — hand-written GLSL port of
// RHI/Shaders/Debug/VectorFieldDebug.metal (vector_field_debug_vs).
// Line-list: 2 vertices per line. Samples the vector-field 3D texture in the
// VERTEX stage (binding 0 is VS|PS in the descriptor layout), draws one
// direction line per texel.

layout(set = 0, binding = 0) uniform texture3D vfTexture;
layout(set = 0, binding = 2) uniform sampler linearSampler;

layout(std140, set = 0, binding = 1) uniform DebugUniforms {
    mat4 viewProjection;          // offset 0
    mat4 model;                   // offset 64
    float scale;                  // offset 128 (union: vf)
    float step_throttle;          // offset 132 — 'step' is a GLSL builtin name
    vec2 _pad0;                   // offsets 136/140
    uvec4 resolution;             // offset 144 — res[0..2] + pad
};

layout(location = 0) out vec3 vColor;

void main() {
    uint vertexID = gl_VertexIndex;
    uint lineIndex = vertexID / 2u;
    uint endpoint = vertexID % 2u;  // 0 = start, 1 = end

    uint actualLineIndex = lineIndex * uint(step_throttle);
    uvec3 res = resolution.xyz;

    if (res.x == 0u || res.y == 0u || res.z == 0u) {
        gl_Position = vec4(0.0);
        vColor = vec3(0.0);
        return;
    }

    // Decode linear index → x,y,z
    uint z = actualLineIndex / (res.x * res.y);
    uint temp = actualLineIndex % (res.x * res.y);
    uint y = temp / res.x;
    uint x = temp % res.x;

    if (z >= res.z) {
        gl_Position = vec4(0.0);
        vColor = vec3(0.0);
        return;
    }

    vec3 uvw = (vec3(x, y, z) + 0.5) / vec3(res);
    vec3 vector = texture(sampler3D(vfTexture, linearSampler), uvw).rgb;

    if (isnan(vector.x) || isnan(vector.y) || isnan(vector.z) ||
        length(vector) < 0.001) {
        gl_Position = vec4(0.0);  // collapse line
        vColor = vec3(0.0);
        return;
    }

    // Map [0,1] UVW to [-0.5, 0.5] model space; endpoint 1 offset by the vector.
    vec3 modelSpacePos = uvw - 0.5;
    if (endpoint == 1u) {
        modelSpacePos += vector * scale;
    }

    vec4 worldPos = model * vec4(modelSpacePos, 1.0);
    gl_Position = viewProjection * worldPos;

    vColor = abs(vector);
}
