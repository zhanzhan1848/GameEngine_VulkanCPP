#version 450 core
// ProbeSphereIrradiance.frag — B1 L3.2 Vulkan 版 SH9 irradiance 采样入口
//（与 ProbeSphere.frag 同顶点输入接口；与 Metal ProbeSphere.metal 的
// probe_sphere_irradiance_fs 同数学）。模块在 Vulkan 上加载本文件编译的
// spv 构建 irradiance 管线；加载失败自动回退色相管线。
//
// 输入接口与 ProbeSphere.vert 输出对齐：
//   location 0..3 = world / normal / clip / flat probe
// SSBO: set0 binding1 = SH9 系数（probeIdx×9 float4，DDGIUpdateIrradiance 写入）。

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vClip;
layout(location = 3) flat in uint vProbe;

layout(set = 0, binding = 1, std430) readonly buffer IrradianceSSBO {
    vec4 coeffs[];
};

layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(vNormal);
    uint base = vProbe * 9u;

    const float A0 = 3.14159265;
    const float A1 = 2.09439510;
    const float A2 = 0.78539816;
    const float C0 = 0.28209479;
    const float C1 = 0.48860251;
    const float C2_0 = 1.09254843;
    const float C2_1 = 0.31539156;
    const float C2_2 = 0.54627421;

    vec3 result = vec3(0.0);
    result += coeffs[base + 0u].xyz * (A0 * C0);
    result += coeffs[base + 1u].xyz * (A1 * -C1 * N.y);
    result += coeffs[base + 2u].xyz * (A1 *  C1 * N.z);
    result += coeffs[base + 3u].xyz * (A1 * -C1 * N.x);
    result += coeffs[base + 4u].xyz * (A2 *  C2_0 * N.x * N.y);
    result += coeffs[base + 5u].xyz * (A2 * -C2_0 * N.y * N.z);
    result += coeffs[base + 6u].xyz * (A2 *  C2_1 * N.z);
    result += coeffs[base + 7u].xyz * (A2 * -C2_1 * N.x);
    result += coeffs[base + 8u].xyz * (A2 *  C2_2 * (3.0 * N.z * N.z - 1.0));

    // tone map 防过曝
    vec3 rgb = result / (1.0 + max(max(result.r, result.g), result.b));
    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
