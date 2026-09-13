#version 450 core
layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 0) out vec4 out_color;
void main() {
    float edge = 1.0 - (in_uv.x - 0.5) * (in_uv.x - 0.5) * 4.0;
    float taper = edge * edge;
    float along = 0.25 + 0.75 * in_uv.y;
    out_color = vec4(in_color.rgb * along, in_color.a * taper);
}
