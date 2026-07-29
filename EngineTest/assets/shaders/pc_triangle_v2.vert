#version 450

layout(push_constant) uniform PC {
    vec4 color;
} pc;

out gl_PerVertex { vec4 gl_Position; };

layout(location = 0) out vec4 fragColor;

const vec2 positions[3] = vec2[3](
    vec2( 0.0, -0.5),
    vec2( 0.5,  0.5),
    vec2(-0.5,  0.5)
);

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    // v2: tint the push constant color green-dominant (used by ReloadShader test)
    fragColor = vec4(pc.color.g, pc.color.r, pc.color.b, pc.color.a);
}
