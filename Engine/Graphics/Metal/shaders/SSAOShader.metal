/**
 * @file SSAOShader.metal
 * @brief Screen Space Ambient Occlusion (SSAO) 着色器实现
 * @details 实现基于屏幕空间的环境光遮挡效果，支持深度自适应采样
 * @author GameEngine Team
 * @date 2024
 */

#include "Common.h"

// SSAO 配置参数
#define SAMPLE_COUNT 16                 // 采样数量
#define SAMPLE_KERNEL_RADIUS 0.2f       // 基础采样半径
#define BLUR_RADIUS 1.2f                // 模糊半径
#define BLUR_FILTER_FACTORY 0.3f        // 法线相似性阈值
#define Bias 0.0001f                     // 偏移值以减少自遮挡
#define AO_STRENGTH 4.0f               // AO强度
#define MAX_SAMPLE_DISTANCE 2.0f       // 最大采样距离
#define MIN_SAMPLE_RADIUS 0.2f         // 最小采样半径
#define MAX_SAMPLE_RADIUS 2.0f         // 最大采样半径
#define OCCLUSION_POWER 1.2f           // 对比度增强

float CompareNormal(float3 normal1, float3 normal2)
{
    return smoothstep(BLUR_FILTER_FACTORY, 1.f, dot(normal1, normal2));
}

/**
 * @brief SSAO主要计算函数
 * @param normal_depth_texture 法线深度纹理，w通道为线性深度[0.1, 64.0]
 * @param albedo_texture 反照率纹理（未使用）
 * @param ssao_texture 输出SSAO纹理
 * @param global_data 全局着色器数据
 * @param light_params 光照参数（未使用）
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

    // 读取当前像素的法线和深度信息
    float4 in_texture = normal_depth_texture.read(thread_id);
    float3 normal = normalize(in_texture.xyz);
    float linearDepth = in_texture.w; // 线性深度，范围[0.1f, 64.0f]
    
    // 计算UV坐标
    float2 uv_pos = float2(thread_id) / float2(global_data.CameraPositionAndViewWidth.w, global_data.CameraDirectionAndViewHeight.w);

	float4 clips = float4(uv_pos * 2.0f - 1.0f, 1.0f, 1.0f);
    clips.y = -clips.y;
	float4 viewRay = global_data.InvProjection * clips;
	viewRay = viewRay / viewRay.w;
	float3 viewPos = linearDepth * normalize(viewRay.xyz);

    // SSAO遮挡计算
    float occluderCount = 0.f;

    // SSAO采样循环
    for(int i = 0; i < SAMPLE_COUNT; ++i)
    {
        // 生成半球采样向量
        float2 sampleSeed = float2(thread_id) + float2(i);
        float3 randomVector3D = sampleHemisphere(normal, sampleSeed, i);
        
        // 在view space中计算采样位置
        float3 sampleViewPos = viewPos + randomVector3D * SAMPLE_KERNEL_RADIUS;

        float4 rclipPos = global_data.Projection * float4(sampleViewPos, 1.0f);
		float2 rscreenPos = (rclipPos.xy / rclipPos.w) * 0.5f + 0.5f;
        rscreenPos.y = -rscreenPos.y;
        rscreenPos = float2(rscreenPos.x * global_data.CameraPositionAndViewWidth.w, rscreenPos.y * global_data.CameraDirectionAndViewHeight.w);
        
        // 边界检查：确保采样坐标在有效范围内
        rscreenPos = min(rscreenPos, float2(global_data.CameraPositionAndViewWidth.w, global_data.CameraDirectionAndViewHeight.w));
        rscreenPos = max(rscreenPos, float2(0, 0));
        
        float sampleDepth = normal_depth_texture.read(uint2(rscreenPos)).w;
        
        float rangeCheck = smoothstep(0.0f, 1.0f, SAMPLE_KERNEL_RADIUS / abs(linearDepth - sampleDepth));
        occluderCount += (linearDepth + Bias <=  sampleDepth ? 1.f : 0.f);
    }
    
    // 计算SSAO遮挡因子
    float occlusion = float(occluderCount) / float(SAMPLE_COUNT);
    
    // 应用遮挡强度曲线增强对比度
    occlusion = pow(clamp(occlusion, 0.0f, 1.0f), OCCLUSION_POWER);
    
    // 计算最终的环境光遮挡因子
    float ao = 1.0f - occlusion;
    ao = pow(ao, AO_STRENGTH);
    
    // 输出四个通道都是AO值
    ssao_texture.write(float4(ao, ao, ao, ao), thread_id);
}

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