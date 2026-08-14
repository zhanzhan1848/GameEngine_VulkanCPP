#version 450 core

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_orm;
layout(location = 3) out vec2 out_velocity;

void main() {
    out_albedo = vec4(vColor, 1.0);
    out_normal = vec4(0.5, 0.5, 1.0, 1.0);
    out_orm = vec4(1.0, 0.5, 0.0, 1.0);
    out_velocity = vec2(0.0);
}
