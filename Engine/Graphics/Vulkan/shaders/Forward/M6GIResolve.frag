#version 450 core

// M6 GI Resolve (D3D12 M6 GI 重构 P5) — 探针 SH-L1 → 独立间接项全屏 pass。
// 顶点阶段复用 M3Post.vert 字节（同 M4DeferredLight/M6DeferredDirect）。
// 单一 GLSL 源双链消费（同 M1..M4；入口: main）。
//
// 语义契约：输出 rgb = albedo · E_h(N) = 只含间接光的独立贡献——不叠
// 直射（直射在 M6Composite 处与间接各叠加恰一次）、无 ambient 常数、
// 无 IBL、无天空（depth ≥ 1 背景 → vec4(0)，背景既非发射体也非接收体）。
// 自发光 ≡ 0：共同链 GBuffer RT2 = AO/rough 占位 + N.z 符号，无 emissive
// 通道（M6 §2 记录在案，不做假纹理）。GI 输入纹理 = M6ProbeAccum EMA 后
// 的探针 SH 网格（hist*0.9 + baked*0.1）——唯一时间递归即该 EMA，本 pass
// 不读任何含间接/最终图像的缓冲（无反馈回路）。
//
// SH 采样（M6 §4/§3）：SH 纹理 (8,8,24)，slab s∈{0,1,2} = 通道 {R,G,B}，
// texel(i,j,s·Nz+k) = (c00,c1-1,c10,c11)。三 slab 各做 trilinear 采样，
// z 权重 w_s = (zt + s·Nz)/(3·Nz)、zt = clamp(uvw.z·Nz, 0.5, Nz-0.5)——
// 把 z 方向三线性内插保持在各自 slab 内（跨 slab 混系数无意义且会串色）。
// 重建基（cosine-lobe 卷积 Â0=π、Â1=2π/3，再 /π；harness Lambert 无
// 1/π 单位，E_h = E_phys/π，M6 §3）：
//   basis = (0.282095, 0.325735·n.y, 0.325735·n.z, 0.325735·n.x)
//   E = max(vec3(dot(shR,basis), dot(shG,basis), dot(shB,basis)), 0)
// 采样位 P = worldPos + N·normalBias（gridMin.w 携带；0.25·min(cell) 米，
// 防自遮挡渗光）。无 GI 强度旋钮（不做强度调参——直接 + 间接各一次叠加）。
//
// 世界重建/法线解码 = M4DeferredLight 逐字同源约定：texel = gl_FragCoord
// 纹素直取；ndc = inUV*2-1（无 Y 翻转）；法线 = RG16F xy + material.a*2-1
// 符号重建 sqrt(1-x²-y²) 后 normalize。
//
// 绑定布局（set0 单 set；反射契约冻结）：
//   set 0 binding 0 = DeferredViewData UBO（80B，块名/成员与 M4 相同）
//   set 0 binding 1 = depthTexture（D32 → R32F；texelFetch）
//   set 0 binding 2 = gbufferAlbedo（RT0 RGBA8）
//   set 0 binding 3 = gbufferNormal（RT1 RG16F）
//   set 0 binding 4 = gbufferMaterial（RT2 RGBA8）
//   set 0 binding 5 = giProbeSH（texture3D SH 探针网格，EMA 后当前帧）
//   set 0 binding 6 = giProbeSampler（宿主 Linear/Clamp trilinear）
//   set 0 binding 7 = ResolveParams UBO（std140 48B：gridMin @0（w =
//                     normalBias）、gridInvExtent @16（xyz = 1/(max-min)）、
//                     gridDims @32（xyz = (Nx,Ny,Nz)））

#extension GL_EXT_samplerless_texture_functions : enable

layout(set = 0, binding = 0) uniform DeferredViewData {
    mat4 invViewProjection;   // 主相机 VP 的逆（宿主逐帧上传）
    vec4 cameraPos;           // xyz = 相机世界坐标（本 pass 不消费，块完整
                              // 80B 与 M4 DeferredViewData 同形）
} viewData;

layout(set = 0, binding = 1) uniform texture2D depthTexture;
layout(set = 0, binding = 2) uniform texture2D gbufferAlbedo;
layout(set = 0, binding = 3) uniform texture2D gbufferNormal;
layout(set = 0, binding = 4) uniform texture2D gbufferMaterial;
layout(set = 0, binding = 5) uniform texture3D giProbeSH;
layout(set = 0, binding = 6) uniform sampler giProbeSampler;

layout(set = 0, binding = 7) uniform ResolveParams {
    vec4 gridMin;             // @0  xyz = 网格世界盒 min，w = normalBias
    vec4 gridInvExtent;       // @16 xyz = 1/(max-min)
    vec4 gridDims;            // @32 xyz = (Nx,Ny,Nz)
} resolveParams;

layout(location = 0) in vec2 inUV;

layout(location = 0) out vec4 outIndirect;

void main() {
    // 1:1 纹素直取（gl_FragCoord.xy 像素中心 → 整数纹素）。
    ivec2 texel = ivec2(gl_FragCoord.xy);

    float depth = texelFetch(depthTexture, texel, 0).r;
    vec3 albedo = texelFetch(gbufferAlbedo, texel, 0).rgb;
    vec2 nxy = texelFetch(gbufferNormal, texel, 0).xy;
    vec4 mat = texelFetch(gbufferMaterial, texel, 0);

    // 背景（无几何）：非 GI 接收体——清零（含 alpha）。
    if (depth >= 1.0) {
        outIndirect = vec4(0.0);
        return;
    }

    // 深度重建世界位（生产同源公式；Y 约定差异见 M4DeferredLight 文件头）。
    vec2 ndcXY = vec2(inUV.x * 2.0 - 1.0, inUV.y * 2.0 - 1.0);
    vec4 ndc = vec4(ndcXY, depth, 1.0);
    vec4 worldPosH = viewData.invViewProjection * ndc;
    vec3 worldPos = worldPosH.xyz / max(worldPosH.w, 1e-6);

    // 法线重建（RG16F xy 直存 + rt2.a 符号；同 M4 逐字约定）。
    float zSign = mat.a * 2.0 - 1.0;
    vec3 N = normalize(vec3(nxy, zSign * sqrt(max(0.0, 1.0 - dot(nxy, nxy)))));

    // 探针采样位：法线偏移防自遮挡（normalBias = gridMin.w）。
    vec3 P = worldPos + N * resolveParams.gridMin.w;
    vec3 uvw = clamp((P - resolveParams.gridMin.xyz)
                       * resolveParams.gridInvExtent.xyz, vec3(0.0), vec3(1.0));

    // 三 slab 各自 trilinear（z 钳制在 slab 内，见文件头）；每 slab 取回
    // 完整 vec4 系数 (c00, c1-1, c10, c11)，dot 全 4 分量参与——basis.w =
    // 0.325735·n.x 正是与 c11 (Y11=x) 配对的项。
    float Nz = resolveParams.gridDims.z;
    float zt = clamp(uvw.z * Nz, 0.5, Nz - 0.5);
    vec4 shR = texture(sampler3D(giProbeSH, giProbeSampler),
                       vec3(uvw.xy, (zt + 0.0 * Nz) / (3.0 * Nz)));
    vec4 shG = texture(sampler3D(giProbeSH, giProbeSampler),
                       vec3(uvw.xy, (zt + 1.0 * Nz) / (3.0 * Nz)));
    vec4 shB = texture(sampler3D(giProbeSH, giProbeSampler),
                       vec3(uvw.xy, (zt + 2.0 * Nz) / (3.0 * Nz)));

    // E_h 重建（M6 §3 公式；cosine 卷积基向量，逐通道点积后钳非负）。
    vec4 basis = vec4(0.282095, 0.325735 * N.y, 0.325735 * N.z, 0.325735 * N.x);
    vec3 E = max(vec3(dot(shR, basis),
                      dot(shG, basis),
                      dot(shB, basis)), vec3(0.0));

    // 独立间接项：albedo × E_h；alpha=1（composite 只取 rgb）。
    outIndirect = vec4(albedo * E, 1.0);
}
