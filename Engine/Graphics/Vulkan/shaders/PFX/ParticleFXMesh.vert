#version 450 core
// PFX mesh 粒子顶点（N2 Vulkan 对称路径）——顶点拉取 + 实例化
// 绑定与 Metal 版 ParticleFXMeshPass 一致：
//   0 positions / 1 normals / 2 instances(48B) / 3 uniforms
// P1.3 修复:positions/normals 为 CPU 紧凑 float3(12B 步长)→ 用 float 数组 +
// std430 紧凑步长(此前声明 vec4 造成 16B 步长错位,网格花屏);
// instances 为 48B/实例 = 3×vec4,vec4 步长本就正确。
layout(std430, set = 0, binding = 0) readonly buffer Positions { float positions[]; };
layout(std430, set = 0, binding = 1) readonly buffer Normals   { float normals[]; };
layout(std430, set = 0, binding = 2) readonly buffer Instances { vec4 instances[]; };  // 3×vec4/实例
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
    uint vi = gl_VertexIndex * 3u;
    vec3 local = vec3(positions[vi], positions[vi + 1u], positions[vi + 2u])
                 * max(scale_and_pad.x, 1e-4);
    vec3 world = pos_and_pad.xyz + quat_rotate(rot, local);
    out_normal = quat_rotate(rot, vec3(normals[vi], normals[vi + 1u], normals[vi + 2u]));
    gl_Position = vp.view_projection * vec4(world, 1.0);
    vec3 n = normalize(out_normal);
    vec3 l1 = normalize(vec3(0.4, 1.0, 0.6));
    // P1.4:实例色(pad 区 RGB)与全局调试色相乘
    vec3 tint = vec3(pos_and_pad.w, scale_and_pad.z, scale_and_pad.w);
    out_color = vp.color * (0.35 + 0.65 * max(dot(n, l1), 0.0)) * vec4(tint, 1.0);
}
