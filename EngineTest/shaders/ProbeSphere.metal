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
};

struct ProbeSpherePC {
    float4x4 view_proj;
    float4   sun_dir;
    float4   cam_pos;
};

struct ProbeSphereVSOut {
    float4 clip   [[position]];
    float3 world;
    float3 nrm;
    float  probe;
};

vertex ProbeSphereVSOut probe_sphere_vs(
    ProbeSphereVSIn in [[stage_in]],
    constant ProbeSpherePC& pc [[buffer(2)]])
{
    ProbeSphereVSOut o;
    o.world = in.pos;
    o.nrm   = in.nrm;
    o.probe = in.probe;

    float4 clip = pc.view_proj * float4(in.pos, 1.0);
    if (distance(in.pos, pc.cam_pos.xyz) < 2.0) {
        clip = float4(2.0, 2.0, 2.0, 1.0);  // 裁剪域外
    }
    o.clip = clip;
    return o;
}

fragment float4 probe_sphere_fs(ProbeSphereVSOut in [[stage_in]])
{
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
