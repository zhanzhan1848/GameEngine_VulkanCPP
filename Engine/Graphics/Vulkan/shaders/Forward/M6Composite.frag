#version 450 core

// M6 Composite (D3D12 M6 GI 重构 P6) — 直射 + 间接恰一次合成全屏 pass。
// 顶点阶段复用 M3Post.vert 字节（同 M4/M6DeferredDirect/M6GIResolve）。
// 单一 GLSL 源双链消费（同 M1..M4；入口: main）。
//
// 语义契约：rgb = giDirect.rgb + giIndirect.rgb，alpha = 1——直射
// （M6DeferredDirect RT0，纯直射 HDR，已含阴影）与间接（M6GIResolve，
// albedo·E_h(N)，独立贡献）在全帧流中只在此处叠加一次（P7 post 只读不
// 再加光）。两个输入缓冲均无 ambient/天空/自发光项：完全阴影面 direct
// 恰 0，间接只来自探针 SH——合成结果 = 唯一最终光照（无双重直射、无
// 反馈）。背景像素（depth ≥ 1）：两输入 rgb 均为 0 → 输出 0（alpha 仍
// 1，保持与 M4 单 RT 输出同一 alpha 语义）。
//
// 无 UBO/push/采样器：texelFetch 1:1 纹素直取（gl_FragCoord → 整数
// 纹素，与上游两 pass 同一像素坐标系——全屏三角形恒等 UV 映射保证
// 三个 pass 纹素一一对应，无重采样）。
//
// 绑定布局（set0 单 set；反射契约冻结）：
//   set 0 binding 0 = giDirect（texture2D = ft.giDirect RGBA16F，P2 RT0）
//   set 0 binding 1 = giIndirect（texture2D = ft.giIndirect RGBA16F，P5 RT）

#extension GL_EXT_samplerless_texture_functions : enable

layout(set = 0, binding = 0) uniform texture2D giDirect;
layout(set = 0, binding = 1) uniform texture2D giIndirect;

layout(location = 0) out vec4 outColor;

void main() {
    ivec2 t = ivec2(gl_FragCoord.xy);
    outColor = vec4(texelFetch(giDirect, t, 0).rgb
                  + texelFetch(giIndirect, t, 0).rgb, 1.0);
}
