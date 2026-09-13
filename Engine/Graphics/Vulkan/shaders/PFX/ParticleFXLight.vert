#version 450 core
// PFX light：HDR 光斑点精灵展开（0 instances 绑定 32B/实例 = 2×vec4）
layout(set = 0, binding = 0) readonly buffer Lights { vec4 lights[]; };   // [pos_intensity, color_rgb_pad] ×N
layout(set = 0, binding = 1) uniform VP { mat4 view_projection; vec4 pad; } vp;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_uv;

void main() {
    uint base = gl_InstanceIndex * 2u;
    vec4 pos_intensity = lights[base + 0u];
    vec4 color_rgb_pad = lights[base + 1u];
    float intensity = max(pos_intensity.w, 0.0);
    vec3 world = pos_intensity.xyz;

    vec4 centerClip = vp.view_projection * vec4(world, 1.0);
    float clipW = max(centerClip.w, 0.001);
    vec2 quadPos[6] = vec2[6](vec2(-1,-1), vec2(1,-1), vec2(-1,1), vec2(-1,1), vec2(1,-1), vec2(1,1));
    vec2 q = quadPos[gl_VertexIndex];
    float screenRadius = 0.02 * (1.0 + intensity * 0.05) / clipW;
    centerClip.x += q.x * screenRadius * clipW;
    centerClip.y += q.y * screenRadius * clipW;
    gl_Position = centerClip;
    out_color = vec4(color_rgb_pad.rgb * intensity, 1.0);
    out_uv = q * 0.5 + 0.5;
}
