#version 450 core
// SDFDebug.vert — hand-written GLSL port of RHI/Shaders/Debug/SDFDebug.metal.
// Entry point: main (Metal: sdf_debug_vs).
// Slice quad billboarded through the model matrix (uniforms on binding 1).

layout(std140, set = 0, binding = 1) uniform DebugUniforms {
    mat4 viewProjection;   // offset 0
    mat4 model;            // offset 64
    float slice_depth;     // offset 128 (union: sdf)
    float mode;            // offset 132
    vec4 _pad0;
};

layout(location = 0) out vec2 vUV;

const vec3 quadVertices[6] = vec3[6](
    vec3(-1.0, -1.0, 0.0), vec3( 1.0, -1.0, 0.0), vec3(-1.0,  1.0, 0.0),
    vec3(-1.0,  1.0, 0.0), vec3( 1.0, -1.0, 0.0), vec3( 1.0,  1.0, 0.0)
);

void main() {
    vec3 pos = quadVertices[gl_VertexIndex];
    vec4 worldPos = model * vec4(pos, 1.0);
    gl_Position = viewProjection * worldPos;
    vUV = pos.xy * 0.5 + 0.5;
}
