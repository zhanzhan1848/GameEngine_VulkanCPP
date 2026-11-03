#version 450 core

// M4 GBuffer (D3D12 M4 F4-2) — GBuffer 顶点阶段，M1Forward.vert 变体。
// 单一 GLSL 源双链消费（同 M1Forward/M2/M3；入口: main）。
//
// 与 M1Forward.vert 逐表达式同源（worldPos/worldNormal/uv/baseColor 输出、
// 32B 顶点布局、80B push constant、UV V 翻转惯例全部不变）——保证 GBuffer
// 光栅化与 forward 光栅化同一几何/同一插值，deferred 读回等价性唯一剩余
// 变量 = GBuffer 量化（M4 冻结子集设计的核心等价证据前提）。
// 差异仅一处：文件身份独立（M1 默认路径零改动纪律——M1Forward.vert 不动）。
//
// 绑定布局（复用 M1 global/material 两布局，宿主零新增 set）：
//   set 0 (global): binding 0 = ViewData UBO（vert 可见，80B，同 M1）
//   set 1 (material): frag 侧 albedo 采样对（见 M4GBuffer.frag）
// push constants: M1PushConsts 80B（mat4 model + vec4 baseColor，同 M1）。

layout(set = 0, binding = 0) uniform ViewData {
    mat4 viewProjection;
    vec4 cameraPos;   // xyz = 相机世界坐标，w 未用
} viewData;

layout(push_constant) uniform M1PushConsts {
    mat4 model;
    vec4 baseColor;   // xyz = 逐 draw 底色，w 未用
} pc;

layout(location = 0) in vec3  in_position;
layout(location = 1) in uint  in_colorTSign;   // M4 未消费（布局占位）
layout(location = 2) in uvec2 in_normal;
layout(location = 3) in uvec2 in_tangent;      // M4 未消费（布局占位）
layout(location = 4) in vec2  in_uv;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outWorldNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outBaseColor;

const float InvIntervals = 2.0 / ((1 << 16) - 1);

vec3 UnpackNormal(uvec2 p) {
    vec2 f = vec2(p);
    f = f * InvIntervals - 1.0;
    float d = dot(f, f);
    if (d > 1.0) {
        return vec3(0.0, 0.0, 1.0);  // invalid data fallback
    }
    float z = sqrt(max(0.0, 1.0 - d));
    return vec3(f.x, f.y, z);
}

void main() {
    vec4 worldPos = pc.model * vec4(in_position, 1.0);
    outWorldPos = worldPos.xyz;

    mat3 normalMatrix = mat3(pc.model[0].xyz, pc.model[1].xyz, pc.model[2].xyz);
    outWorldNormal = normalize(normalMatrix * UnpackNormal(in_normal));

    outUV = vec2(in_uv.x, 1.0 - in_uv.y);   // 生产惯例（GBuffer.vert:130 同式）

    outBaseColor = pc.baseColor;

    gl_Position = viewData.viewProjection * worldPos;
}
