#version 450 core
// ProbeSphereWRC.frag — [WRC] 探针球 WRC 采样入口（对齐
// EngineTest/shaders/ProbeSphere.metal 的 probe_sphere_wrc_fs）。
// probe_data 布局（WRCTypes.h）：每探针 stride = K*2+4 u32；方向 d 的
// radiance 打包在 base + d*2（packHalf2x16(rgb.rg) | packHalf2x16(rgb.b,0)）；
// base + K*2 + 2 = state（bit0 valid，bit1 inGeometry）。方向集与 trace 侧
// 同构重推（Fibonacci，固定坐标系，WRCTraceFinalize.comp 同式）。

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vClip;
layout(location = 3) flat in uint vProbe;
layout(location = 4) flat in float vState;
// [P0] 着色模式（VS push constant misc.x 直通）：
//   0 = state（现行为：radiance + invalid 灰 + inGeo 红——生命周期诊断）
//   1 = radiance（纯能量视图：只保留 invalid 暗灰标记）
//   2 = confidence（消费计数 bits[16..31] 热图：蓝→黄，64 次=饱和）
//   3 = variance（64 方向亮度 var/mean 角向方差：蓝=近单色，红=富方向）
layout(location = 5) flat in float vMode;
// [P2 SoA] probeTotal（PC misc.y 直通）——meta 区域基址定位。
layout(location = 6) flat in float vTotal;

layout(location = 0) out vec4 outColor;

layout(std430, set = 0, binding = 1) readonly buffer WRCProbeData {
    uint probe_data[];   // u32 单位，探针 stride = 64*2+4
} wrc;

void main() {
    vec3 N = normalize(vNormal);
    const uint K = 64u;                 // kWRCDirsPerProbe
    const uint radStride = K * 2u;      // kWRCProbeRadianceStrideU32 [P2 SoA]
    // [P2 SoA]: metaBase 需 probeTotal——经 PC misc.y（模块填 inputs.probe_count）。
    const uint metaBase = uint(vTotal) * radStride + uint(vProbe) * 4u;
    const uint rbase = uint(vProbe) * radStride;

    uint state = wrc.probe_data[metaBase + 2u];
    const bool valid = (state & 1u) != 0u;
    const bool inGeo = (state & 2u) != 0u;

    // [P0] confidence（mode 2）：消费计数热图——WRC 预算的需求侧读数。
    if (vMode > 1.5 && vMode < 2.5) {
        if (!valid) { outColor = vec4(0.10, 0.10, 0.11, 1.0); return; }
        const uint consume = (state >> 16u) & 0xFFFFu;
        float t = clamp(float(consume) / 64.0, 0.0, 1.0);
        vec3 heat = mix(vec3(0.15, 0.25, 0.85), vec3(0.95, 0.85, 0.20), t);
        outColor = vec4(heat, 1.0);
        return;
    }

    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    float lumSum = 0.0;
    float lumSqSum = 0.0;
    for (uint d = 0u; d < K; ++d) {
        float y = 1.0 - 2.0 * (float(d) + 0.5) / float(K);
        float r = sqrt(max(1.0 - y * y, 0.0));
        float a = 2.399963229728653 * float(d);
        vec3 dir = vec3(cos(a) * r, y, sin(a) * r);
        uint lo = wrc.probe_data[rbase + d * 2u + 0u];
        uint hi = wrc.probe_data[rbase + d * 2u + 1u];
        vec3 rad = vec3(unpackHalf2x16(lo), unpackHalf2x16(hi).x);
        float lum = dot(rad, vec3(0.2126, 0.7152, 0.0722));
        lumSum += lum;
        lumSqSum += lum * lum;
        float w = max(dot(N, dir), 0.0);
        if (w <= 0.0) continue;
        w = w * w;
        acc += rad * w;
        wsum += w;
    }

    // [P0] variance（mode 3）：角向方差 var/mean —— 各向异性健康度
    //（LumenWRCPass [WRC_ANGVAR] 的着色版）。近单色（<0.005）= 蓝灰。
    if (vMode > 2.5) {
        if (!valid) { outColor = vec4(0.10, 0.10, 0.11, 1.0); return; }
        float meanL = lumSum / float(K);
        float varL = max(lumSqSum / float(K) - meanL * meanL, 0.0);
        float ratio = (meanL > 1e-4) ? varL / (meanL * meanL) : 0.0;
        float t = clamp(ratio / 0.5, 0.0, 1.0);
        vec3 heat = mix(vec3(0.35, 0.40, 0.55), vec3(0.15, 0.85, 0.30), t);
        if (inGeo) heat = mix(heat, vec3(0.55, 0.08, 0.05), 0.5);
        outColor = vec4(heat, 1.0);
        return;
    }

    vec3 rgb = wsum > 0.0 ? acc / wsum : vec3(0.0);
    rgb = rgb / (1.0 + max(max(rgb.r, rgb.g), rgb.b));   // tone map
    if (!valid) rgb = vec3(0.10, 0.10, 0.11);            // invalid：暗灰
    else if (inGeo && vMode < 0.5) rgb = mix(rgb, vec3(0.55, 0.08, 0.05), 0.5);  // inGeo 红=state 语义
    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
