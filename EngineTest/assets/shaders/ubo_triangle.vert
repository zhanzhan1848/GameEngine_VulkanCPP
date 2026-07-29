#version 450

layout(set = 0, binding = 0) uniform UBO {
    vec4 color;
} ubo;

out gl_PerVertex { vec4 gl_Position; };

layout(location = 0) out vec4 fragColor;

const vec2 positions[3] = vec2[3](
    vec2( 0.0, -0.5),
    vec2( 0.5,  0.5),
    vec2(-0.5,  0.5)
);

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragColor = ubo.color;
}
