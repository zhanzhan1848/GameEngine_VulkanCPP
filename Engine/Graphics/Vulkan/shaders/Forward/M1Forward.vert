#version 450 core

// M1 Forward (D3D12 M1 F1-7) — 最小 forward 顶点阶段，单一 GLSL 源同时
// 供 Vulkan (glslang→SPIR-V) 与 D3D12 (glslang→SPIR-V→spirv-cross→HLSL→dxc)
// 两条编译链消费；跨后端像素一致性以本文件为单一事实源。
// 入口: main（跨链恒 main，见 Docs/RHI/D3D12_Shader_Spike_Report.md §5）。
//
// 绑定布局（与 TestRHISponzaShared 宿主 CreateDescriptorLayouts 对齐）：
//   set 0 (global): binding 0 = ViewData UBO（vert+frag 可见，80B）
// push constants（PCGPushConsts 80B 惯例，std140 对齐技巧同 GBuffer.vert）：
//   mat4 model (64B) + vec4 baseColor (16B) = 80B——vec4 打包避免 uvec3 的
//   16B 对齐陷阱；baseColor 经 vertex output 传给像素阶段（frag 不声明
//   push constant，跨阶段寄存器分配由各后端单点处理）。
//
// 顶点输入（32B stride，同生产 GBuffer 布局；M1 最小路径不消费
// colorTSign/tangent——字段仍按生产布局声明，保持单一顶点缓冲格式）：
//   location 0  vec3  position    (offset 0,  RGB32_Float)
//   location 1  uint  colorTSign  (offset 12, R32_UInt)
//   location 2  uvec2 normal      (offset 16, RG16_UInt)
//   location 3  uvec2 tangent     (offset 20, RG16_UInt)
//   location 4  vec2  uv          (offset 24, RG32_Float)
//
// Y 不翻转（M1 §7.2-1 决策）：gl_Position 直接 viewProjection * worldPos；
// D3D12 侧用负 viewport 高度 + cull 翻转对齐，两端原始读回同向。

layout(set = 0, binding = 0) uniform ViewData {
    mat4 viewProjection;
    vec4 cameraPos;   // xyz = 相机世界坐标（frag 光照用），w 未用
} viewData;

layout(push_constant) uniform M1PushConsts {
    mat4 model;
    vec4 baseColor;   // xyz = 逐 draw 底色（无贴图 mesh 的染色），w 未用
} pc;

layout(location = 0) in vec3  in_position;
layout(location = 1) in uint  in_colorTSign;   // M1 未消费
layout(location = 2) in uvec2 in_normal;
layout(location = 3) in uvec2 in_tangent;      // M1 未消费
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

    // 世界矩阵按 PCGPushConsts 惯例为刚性变换（Sponza 全 identity model）；
    // 一般化取左上 3x3（无非均匀缩放场景即法线矩阵）。
    mat3 normalMatrix = mat3(pc.model[0].xyz, pc.model[1].xyz, pc.model[2].xyz);
    outWorldNormal = normalize(normalMatrix * UnpackNormal(in_normal));

    // 生产惯例（GBuffer.vert:130）：UV V 翻转以匹配 stbi 行序上传的贴图；
    // 属共享源内行为，两端一致（M1 §7.2-4：宿主/贴图数据层不再翻转）。
    outUV = vec2(in_uv.x, 1.0 - in_uv.y);

    outBaseColor = pc.baseColor;

    gl_Position = viewData.viewProjection * worldPos;
}
