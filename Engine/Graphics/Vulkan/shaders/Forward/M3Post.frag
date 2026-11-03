#version 450 core

// M3 Post (D3D12 M3 F3-2/F3-3) — 共同最小 post 链像素阶段：
//   HDR(RGBA16F) × exposure → 生产同源 ACES 拟合 → BGRA8 UNorm 直写。
// 单一 GLSL 源双链消费（同 M1Forward/M2*；入口 main）。
//
// 顺序/格式契约（M3 PhaseDoc §3 + M1 §7.2-3）：
//   lit(HDR RGBA16F) → post(BGRA8) 两级；所有计算在线性域；
//   唯一编码点 = BGRA8 UNorm RT 写出（无硬件 sRGB 视图、无 shader pow）。
//   ——生产 ToneMapping.wgsl tonemap_fs 在 shader 内带 pow(1/2.2)（其
//   交换链为 UNORM 直出，pow 是生产侧的显示编码点）；共同最小链按
//   M1 §7.2-3 契约把编码推迟到 M7 真交换链 sRGB 视图，本文件无 pow
//   （选型偏差登记于 M3 交付报告，见任务书 T2 契约注释要求）。
//
// 绑定布局（宿主 post set0；反射契约冻结）：
//   set 0 binding 0 = PostParams UBO（32B，std140）
//   set 0 binding 1 = hdrTexture（分离镜像 texture2D = lit pass RGBA16F 输出）
//   set 0 binding 2 = hdrSampler（宿主 Linear/Clamp，同生产 ToneMapping 采样档）
//
// PostParams（宿主一次性上传；曲线系数从生产盘点逐字抄录）：
//   exposure  固定 EV 曝光乘子（轨迹 render.post.exposure，默认 1.0；
//             EV 微调经宿主 RHI_POST_EV：effective = base × 2^EV——生产
//             ToneExposureEV 同式；无 adapt 项，眼适应=历史依赖不入选）
//   a/b/c/d/e ACES 拟合系数（生产 ToneMapping.wgsl ACESFilm 逐位同值：
//             2.51 / 0.03 / 2.43 / 0.59 / 0.14）
//   生产链的 grading（contrast/saturation/vignette，显示域后处理）与
//   bloom 合成不在共同最小链内（bloom 生产默认关；grading 依赖其
//   pow 编码点语义）——不声明、不采样、不合成。

layout(set = 0, binding = 0) uniform PostParams {
    float exposure;   // @0
    float acesA;      // @4
    float acesB;      // @8
    float acesC;      // @12
    float acesD;      // @16
    float acesE;      // @20
    float _pad0;      // @24
    float _pad1;      // @28
} params;

layout(set = 0, binding = 1) uniform texture2D hdrTexture;
layout(set = 0, binding = 2) uniform sampler    hdrSampler;

layout(location = 0) in vec2 inUV;

layout(location = 0) out vec4 outColor;

// 生产同源 ACES 拟合（ToneMapping.wgsl ACESFilm；系数经 UBO 参数化，
// 数值不变）：x(a x + b) / (x(c x + d) + e)，clamp [0,1]。
vec3 ACESFilm(vec3 x) {
    return clamp((x * (vec3(params.acesA) * x + vec3(params.acesB))) /
                 (x * (vec3(params.acesC) * x + vec3(params.acesD)) + vec3(params.acesE)),
                 vec3(0.0), vec3(1.0));
}

void main() {
    // 线性域 HDR 采样（RGBA16F 无 sRGB 语义，读什么是什么）。
    vec3 hdr = texture(sampler2D(hdrTexture, hdrSampler), inUV).rgb;

    // 固定 EV 曝光（线性乘子）→ ACES 肩部滚降。
    vec3 result = ACESFilm(hdr * params.exposure);

    // 唯一编码点：BGRA8 UNorm 量化直写（[0,1] clamp 已由 ACES 曲线给出；
    // 无 pow、无 sRGB 视图——读回即线性域 tonemap 值，逐位可比）。
    outColor = vec4(result, 1.0);
}
