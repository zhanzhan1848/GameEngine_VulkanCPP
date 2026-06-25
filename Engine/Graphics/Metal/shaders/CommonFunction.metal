#pragma once
#ifndef COMMON_FUNCTION_METAL
#define COMMON_FUNCTION_METAL

// Basic
float length2(float3 v) {
    return dot(v, v);  // 返回向量的长度平方
}

float dot2(float2 v) {
    return dot(v, v);  // 使用标准dot函数
}

// dot2 for float3
float dot2(float3 v) {
    return dot(v, v);  // 使用标准dot函数
}

// ndot for float2
float ndot(float2 a, float2 b) {
    return a.x * b.x - a.y * b.y;  // 无内置替代，直接实现
}

float2 OPU( float2 d1, float2 d2 )
{
	return (d1.x < d2.x) ? d1 : d2;
}

// 通用 hash 随机数 [0,1]
float hash(float2 p)
{
    return fract(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

// 生成 3D 随机向量
float3 rand3(float2 seed, float index) {
    return normalize(float3(
        hash(seed + index * 0.11),
        hash(seed + index * 0.37),
        hash(seed + index * 0.73)
    ));
}

/**
 * @brief 将NDC深度转换为线性深度
 * @param ndcDepth NDC空间深度值 [0,1] (Metal) 或 [-1,1] (其他API)
 * @param nearPlane 近裁剪面距离
 * @param farPlane 远裁剪面距离
 * @return 线性深度值
 */
float NDCDepthToLinear(float ndcDepth, float nearPlane, float farPlane)
{
    // Metal使用[0,1]深度范围，转换为线性深度
    return (2.0 * nearPlane * farPlane) / (farPlane + nearPlane - ndcDepth * (farPlane - nearPlane));
}

/**
 * @brief 将线性深度转换为NDC深度
 * @param linearDepth 线性深度值
 * @param nearPlane 近裁剪面距离
 * @param farPlane 远裁剪面距离
 * @return NDC空间深度值
 */
float LinearDepthToNDC(float linearDepth, float nearPlane, float farPlane)
{
    return (farPlane + nearPlane - (2.0 * nearPlane * farPlane) / linearDepth) / (farPlane - nearPlane);
}

// 半球采样（Cosine-weighted），随机投影法生成 TBN
// float3 sampleHemisphere(float3 N, float2 seed, int sampleIndex) 
// {
//     float xi1 = hash(seed + float(sampleIndex) * 0.37);
//     float xi2 = hash(seed + float(sampleIndex) * 0.73);

//     // Cosine-weighted hemisphere sampling
//     float phi = 2.0 * PI * xi1;
//     float r = sqrt(xi2);
//     float x = (r * cos(phi)) * 2.f - 1.f;
//     float y = (r * sin(phi)) * 2.f - 1.f;
//     float z = sqrt(1.0 - xi2);

//     // ==== 随机投影法创建切线基 ====
//     float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);sampleIndex));
//     float3 tangent = normalize(cross(up, N));- N * dot(randVec, N));
//     float3 bitangent = cross(N, tangent); // 已经是单位向量
//     float3x3 TBN     = float3x3(tangent, bitangent, N);

//     return normalize(TBN * float3(x, y, z));
// }

// SH9 Irradiance Evaluation
template<typename T>
float3 EvalSH9Irradiance(float3 N, const T sh_coeffs)
{
    // Constants
    const float A0 = 3.14159265;
    const float A1 = 2.09439510;
    const float A2 = 0.78539816;

    const float C0 = 0.28209479;
    const float C1 = 0.48860251;
    const float C2_0 = 1.09254843;
    const float C2_1 = 0.31539156;
    const float C2_2 = 0.54627421;

    float3 result = float3(0.0);

    // L0
    result += sh_coeffs[0].xyz * (A0 * C0);

    // L1
    result += sh_coeffs[1].xyz * (A1 * -C1 * N.y);
    result += sh_coeffs[2].xyz * (A1 * C1 * N.z);
    result += sh_coeffs[3].xyz * (A1 * -C1 * N.x);

    // L2
    result += sh_coeffs[4].xyz * (A2 * C2_0 * N.x * N.y);
    result += sh_coeffs[5].xyz * (A2 * -C2_0 * N.y * N.z);
    result += sh_coeffs[6].xyz * (A2 * C2_1 * (3.0 * N.z * N.z - 1.0));
    result += sh_coeffs[7].xyz * (A2 * -C2_0 * N.x * N.z);
    result += sh_coeffs[8].xyz * (A2 * C2_2 * (N.x * N.x - N.y * N.y));

    return max(result, float3(0.0));
}

/**
 * @brief 改进的半球采样函数，使用Hammersley序列生成更均匀的随机分布
 * @param N 表面法线
 * @param seed 随机种子
 * @param sampleIndex 采样索引
 * @return 半球空间中的随机方向向量
 */
float3 sampleHemisphere(float3 N, float2 seed, int sampleIndex) 
{
    // 使用Hammersley序列获得更均匀的分布
    float xi1 = float(sampleIndex) / 64.0f; // 假设最大64个样本
    
    // Van der Corput序列用于第二个维度
    uint bits = uint(sampleIndex);
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    float xi2 = float(bits) * 2.3283064365386963e-10f; // / 0x100000000
    
    // 添加随机扰动避免规律性
    float noise1 = hash(seed + float2(sampleIndex * 0.1f, sampleIndex * 0.2f));
    float noise2 = hash(seed + float2(sampleIndex * 0.3f, sampleIndex * 0.4f));
    xi1 = fract(xi1 + noise1 * 0.1f);
    xi2 = fract(xi2 + noise2 * 0.1f);

    // 使用uniform hemisphere sampling而不是cosine-weighted
    float phi = 2.0f * 3.1415926535897932384626433832795f * xi1;
    float cosTheta = xi2; // uniform分布在[0,1]
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));

    // 在切线空间中构建采样向量
    float x = sinTheta * cos(phi);
    float y = sinTheta * sin(phi);
    float z = cosTheta;

    // 构建更稳定的切线基，避免退化情况
    float3 up = abs(N.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent); // 已经是单位向量
    
    // 构建TBN矩阵并变换到世界空间
    float3x3 TBN = float3x3(tangent, N, bitangent);
    float3 result = TBN * float3(x, y, z);
    
    // 确保结果在正确的半球
    if (dot(result, N) < 0.0f) {
        result = -result;
    }
    
    return normalize(result);
}

/**
 * @brief 将屏幕空间UV和线性深度重建为世界坐标
 * @param pixelUv 屏幕空间UV坐标 [0,1]
 * @param linearDepth 线性深度值
 * @param gd 全局着色器数据，包含相机变换矩阵
 * @return 世界空间坐标
 */
float3 ReconstructWorldPos(float2 pixelUv, float linearDepth, device GlobalShaderData& gd)
{
    // 将屏幕空间UV转换为NDC坐标 [-1,1]
    float2 ndc = pixelUv * 2.0 - 1.0;
    
    // 构建NDC空间的齐次坐标
    // 在Metal中，NDC深度范围是[0,1]，这里使用线性深度作为NDC深度
    float4 ndcPos = float4(ndc.x, ndc.y, 1.0, 1.0);
    
    float4 viewRayH = float4(
        dot(gd.InvProjection[0], ndcPos),
        dot(gd.InvProjection[1], ndcPos),
        dot(gd.InvProjection[2], ndcPos),
        dot(gd.InvProjection[3], ndcPos)
    );

    // 除掉齐次分量，得到方向
    float3 viewRay = viewRayH.xyz / viewRayH.w;
    viewRay = normalize(viewRay);

    // linearDepth 就是沿着 viewRay 的长度
    return viewRay * linearDepth;
}

/**
 * @brief 将屏幕空间UV和线性深度重建为view space坐标（修正Metal左手坐标系）
 * @param pixelUv 屏幕空间UV坐标 [0,1]
 * @param linearDepth 线性深度值 [0.1, 64.0]
 * @param gd 全局着色器数据
 * @return view space坐标
 */
float3 ReconstructViewPos(float2 pixelUv, float linearDepth, device GlobalShaderData& gd)
{
    // 将屏幕空间UV转换为NDC坐标 [-1,1]
    float2 ndc = pixelUv * 2.0 - 1.0;
    
    // Metal使用左手坐标系，Y轴需要翻转
    ndc.y = -ndc.y;
    
    // 构建NDC空间的齐次坐标，使用远平面深度值进行重建
    float4 ndcPos = float4(ndc.x, ndc.y, 1.0, 1.0);
    
    // 通过逆投影矩阵获得view space方向
    float4 viewRayH = gd.InvProjection * ndcPos;

    // 透视除法，得到view space方向向量
    float3 viewRay = viewRayH.xyz / viewRayH.w;
    
    // 在Metal左手坐标系中，相机看向+Z方向
    // view space中正Z值表示距离相机的深度
    float rayLength = length(viewRay);
    if (rayLength > 1e-6) {
        viewRay = normalize(viewRay);
        // 直接使用线性深度缩放view ray
        // 在Metal左手坐标系中，保持Z值为正值（表示距离相机的深度）
        float3 viewPos = viewRay * linearDepth;
        // 确保Z值为正值，符合Metal左手坐标系约定
        viewPos.z = abs(viewPos.z);
        return viewPos;
    } else {
        // 处理退化情况，Z值为负表示深度
        return float3(0.0, 0.0, -linearDepth);
    }
}

// SDF


// PBR
float DistributionGGX(float3 N, float3 H, float a)
{
	float a2 = a * a * a * a;
	float NdotH = max(dot(N, H), 0.0);
	float NdotH2 = NdotH * NdotH;

	float nom = a2;
	float denom = (NdotH2 * (a2 - 1.0) + 1.0);
	denom = 3.1415926535897932384626433832795 * denom * denom;

	return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float k)
{
	float r = k + 1.0;
	float a = (r * r) / 8.0;

	float nom = NdotV;
	float denom = NdotV * (1.0 - k) + k;

	return nom / denom;
}

float GeometrySmith(float3 N, float3 V, float3 L, float k)
{
	float NdotV = max(dot(N, V), 0.0);
	float NdotL = max(dot(N, L), 0.0);
	float ggx1 = GeometrySchlickGGX(NdotV, k);
	float ggx2 = GeometrySchlickGGX(NdotL, k);

	return ggx1 * ggx2;
}

float3 fresnelSchlick(float cosTheta, float3 F0)
{
	return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}


// 色彩空间转换
float3 srgb_to_acescg(float3 col) {
    auto mat = float3x3(0.61319, 0.33951, 0.04737,
                        0.07021, 0.91634, 0.01345,
                        0.02062, 0.10957, 0.86961);
    return mat * col;
}

float3 acescg_to_srgb(float3 col) {
    auto mat = float3x3(1.70505, -0.62179, -0.08326,
                        -0.13026, 1.14080, -0.01055,
                        -0.02400, -0.12897, 1.15297);
    return mat * col;
}

float3 PhongBRDF(float3 N, float3 L, float3 V, float3 diffuseColor, float3 specularColor, float shininess)
{
	float3 color = diffuseColor;
	const float3 R = reflect(-L, N);
	const float VoR = max(dot(V, R), 0.f);
	color += pow(VoR, max(shininess, 1.f)) * specularColor;

	return color;
}

float3 CalculateLighting(Surface S, float3 L, float3 V, float3 lightColor)
{
    const float NoL = clamp(dot(S.Normal, L), 0.f, 1.f);
    // 确保PI不为零，避免除零错误
    const float invPI = 1.0f / max(PI, 1e-6f);
	return PhongBRDF(S.Normal, L, V, S.BaseColor, 1.f, (1 - S.PerceptualRoughness) * 100.f) * (NoL * invPI) * lightColor;
}

#endif // COMMON_FUNCTION_METAL