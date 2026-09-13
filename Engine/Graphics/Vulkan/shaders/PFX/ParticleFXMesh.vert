#version 450 core
// PFX mesh 粒子顶点（N2 Vulkan 对称路径）——顶点拉取 + 实例化
// 绑定与 Metal 版 ParticleFXMeshPass 一致：
//   0 positions / 1 normals / 2 instances(48B) / 3 uniforms
layout(set = 0, binding = 0) readonly buffer Positions { vec4 positions[]; };
layout(set = 0, binding = 1) readonly buffer Normals   { vec4 normals[]; };
layout(set = 0, binding = 2) readonly buffer Instances { vec4 instances[]; };  // 3×vec4/实例
layout(set = 0, binding = 3) uniform VP { mat4 view_projection; vec4 color; } vp;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec4 out_color;

vec3 quat_rotate(vec4 q, vec3 v) {
    vec3 qv = q.xyz;
    vec3 t = 2.0 * cross(qv, v);
    return v + q.w * t + cross(qv, t);
}

void main() {
    uint base = gl_InstanceIndex * 3u;
    vec4 pos_and_pad   = instances[base + 0u];
    vec4 rot           = instances[base + 1u];
    vec4 scale_and_pad = instances[base + 2u];
    vec3 local = positions[gl_VertexIndex].xyz * max(scale_and_pad.x, 1e-4);
    vec3 world = pos_and_pad.xyz + quat_rotate(rot, local);
    out_normal = quat_rotate(rot, normals[gl_VertexIndex].xyz);
    gl_Position = vp.view_projection * vec4(world, 1.0);
    vec3 n = normalize(out_normal);
    vec3 l1 = normalize(vec3(0.4, 1.0, 0.6));
    out_color = vp.color * (0.35 + 0.65 * max(dot(n, l1), 0.0));
}
