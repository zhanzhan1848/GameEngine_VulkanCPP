#version 450 core
// PFX decal：全屏实例（48B/实例 = 3×vec4）
layout(set = 0, binding = 1) readonly buffer Decals { vec4 decals[]; };  // 3×vec4/实例

layout(location = 0) out vec2 out_uv;
layout(location = 1) out float out_radius;
layout(location = 2) out vec4 out_center;
layout(location = 3) out vec4 out_color;
layout(location = 4) out float out_softness;

void main() {
    uint base = gl_InstanceIndex * 3u;
    vec4 center_radius = decals[base + 0u];
    vec4 color         = decals[base + 1u];
    vec4 params        = decals[base + 2u];
    vec2 quadPos[6] = vec2[6](vec2(-1,-1), vec2(1,-1), vec2(-1,1), vec2(-1,1), vec2(1,-1), vec2(1,1));
    vec2 q = quadPos[gl_VertexIndex];
    gl_Position = vec4(q, 0.0, 1.0);
    out_uv = q * 0.5 + 0.5;
    out_radius = max(center_radius.w, 1e-4);
    out_center = center_radius;
    out_color = color;
    out_softness = params.x;
}
