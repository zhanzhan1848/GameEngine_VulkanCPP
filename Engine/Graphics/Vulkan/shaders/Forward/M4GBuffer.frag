#version 450 core

// M4 GBuffer (D3D12 M4 F4-2) — GBuffer 像素阶段：MRT×3 写出（冻结最小子集）。
// 单一 GLSL 源双链消费（同 M1Forward/M2/M3；入口: main）。
//
// MRT 布局（M4 冻结子集；T1 盘点对照见阶段文档 §4）：
//   location 0  SV_Target0  RGBA8_UNorm  albedo.rgb = 贴图 albedo × baseColor
//                                          （采样表达式与 M1Forward.frag 逐字
//                                           同源——量化进 8bit 是 forward/
//                                           deferred 唯一 albedo 差异源）
//   location 1  SV_Target1  RG16_Float   世界法线 xy 直接存储（f16/通道）
//                                          ——生产主路径为 BGRA8 ×0.5+0.5 编码
//                                           （非八面体；2026-10-23 追踪 §2.5），
//                                           共同最小路径取 RG16F 直存=更高
//                                           精度（选型偏差登记）；z 分量不存，
//                                           由 rt2.a 携带符号重建
//                                           sqrt(1-x²-y²)（单位法线）。
//   location 2  SV_Target2  RGBA8_UNorm  r=AO 占位(1.0) g=roughness 占位(0.5)
//                                          b=uv.x 备用 a=法线 z 符号(1=+ 0=−)
//                                          ——共同链光照数学=M1 固定参数
//                                           Blinn-Phong，不消费 rough/metal
//                                           （占位通道供 RHI_GBUFFER_DUMP
//                                           核验与后续阶段扩展）。
//   depth       D32_Float    光栅深度（与 forward pass 同一深度约定 [0,1] 正向）
//
// 生产对照（DeferredLighting 消费侧 ORM.g=roughness/orm.b=metallic；本布局
// rt2.g=roughness 占位、metallic 不入共同链——生产 ORM A=transmission 掩码、
// 共同链无透光项，均登记为冻结子集裁剪）。
//
// 绑定布局（复用 M1 material set1；set0 ViewData 仅 vert 消费）：
//   set 1 binding 0 = albedo texture2D（分离镜像） + binding 1 = sampler

layout(set = 1, binding = 0) uniform texture2D albedoTexture;
layout(set = 1, binding = 1) uniform sampler    albedoSampler;

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inWorldNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inBaseColor;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec2 outNormal;
layout(location = 2) out vec4 outMaterial;

void main() {
    // albedo：与 M1Forward.frag 逐表达式同源（同一采样/同一乘法）。
    vec3 albedo = texture(sampler2D(albedoTexture, albedoSampler), inUV).rgb
                * inBaseColor.rgb;
    outAlbedo = vec4(albedo, 1.0);

    // 世界法线（M1Forward.frag 同式 normalize），xy 直存 RG16F。
    vec3 N = normalize(inWorldNormal);
    outNormal = N.xy;

    // 材质占位 + z 符号打包（延迟侧重建 N.z = sign*sqrt(max(0,1-x²-y²))）。
    outMaterial = vec4(1.0, 0.5, inUV.x, N.z >= 0.0 ? 1.0 : 0.0);
}
