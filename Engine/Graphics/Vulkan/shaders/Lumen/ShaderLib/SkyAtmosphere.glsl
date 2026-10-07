// SkyAtmosphere.glsl — P1.1 (2026-09-15): 程序化解析天空单一事实源。
// 三个消费点共用同一函数，保证「可见天空 / GI miss 项 / (未来) IBL 底」
// 亮度与色度一致（路线图 P1.2）：
//   1. DeferredLighting.frag 背景像素（EvaluateSky，含日晕）
//   2. DDGI 探针逃逸射线 miss（EvaluateSkyBase × SkyColor.x 档位门）
//   3. SPGI 屏幕探针 DDGI 不可用回退（EvaluateSkyBase）
// 数值来源：2026-09-06 P1 天空接入 + B15 ×2 重标（zenith 5.50,5.76,6.00 →
// ACES×0.10 显示 ~216/255，对齐 UE 参考开口区 210,220,229）。
// 预留升级位：后续可替换为 Hillaire 2020 LUT 实现而保签名不变。
//
// 无嵌套 #include，include 方式：
//   Lumen/ 下 comp：     #include "ShaderLib/SkyAtmosphere.glsl"
//   Forward/ 下 frag：   #include "../Lumen/ShaderLib/SkyAtmosphere.glsl"

// 天空基色（不含太阳前向散射日晕）——供 GI miss 使用（漫反射探针不含
// 太阳盘/日晕的方向性热点，直射阳光由 SC 路径单独供给，避免重复计能）。
// sun-audit P2 同源 IBL：TestVulkanSponzaRenderGraph::InitializeIBL 在
// PRIMAL_SKY_IBL=1 时用本函数的 CPU 逐位镜像填充镜面环境——改这里的
// 常量必须同步该处。
vec3 EvaluateSkyBase(vec3 dir) {
    float up = clamp(dir.y, 0.0, 1.0);
    vec3 horizonCol = vec3(4.40, 4.20, 3.80); // 暖白地平雾
    vec3 zenithCol  = vec3(5.50, 5.76, 6.00); // 亮淡蓝天顶
    return mix(horizonCol, zenithCol, pow(up, 0.45));
}

// 完整天空（含前向散射日晕）——供可见天空背景像素使用。
// sun-audit §7 P2：日晕色度不再固定 (1,.85,.6)——固定值不会随太阳色温
// 改变。现在按太阳线性色（lightColor.rgb，BRDF 前的线性输入）归一出色度，
// 亮度以旧常量的 Y 归一（保留 2026-09-06 天空标定的能量档，色度/强度
// 独立控制）；输入接近黑时回退旧常量。GI miss（EvaluateSkyBase）仍不含
// 日晕，重复计能边界不变。
vec3 EvaluateSky(vec3 dir, vec3 sunDir, vec3 sunLinearColor) {
    vec3 sky = EvaluateSkyBase(dir);
    float sunAmount = pow(clamp(dot(dir, sunDir), 0.0, 1.0), 16.0);
    const vec3 kLum = vec3(0.2126, 0.7152, 0.0722);
    const float kHaloRefLum = dot(vec3(1.0, 0.85, 0.6), kLum);
    float lum = dot(sunLinearColor, kLum);
    vec3 halo = (lum > 1e-4) ? sunLinearColor * (kHaloRefLum / lum)
                             : vec3(1.0, 0.85, 0.6);
    sky += halo * sunAmount * 1.2;
    return sky;
}
