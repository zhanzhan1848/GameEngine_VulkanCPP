/**
 * @file SSAOShader.metal
 * @brief Screen Space Directional Occlusion (SSDO) 着色器实现
 * @details 实现基于屏幕空间的方向性遮挡效果，支持深度自适应采样和方向性光照
 * @author GameEngine Team
 * @date 2025-09-04
 */

#include "Common.h"

// SSDO 配置参数 - 针对线性深度范围[0.1, 64.0]优化
#define SAMPLE_COUNT 16                 // 采样数量
#define SAMPLE_KERNEL_RADIUS 1.5f       // 基础采样半径（增大以适应新深度范围）
#define BLUR_RADIUS 0.6f                // 模糊半径
#define BLUR_FILTER_FACTORY 0.1f        // 法线相似性阈值
#define Bias 0.05f                      // 偏移值（减小以避免过度偏移）
#define AO_STRENGTH 2.0f                // AO强度（增强效果）
#define DO_STRENGTH 1.5f                // 方向性遮挡强度（增强）
#define MAX_SAMPLE_DISTANCE 2.0f        // 最大采样距离（增大）
#define MIN_SAMPLE_RADIUS 0.5f          // 最小采样半径（增大）
#define MAX_SAMPLE_RADIUS 2.0f          // 最大采样半径（增大）
#define OCCLUSION_POWER 1.8f            // 对比度增强（增强对比度）
#define DIRECTIONAL_BIAS 0.05f          // 方向性偏移（减小以增强效果）

float CompareNormal(float3 normal1, float3 normal2)
{
    return smoothstep(BLUR_FILTER_FACTORY, 1.f, dot(normal1, normal2));
}

/**
 * @brief SSDO主要计算函数
 * @param normal_depth_texture 法线深度纹理，w通道为线性深度[0.1, 64.0]
 * @param albedo_texture 反照率纹理
 * @param ssao_texture 输出SSDO纹理（RGB:方向性遮挡颜色，A:环境光遮挡）
 * @param light_params 方向光参数数组，支持多个方向光源
 * @param gid 线程组ID
 */
kernel void ssao_pass(
    texture2d<float, access::read> normal_depth_texture [[texture(0)]],
    texture2d<float, access::read> albedo_texture [[texture(1)]],
    texture2d<float, access::write> ssao_texture [[texture(2)]],
    device GlobalShaderData& global_data [[buffer(0)]],
    device DirectionalLightParameters* light_params [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]])
{
    uint2 thread_id = gid.xy;
    
    // 边界检查
    if (thread_id.x >= global_data.CameraPositionAndViewWidth.w || 
        thread_id.y >= global_data.CameraDirectionAndViewHeight.w ||
        thread_id.x < 0 ||
        thread_id.y < 0)
    {
        return;
    }

    // 读取当前像素的法线、深度和反照率信息
    float4 in_texture = normal_depth_texture.read(thread_id);
    float3 normal = normalize(in_texture.xyz * 2.0f - 1.0f);
    float linearDepth = in_texture.w; // 线性深度，范围[0.1f, 64.0f]
    
    // 读取反照率纹理用于方向性遮挡计算
    float3 albedo = albedo_texture.read(thread_id).rgb;
    
    // 计算UV坐标
    float2 uv_pos = float2(thread_id) / float2(global_data.CameraPositionAndViewWidth.w, global_data.CameraDirectionAndViewHeight.w);
    uv_pos.y = 1.f - uv_pos.y;

	float4 clips = float4(uv_pos * 2.0f - 1.0f, 1.0f, 1.0f);
    // clips.y = 1.f - clips.y;
	float4 viewRay = global_data.InvProjection * clips;
	viewRay = viewRay / viewRay.w;
	float3 viewPos = linearDepth * normalize(viewRay.xyz);

    // SSDO遮挡计算
    float occluderCount = 0.f;
    float3 indirectLighting = float3(0.0f); // 累积间接光照
    
    // 获取方向光数量
    uint numDirectionalLights = global_data.NumDirectionalLights;

    // 深度自适应采样半径计算
    // 将线性深度[0.1, 64.0]映射到采样半径[MIN_SAMPLE_RADIUS, MAX_SAMPLE_RADIUS]
    float depthFactor = (linearDepth - 0.1f) / (64.0f - 0.1f); // 归一化到[0,1]
    float adaptiveRadius = mix(MIN_SAMPLE_RADIUS, MAX_SAMPLE_RADIUS, depthFactor);
    
    // SSDO采样循环
    for(int i = 0; i < SAMPLE_COUNT; ++i)
    {
        // 生成半球采样向量
        float2 sampleSeed = float2(thread_id) + float2(i);
        float3 randomVector3D = sampleHemisphere(normal, sampleSeed, i);
        
        // 在view space中计算采样位置，使用自适应半径
        float3 sampleViewPos = viewPos + randomVector3D * adaptiveRadius;

        float4 rclipPos = global_data.Projection * float4(sampleViewPos, 1.0f);
		float2 rscreenPos = (rclipPos.xy / rclipPos.w) * 0.5f + 0.5f;
        rscreenPos.y = 1.f - rscreenPos.y;

        // 边界检查
        if (any(rscreenPos < 0.0f) || any(rscreenPos > 1.0f)) {
            continue;
        }

        rscreenPos = float2(rscreenPos.x * global_data.CameraPositionAndViewWidth.w, rscreenPos.y * global_data.CameraDirectionAndViewHeight.w);
        rscreenPos.y = 1.f - rscreenPos.y;
        
        // 读取采样点的深度和法线信息
        float4 sampleTexture = normal_depth_texture.read(uint2(rscreenPos));
        float sampleDepth = sampleTexture.w;
        float3 sampleNormal = normalize(sampleTexture.xyz * 2.0f - 1.0f);
        
        // 改进的深度比较逻辑
        float depthDiff = sampleDepth - linearDepth;
        
        // 自适应偏移值，基于深度距离
        float adaptiveBias = Bias * (1.0f + linearDepth * 0.01f);
        
        // 距离范围检查：避免采样过远的点
        float sampleDistance = length(randomVector3D * adaptiveRadius);
        bool withinRange = sampleDistance <= MAX_SAMPLE_DISTANCE;
        
        // 遮挡判断：深度差异合理且在采样范围内
        bool isOccluder = (linearDepth + adaptiveBias <= sampleDepth) && 
                         (depthDiff < MAX_SAMPLE_DISTANCE) && withinRange;
        
        if (isOccluder) 
        {
            // 基于深度差异的遮挡权重
            float occlusionWeight = 1.0f - smoothstep(0.0f, MAX_SAMPLE_DISTANCE, depthDiff);
            occluderCount += occlusionWeight;
            
            // SSDO: 计算方向性间接光照
            // 计算从当前点到采样点的方向
            float3 sampleDirection = normalize(randomVector3D);
            
            // 距离衰减（使用正确的自适应半径）
            float distance = sampleDistance;
            float attenuation = (1.0f / (1.0f + distance * distance)) * occlusionWeight;
            
            // 读取采样点的反照率
            float3 sampleAlbedo = albedo_texture.read(uint2(rscreenPos)).rgb;
            
            // 遍历所有方向光源计算间接光照贡献
            float3 totalIndirectContribution = float3(0.0f);
            for (uint lightIndex = 0; lightIndex < numDirectionalLights; ++lightIndex) 
            {
                // 获取当前方向光信息
                float3 lightDir = normalize(-light_params[lightIndex].DirectionAndIntensity.xyz);
                float3 lightColor = light_params[lightIndex].Color.rgb;
                float lightIntensity = light_params[lightIndex].DirectionAndIntensity.w * 0.1f;
                
                // 计算光源对采样表面的照明强度
                float sampleLightDot = max(0.0f, dot(sampleNormal, lightDir));
                
                // 计算方向性因子：采样方向与光源方向的关系
                float directionalFactor = max(0.0f, dot(sampleDirection, lightDir));
                
                // 计算当前光源的间接光照贡献
                float3 lightContribution = sampleAlbedo * lightColor * lightIntensity * 
                                          sampleLightDot * directionalFactor * attenuation;
                
                // 应用方向性偏移，避免过度明亮
                lightContribution *= (1.0f - DIRECTIONAL_BIAS);
                
                totalIndirectContribution += lightContribution;
            }
            
            // 对多光源贡献进行归一化，避免过度明亮
            if (numDirectionalLights > 1) 
            {
                totalIndirectContribution /= float(numDirectionalLights);
            }
            
            indirectLighting += totalIndirectContribution;
        }
    }
    
    // 计算SSDO遮挡因子（现在occluderCount是加权和）
    float occlusion = clamp(occluderCount / float(SAMPLE_COUNT), 0.0f, 1.0f);
    
    // 应用遮挡强度曲线增强对比度
    occlusion = pow(occlusion, OCCLUSION_POWER);
    
    // 计算最终的环境光遮挡因子
    float ao = 1.0f - occlusion;
    ao = clamp(pow(ao, AO_STRENGTH), 0.0f, 1.0f);
    
    // 归一化间接光照
    indirectLighting /= float(SAMPLE_COUNT);
    
    // 应用方向性遮挡强度
    indirectLighting *= DO_STRENGTH;
    
    // 确保间接光照不会过度明亮
    indirectLighting = clamp(indirectLighting, 0.0f, 1.0f);
    
    // 计算最终的方向性遮挡颜色
    // RGB通道：方向性间接光照 + 基础反照率 * 环境光遮挡
    // A通道：环境光遮挡因子
    float3 finalColor = indirectLighting; // + albedo * ao * 0.1f; // 0.1f为环境光强度
    
    // 输出SSDO结果：RGB为方向性遮挡颜色，A为环境光遮挡
    ssao_texture.write(float4(finalColor, ao), thread_id);
}

/**
 * @brief SSDO模糊函数
 * @param ssao_texture 输入SSDO纹理（RGB:方向性遮挡颜色，A:环境光遮挡）
 * @param ssao_blur_texture 输出模糊后的SSDO纹理
 * @param normal_depth_texture 法线深度纹理，用于边缘保持
 * @param global_data 全局着色器数据
 * @param gid 线程组ID
 */
kernel void ssao_blur(
    texture2d<float, access::read> ssao_texture [[texture(0)]],
    texture2d<float, access::write> ssao_blur_texture [[texture(1)]],
    texture2d<float, access::read> normal_depth_texture [[texture(2)]],
    device GlobalShaderData& global_data [[buffer(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    uint2 thread_id = gid.xy;
    uint width = global_data.CameraPositionAndViewWidth.w;
    uint height = global_data.CameraDirectionAndViewHeight.w;
    
    // 边界检查：uint类型天然非负，只需检查上界
    if (thread_id.x >= width || thread_id.y >= height)
    {
        return;
    }

    int blur_radius = int(BLUR_RADIUS);
    
    // 修正UV坐标计算：直接使用像素坐标，不需要除法
    uint2 uv = thread_id.xy;
    
    // 使用int类型进行偏移计算，避免uint下溢
    int2 base_coord = int2(thread_id.xy);
    
    // 计算采样坐标，并进行边界检查
    int2 coord0a = base_coord - int2(blur_radius, blur_radius);
    int2 coord0b = base_coord + int2(blur_radius, blur_radius);
    int2 coord1a = base_coord - int2(2 * blur_radius, 2 * blur_radius);
    int2 coord1b = base_coord + int2(2 * blur_radius, 2 * blur_radius);
    int2 coord2a = base_coord - int2(3 * blur_radius, 3 * blur_radius);
    int2 coord2b = base_coord + int2(3 * blur_radius, 3 * blur_radius);
    
    // 边界裁剪：确保所有坐标都在有效范围内
    uint2 uv0a = uint2(clamp(coord0a, int2(0), int2(width-1, height-1)));
    uint2 uv0b = uint2(clamp(coord0b, int2(0), int2(width-1, height-1)));
    uint2 uv1a = uint2(clamp(coord1a, int2(0), int2(width-1, height-1)));
    uint2 uv1b = uint2(clamp(coord1b, int2(0), int2(width-1, height-1)));
    uint2 uv2a = uint2(clamp(coord2a, int2(0), int2(width-1, height-1)));
    uint2 uv2b = uint2(clamp(coord2b, int2(0), int2(width-1, height-1)));

    float3 normal = normal_depth_texture.read(uint2(uv)).xyz;
    float3 normal0a = normal_depth_texture.read(uint2(uv0a)).xyz;
    float3 normal0b = normal_depth_texture.read(uint2(uv0b)).xyz;
    float3 normal1a = normal_depth_texture.read(uint2(uv1a)).xyz;
    float3 normal1b = normal_depth_texture.read(uint2(uv1b)).xyz;
    float3 normal2a = normal_depth_texture.read(uint2(uv2a)).xyz;
    float3 normal2b = normal_depth_texture.read(uint2(uv2b)).xyz;

    float4 col = ssao_texture.read(uint2(uv));
    float4 col0a = ssao_texture.read(uint2(uv0a));
    float4 col0b = ssao_texture.read(uint2(uv0b));
    float4 col1a = ssao_texture.read(uint2(uv1a));
    float4 col1b = ssao_texture.read(uint2(uv1b));
    float4 col2a = ssao_texture.read(uint2(uv2a));
    float4 col2b = ssao_texture.read(uint2(uv2b));

    float w = 0.37004405286;
    float w0a = CompareNormal(normal, normal0a) * 0.31718061674;
    float w0b = CompareNormal(normal, normal0b) * 0.31718061674;
    float w1a = CompareNormal(normal, normal1a) * 0.19823788546;
    float w1b = CompareNormal(normal, normal1b) * 0.19823788546;
    float w2a = CompareNormal(normal, normal2a) * 0.11453744493;
    float w2b = CompareNormal(normal, normal2b) * 0.11453744493;

    float4 result;
    result = w * col;
    result += w0a * col0a;
    result += w0b * col0b;
    result += w1a * col1a;
    result += w1b * col1b;
    result += w2a * col2a;
    result += w2b * col2b;

    result /= w + w0a + w0b + w1a + w1b + w2a + w2b;
    ssao_blur_texture.write(result, thread_id);
}
// /**
//  * @brief 兼容性SSAO函数，从SSDO结果中提取环境光遮挡
//  * @param normal_depth_texture 法线深度纹理
//  * @param albedo_texture 反照率纹理
//  * @param ssao_texture 输出SSAO纹理（仅环境光遮挡）
//  * @param global_data 全局着色器数据
//  * @param light_params 光照参数
//  * @param gid 线程组ID
//  */
// kernel void ssao_pass(
//     texture2d<float, access::read> normal_depth_texture [[texture(0)]],
//     texture2d<float, access::read> albedo_texture [[texture(1)]],
//     texture2d<float, access::write> ssao_texture [[texture(2)]],
//     device GlobalShaderData& global_data [[buffer(0)]],
//     device DirectionalLightParameters* light_params [[buffer(1)]],
//     uint2 gid [[thread_position_in_grid]])
// {
//     uint2 thread_id = gid.xy;
    
//     // 边界检查
//     if (thread_id.x >= global_data.CameraPositionAndViewWidth.w || 
//         thread_id.y >= global_data.CameraDirectionAndViewHeight.w ||
//         thread_id.x < 0 ||
//         thread_id.y < 0)
//     {
//         return;
//     }

//     // 读取当前像素的法线和深度信息
//     float4 in_texture = normal_depth_texture.read(thread_id);
//     float3 normal = normalize(in_texture.xyz);
//     float linearDepth = in_texture.w;
    
//     // 计算UV坐标
//     float2 uv_pos = float2(thread_id) / float2(global_data.CameraPositionAndViewWidth.w, global_data.CameraDirectionAndViewHeight.w);

// 	float4 clips = float4(uv_pos * 2.0f - 1.0f, 1.0f, 1.0f);
//     clips.y = -clips.y;
// 	float4 viewRay = global_data.InvProjection * clips;
// 	viewRay = viewRay / viewRay.w;
// 	float3 viewPos = linearDepth * normalize(viewRay.xyz);

//     // 简化的SSAO计算
//     float occluderCount = 0.f;

//     for(int i = 0; i < SAMPLE_COUNT; ++i)
//     {
//         float2 sampleSeed = float2(thread_id) + float2(i);
//         float3 randomVector3D = sampleHemisphere(normal, sampleSeed, i);
        
//         float3 sampleViewPos = viewPos + randomVector3D * SAMPLE_KERNEL_RADIUS;

//         float4 rclipPos = global_data.Projection * float4(sampleViewPos, 1.0f);
// 		float2 rscreenPos = (rclipPos.xy / rclipPos.w) * 0.5f + 0.5f;
//         rscreenPos.y = -rscreenPos.y;
//         rscreenPos = float2(rscreenPos.x * global_data.CameraPositionAndViewWidth.w, rscreenPos.y * global_data.CameraDirectionAndViewHeight.w);
        
//         rscreenPos = min(rscreenPos, float2(global_data.CameraPositionAndViewWidth.w, global_data.CameraDirectionAndViewHeight.w));
//         rscreenPos = max(rscreenPos, float2(0, 0));
        
//         float sampleDepth = normal_depth_texture.read(uint2(rscreenPos)).w;
//         occluderCount += (linearDepth + Bias <= sampleDepth ? 1.f : 0.f);
//     }
    
//     float occlusion = float(occluderCount) / float(SAMPLE_COUNT);
//     occlusion = pow(clamp(occlusion, 0.0f, 1.0f), OCCLUSION_POWER);
//     float ao = 1.0f - occlusion;
//     ao = pow(ao, AO_STRENGTH);
    
//     ssao_texture.write(float4(ao, ao, ao, ao), thread_id);
// }

// /**
//  * @brief 兼容性SSAO模糊函数
//  */
// kernel void ssao_blur(
//     texture2d<float, access::read> ssao_texture [[texture(0)]],
//     texture2d<float, access::write> ssao_blur_texture [[texture(1)]],
//     texture2d<float, access::read> normal_depth_texture [[texture(2)]],
//     device GlobalShaderData& global_data [[buffer(0)]],
//     uint2 gid [[thread_position_in_grid]])
// {
//     uint2 thread_id = gid.xy;
//     uint width = global_data.CameraPositionAndViewWidth.w;
//     uint height = global_data.CameraDirectionAndViewHeight.w;
    
//     if (thread_id.x >= width || thread_id.y >= height)
//     {
//         return;
//     }

//     int blur_radius = int(BLUR_RADIUS);
//     uint2 uv = thread_id.xy;
//     int2 base_coord = int2(thread_id.xy);
    
//     int2 coord0a = base_coord - int2(blur_radius, blur_radius);
//     int2 coord0b = base_coord + int2(blur_radius, blur_radius);
//     int2 coord1a = base_coord - int2(2 * blur_radius, 2 * blur_radius);
//     int2 coord1b = base_coord + int2(2 * blur_radius, 2 * blur_radius);
//     int2 coord2a = base_coord - int2(3 * blur_radius, 3 * blur_radius);
//     int2 coord2b = base_coord + int2(3 * blur_radius, 3 * blur_radius);
    
//     uint2 uv0a = uint2(clamp(coord0a, int2(0), int2(width-1, height-1)));
//     uint2 uv0b = uint2(clamp(coord0b, int2(0), int2(width-1, height-1)));
//     uint2 uv1a = uint2(clamp(coord1a, int2(0), int2(width-1, height-1)));
//     uint2 uv1b = uint2(clamp(coord1b, int2(0), int2(width-1, height-1)));
//     uint2 uv2a = uint2(clamp(coord2a, int2(0), int2(width-1, height-1)));
//     uint2 uv2b = uint2(clamp(coord2b, int2(0), int2(width-1, height-1)));

//     float3 normal = normal_depth_texture.read(uint2(uv)).xyz;
//     float3 normal0a = normal_depth_texture.read(uint2(uv0a)).xyz;
//     float3 normal0b = normal_depth_texture.read(uint2(uv0b)).xyz;
//     float3 normal1a = normal_depth_texture.read(uint2(uv1a)).xyz;
//     float3 normal1b = normal_depth_texture.read(uint2(uv1b)).xyz;
//     float3 normal2a = normal_depth_texture.read(uint2(uv2a)).xyz;
//     float3 normal2b = normal_depth_texture.read(uint2(uv2b)).xyz;

//     float4 col = ssao_texture.read(uint2(uv));
//     float4 col0a = ssao_texture.read(uint2(uv0a));
//     float4 col0b = ssao_texture.read(uint2(uv0b));
//     float4 col1a = ssao_texture.read(uint2(uv1a));
//     float4 col1b = ssao_texture.read(uint2(uv1b));
//     float4 col2a = ssao_texture.read(uint2(uv2a));
//     float4 col2b = ssao_texture.read(uint2(uv2b));

//     float w = 0.37004405286;
//     float w0a = CompareNormal(normal, normal0a) * 0.31718061674;
//     float w0b = CompareNormal(normal, normal0b) * 0.31718061674;
//     float w1a = CompareNormal(normal, normal1a) * 0.19823788546;
//     float w1b = CompareNormal(normal, normal1b) * 0.19823788546;
//     float w2a = CompareNormal(normal, normal2a) * 0.11453744493;
//     float w2b = CompareNormal(normal, normal2b) * 0.11453744493;

//     float4 result;
//     result = w * col;
//     result += w0a * col0a;
//     result += w0b * col0b;
//     result += w1a * col1a;
//     result += w1b * col1b;
//     result += w2a * col2a;
//     result += w2b * col2b;

//     result /= w + w0a + w0b + w1a + w1b + w2a + w2b;
//     ssao_blur_texture.write(result, thread_id);
// }
