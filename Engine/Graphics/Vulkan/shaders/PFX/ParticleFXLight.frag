#version 450 core
layout(location = 0) in vec4 in_color;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
void main() {
    vec2 c = (in_uv - 0.5) * 2.0;
    float r2 = dot(c, c);
    float falloff = clamp(1.0 - r2, 0.0, 1.0);
    falloff *= falloff;
    out_color = vec4(in_color.rgb * falloff, falloff);
}
