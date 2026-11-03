#version 450 core

// M3 Post (D3D12 M3 F3-2) — post 全屏三角形顶点阶段。
// 单一 GLSL 源双链消费（同 M1Forward/M2*；入口 main）。
//
// 无顶点输入/无 UBO/push：gl_VertexIndex 驱动 3 顶点覆盖屏幕
// （生产 ToneMapping.wgsl tonemap_vs 同构；跨链 vertex-ID 语义：
// Vulkan=gl_VertexIndex，spirv-cross HLSL=SV_VertexID，dxc 接受）。
//
// UV 映射 = 恒等直通：NDC (x,y) → uv ((x+1)/2, (y+1)/2)。
// 即 NDC (-1,-1) → uv (0,0)，外延 (3,-1) → uv (2,0)，(-1,3) → uv (0,2)。
// lit pass 与 post pass 同一 viewport 约定（D3D12 负 viewport 高度对两
// pass 对称生效，M1 §7.2-1 沿袭）→ 同一 NDC 位置在两 pass 落同一纹素，
// 采样恒等保持图像方向，两端读回同向、逐位可比。
// 注意：生产 ToneMapping.wgsl 的 uvs (0,1)/(2,1)/(0,-1) 是翻转映射——
// 生产链中 FinalBlit 再翻回（两次翻转=恒等）；M3 单级 post 直出没有
// 第二级翻转，必须用本恒等映射（初版抄生产惯例引入垂直翻转，已由
// POST=0/POST=1 逐像素预测核验定位并修正，见 M3 交付报告）。

layout(location = 0) out vec2 outUV;

void main() {
    // 覆盖屏幕的单三角形（大三角形裁剪法）。
    vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    // 恒等映射（见文件头注释）：uv = (pos + 1) / 2，角点外推线性。
    vec2 uvs[3] = vec2[3](
        vec2(0.0, 0.0),
        vec2(2.0, 0.0),
        vec2(0.0, 2.0)
    );
    int vi = int(gl_VertexIndex);
    outUV = uvs[vi];
    gl_Position = vec4(positions[vi], 0.0, 1.0);
}
