#version 450 core
// VectorFieldDebug.frag — hand-written GLSL port (vector_field_debug_fs).

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(vColor, 1.0);
}
