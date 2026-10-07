// ProbeSphere.metal — DebugProbeSphereModule（B1 L3 probe 球 overlay）的 MSL
// 着色器。与 Engine/Graphics/Vulkan/shaders/Debug/ProbeSphere.{vert,frag}
// 同契约的 Metal 版：
//   顶点流（展开式，stride 28B）：pos(3f)@0 normal(3f)@12 probeIdx(1f)@24
//   Push constants（Metal RHI 约定 [[buffer(2)]]，Vertex 段，112B）：
//     mat4 view_proj | float4 sun_dir | float4 cam_pos
//   无 descriptor 依赖（UBO 槽位保留但不着色器读）。
// 颜色：per-probe 黄金比例稳定色相 + 方向光漫反射。相机 2m 内的探针推出
// 裁剪域（相机坐在探针格内，最近的球会糊满屏幕）。
#include <metal_stdlib>
using namespace metal;

struct ProbeSphereVSIn {
    float3 pos   [[attribute(0)]];
    float3 nrm   [[attribute(1)]];
    float  probe [[attribute(2)]];
    // 2026-09-17 审计 §8.3：重定位分类（2.0=无状态数据 → legacy 色相；
    // 0=未 trace 灰 / 0.5=借格红 / 0.75=重定位黄 / 1.0=本格自由绿）。
    float  state [[attribute(3)]];
    // 探针中心——相机隐藏按中心判定（逐顶点判定会产生拉伸三角形）。
    float3 center [[attribute(4)]];
};

struct ProbeSpherePC {
    float4x4 view_proj;
    float4   sun_dir;
    float4   cam_pos;   // xyz = camera, w = hide radius (m)
    float4   misc;      // [P0] x = 着色模式（0=state 1=radiance 2=confidence 3=variance）
};

struct ProbeSphereVSOut {
    float4 clip   [[position]];
    float3 world;
    float3 nrm;
    float  probe;
    float  state;
    float  mode;
    float  total;   // [P2 SoA] probeTotal
};

vertex ProbeSphereVSOut probe_sphere_vs(
    ProbeSphereVSIn in [[stage_in]],
    constant ProbeSpherePC& pc [[buffer(2)]])
{
    ProbeSphereVSOut o;
    o.world = in.pos;
    o.nrm   = in.nrm;
    o.probe = in.probe;
    o.state = in.state;
    o.mode  = pc.misc.x;
    o.total = pc.misc.y;

    float4 clip = pc.view_proj * float4(in.pos, 1.0);
    // 相机隐藏：按探针中心（全部 60 顶点一致塌缩 → 干净的退化裁剪）。
    if (distance(in.center, pc.cam_pos.xyz) < pc.cam_pos.w) {
        clip = float4(2.0, 2.0, 2.0, 1.0);  // 裁剪域外
    }
    o.clip = clip;
    return o;
}

fragment float4 probe_sphere_fs(ProbeSphereVSOut in [[stage_in]])
{
    // 2026-09-17：状态色优先（与 Vulkan ProbeSphere.frag 同一调色板）。
    if (in.state < 1.5) {
        float3 stateColor;
        if (in.state < 0.125)      stateColor = float3(0.45, 0.45, 0.48);
        else if (in.state < 0.625) stateColor = float3(0.90, 0.15, 0.10);
        else if (in.state < 0.875) stateColor = float3(0.95, 0.80, 0.10);
        else                       stateColor = float3(0.15, 0.85, 0.25);
        float3 N = normalize(in.nrm);
        float ndl = 0.5 + 0.5 * dot(N, normalize(float3(0.4, 0.8, 0.3)));
        return float4(stateColor * (0.35 + 0.65 * ndl), 1.0);
    }
    float h = fract(in.probe * 0.6180339887);
    float3 hue = clamp(float3(1.5 - abs(h * 6.0 - 3.0) - 1.0,
                              1.5 - abs(h * 6.0 - 1.0) - 1.0,
                              1.5 - abs(h * 6.0 - 5.0) - 1.0),
                       float3(0.25), float3(1.0));
    float3 N = normalize(in.nrm);
    float ndl = max(dot(N, normalize(float3(0.4, 0.8, 0.3))), 0.0);
    return float4(hue * (0.35 + 0.65 * ndl), 1.0);
}


// ----------------------------------------------------------------------------
// B1 L3.2: irradiance 采样入口——球面法线即"probe 中心 → 片元"方向，直接
// 评估该 probe 的 SH9 系数（DDGIUpdateIrradiance 写入的 probeIdx×9 float4
// 布局）。SH 数学逐行镜像 CommonFunction.metal::EvalSH9Irradiance。
// ----------------------------------------------------------------------------
fragment float4 probe_sphere_irradiance_fs(
    ProbeSphereVSOut in [[stage_in]],
    device const float4* irradiance [[buffer(1)]])
{
    float3 N = normalize(in.nrm);
    uint base = uint(in.probe) * 9u;

    const float A0 = 3.14159265;
    const float A1 = 2.09439510;
    const float A2 = 0.78539816;
    const float C0 = 0.28209479;
    const float C1 = 0.48860251;
    const float C2_0 = 1.09254843;
    const float C2_1 = 0.31539156;
    const float C2_2 = 0.54627421;

    float3 result = float3(0.0);
    result += irradiance[base + 0u].xyz * (A0 * C0);
    result += irradiance[base + 1u].xyz * (A1 * -C1 * N.y);
    result += irradiance[base + 2u].xyz * (A1 * C1 * N.z);
    result += irradiance[base + 3u].xyz * (A1 * -C1 * N.x);
    result += irradiance[base + 4u].xyz * (A2 * C2_0 * N.x * N.y);
    result += irradiance[base + 5u].xyz * (A2 * -C2_0 * N.y * N.z);
    result += irradiance[base + 6u].xyz * (A2 * C2_1 * N.z);
    result += irradiance[base + 7u].xyz * (A2 * -C2_1 * N.x);
    result += irradiance[base + 8u].xyz * (A2 * C2_2 * (3.0 * N.z * N.z - 1.0));

    // tone map 防过曝（SH9 直接值可超 1）
    float3 rgb = result / (1.0 + max(max(result.r, result.g), result.b));
    return float4(clamp(rgb, 0.0, 1.0), 1.0);
}

// ----------------------------------------------------------------------------
// [WRC] 探针球 WRC 采样入口（PassDebugView WRC Probes 开关）：
// probe_data 布局（WRCTypes.h）：每探针 stride = K*2+4 u32；方向 d 的
// radiance 打包在 base + d*2（packHalf2x16(rgb.rg) | packHalf2x16(rgb.b,0)）；
// base + K*2 + 2 = state（bit0 valid，bit1 inGeometry）。方向集与 trace 侧
// 同构重推（Fibonacci，固定坐标系，WRCTraceFinalize.comp 同式）。
// 球面法线方向 = cos 加权混合 64 方向 radiance（debug 可读性优先，非精确
// 球面积分）；invalid 探针暗灰、inGeometry 偏红。
// ----------------------------------------------------------------------------
fragment float4 probe_sphere_wrc_fs(
    ProbeSphereVSOut in [[stage_in]],
    device const uint* probe_data [[buffer(1)]])
{
    float3 N = normalize(in.nrm);
    const uint K = 64u;                 // kWRCDirsPerProbe
    const uint radStride = K * 2u;      // kWRCProbeRadianceStrideU32 [P2 SoA]
    // [P2 SoA]: metaBase = probeTotal × K×2 + probe×4（misc.y = probeTotal）。
    const uint metaBase = uint(in.total) * radStride + uint(in.probe) * 4u;
    const uint rbase = uint(in.probe) * radStride;

    uint state = probe_data[metaBase + 2u];
    const bool valid = (state & 1u) != 0u;
    const bool inGeo = (state & 2u) != 0u;

    // [P0] mode 2 = confidence（消费计数热图，与 Vulkan 孪生同款）。
    if (in.mode > 1.5 && in.mode < 2.5) {
        if (!valid) return float4(0.10, 0.10, 0.11, 1.0);
        const uint consume = (state >> 16u) & 0xFFFFu;
        float t = clamp(float(consume) / 64.0, 0.0, 1.0);
        return float4(mix(float3(0.15, 0.25, 0.85), float3(0.95, 0.85, 0.20), t), 1.0);
    }

    float3 acc = float3(0.0);
    float wsum = 0.0;
    float lumSum = 0.0;
    float lumSqSum = 0.0;
    for (uint d = 0u; d < K; ++d) {
        float y = 1.0 - 2.0 * (float(d) + 0.5) / float(K);
        float r = sqrt(max(1.0 - y * y, 0.0));
        float a = 2.399963229728653 * float(d);
        float3 dir = float3(cos(a) * r, y, sin(a) * r);
        uint lo = probe_data[rbase + d * 2u + 0u];
        uint hi = probe_data[rbase + d * 2u + 1u];
        half2 rg = as_type<half2>(lo);
        half2 b_ = as_type<half2>(hi);
        float3 rad = float3(float(rg.x), float(rg.y), float(b_.x));
        float lum = dot(rad, float3(0.2126, 0.7152, 0.0722));
        lumSum += lum;
        lumSqSum += lum * lum;
        float w = max(dot(N, dir), 0.0);
        if (w <= 0.0) continue;
        w = w * w;
        acc += rad * w;
        wsum += w;
    }

    // [P0] mode 3 = variance（64 方向 var/mean 角向方差热图）。
    if (in.mode > 2.5) {
        if (!valid) return float4(0.10, 0.10, 0.11, 1.0);
        float meanL = lumSum / float(K);
        float varL = max(lumSqSum / float(K) - meanL * meanL, 0.0);
        float ratio = (meanL > 1e-4) ? varL / (meanL * meanL) : 0.0;
        float t = clamp(ratio / 0.5, 0.0, 1.0);
        float3 heat = mix(float3(0.35, 0.40, 0.55), float3(0.15, 0.85, 0.30), t);
        if (inGeo) heat = mix(heat, float3(0.55, 0.08, 0.05), 0.5);
        return float4(heat, 1.0);
    }

    float3 rgb = wsum > 0.0 ? acc / wsum : float3(0.0);
    rgb = rgb / (1.0 + max(max(rgb.r, rgb.g), rgb.b));   // tone map
    if (!valid) rgb = float3(0.10, 0.10, 0.11);          // invalid：暗灰
    else if (inGeo && in.mode < 0.5) rgb = mix(rgb, float3(0.55, 0.08, 0.05), 0.5);  // inGeo=state 语义
    return float4(clamp(rgb, 0.0, 1.0), 1.0);
}
