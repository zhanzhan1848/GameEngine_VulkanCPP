#version 450 core

// M2 Shadow Depth (D3D12 M2 F2-5) — 单级联 shadow map 深度 pass 顶点阶段。
// 单一 GLSL 源双链消费（glslang→SPIR-V / glslang→SPIR-V→spirv-cross→HLSL→dxc），
// 跨后端阴影贴图一致性以本文件为单一事实源。入口: main。
//
// T1 盘点对齐（生产 = GPUDrivenDrawPipeline::ExecuteShadowRaster 管线，
// 其 VS 为 Dawn/shaders/Nanite/ShadowDepth.wgsl 经 naga 编译）：
//   - depth-only（无 pixel shader，PSO renderTargetCount=0）
//   - CullMode::None（meshlet 缠绕方向不一，阴影惯例双面）
//   - rasterizer depth bias = 0（生产 PSO 未设 RasterizerState bias 字段，
//     全部 bias 走接收端 B17 slope 公式 + 本处 occluder 侧常数 clip.z bias）
//   - occluder 常数 bias：ShadowDepth.wgsl:114 `clip.z += 0.001`
//     （[0,1] 正向 Z，w=1 正交 → 直接 NDC 偏移，"更远离光源"）
//
// 绑定布局：
//   set 0 binding 0 = ShadowDepthData UBO（64B：light VP）
//   push constant   = M2ShadowPush（64B：model matrix）
//     （D3D12 侧 push constant 落位沿 spike §10 固化规则 = b1，b0 已被 UBO 占）
//
// 顶点输入：32B stride 生产布局的 position 通道（location 0）——
// 同生产 DepthOnly 前例（T4.6.5 part 13：深度 pass 只声明 position），
// 其余通道由顶点缓冲提供但本 shader 不消费。
//
// Y 不翻转（M1 §7.2-1 决策沿袭）：gl_Position 直接 lightVP * worldPos；
// D3D12 侧负 viewport 高度对本 pass 与 lit pass 对称生效，故 lit 端
// NDC→shadow UV 用朴素 xy*0.5+0.5，两端同式（不同于生产 ShadowFilter 的
// 0.5-y*0.5——那是补偿生产 WGSL VS 内的 Y-negate，M2 两条 pass 均无
// negate，自洽）。

layout(set = 0, binding = 0) uniform ShadowDepthData {
    mat4 lightViewProjection;
} shadowData;

layout(push_constant) uniform M2ShadowPush {
    mat4 model;
} pc;

layout(location = 0) in vec3 in_position;

void main() {
    vec4 worldPos = pc.model * vec4(in_position, 1.0);
    gl_Position = shadowData.lightViewProjection * worldPos;
    // 生产对齐（ShadowDepth.wgsl:114）：occluder 侧常数 bias。
    gl_Position.z += 0.001;
}
