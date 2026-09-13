#version 450 core
layout(set = 0, binding = 0) uniform sampler2D scene_depth;
layout(set = 0, binding = 2) uniform VP {
    mat4 inv_view_proj;
    vec4 screen_size;
    vec4 decal_color;
    vec4 pad;
} vp;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in float in_radius;
layout(location = 2) in vec4 in_center;
layout(location = 3) in vec4 in_color;
layout(location = 4) in float in_softness;
layout(location = 0) out vec4 out_color;

void main() {
    float scene_d = texture(scene_depth, in_uv).r;
    if (scene_d >= 1.0) discard;
    vec4 ndc = vec4(in_uv.x * 2.0 - 1.0, in_uv.y * 2.0 - 1.0, scene_d, 1.0);
    vec4 world = vp.inv_view_proj * ndc;
    world /= world.w;
    float dist = length(world.xyz - in_center.xyz);
    float t = clamp(dist / in_radius, 0.0, 1.0);
    float falloff = 1.0 - smoothstep(1.0 - in_softness, 1.0, t);
    if (falloff <= 0.0) discard;
    out_color = vec4(in_color.rgb * falloff * in_color.a, falloff);
}
