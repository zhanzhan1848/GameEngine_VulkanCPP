/**
 * @file SSGIShader.metal
 * @brief 屏幕空间全局光照(SSGI)着色器实现
 * @author GameEngine Team
 * @date 2024
 * 
 * 实现功能:
 * - 多次光照反弹计算
 * - 时间累积降噪
 * - 空间滤波
 * - 可配置的质量级别
 */

#include "Common.h"

// SSGI配置常量 - 可通过环境变量控制
#ifndef SSGI_SAMPLE_COUNT
#define SSGI_SAMPLE_COUNT 32        // 每像素采样数量
#endif

#ifndef SSGI_MAX_BOUNCES
#define SSGI_MAX_BOUNCES 1          // 最大反弹次数
#endif

#ifndef SSGI_ENABLE_TEMPORAL
#define SSGI_ENABLE_TEMPORAL 0      // 启用时间累积
#endif

#ifndef SSGI_ENABLE_SPATIAL_FILTER
#define SSGI_ENABLE_SPATIAL_FILTER 1 // 启用空间滤波
#endif

#ifndef SSGI_ENABLE_MULTI_BOUNCE
#define SSGI_ENABLE_MULTI_BOUNCE 1  // 启用多次反弹
#endif

#ifndef SSGI_QUALITY_LEVEL
#define SSGI_QUALITY_LEVEL 2        // 质量级别 0=低, 1=中, 2=高
#endif

// SSGI参数
constant float SSGI_RADIUS = 2.0f;              // 采样半径
constant float SSGI_INTENSITY = 1.0f;           // 全局光照强度
constant float SSGI_BIAS = 0.5f;               // 深度偏移
constant float SSGI_THICKNESS = 0.25f;           // 表面厚度
constant float SSGI_TEMPORAL_WEIGHT = 0.95f;    // 时间累积权重
constant float SSGI_DISTANCE_FALLOFF = 0.1f;    // 距离衰减
constant float SSGI_NORMAL_WEIGHT = 0.8f;       // 法线权重

/**
 * @brief SSGI主计算内核
 * @param tid 线程ID
 * @param normal_depth_texture 法线深度纹理 (RGB=法线, A=线性深度)
 * @param albedo_texture 反照率纹理
 * @param gpass_texture G-Buffer纹理
 * @param ssgi_output 输出纹理 (RGB=间接光照, A=置信度)
 * @param previous_ssgi 上一帧SSGI结果 (用于时间累积)
 * @param motion_vectors 运动向量纹理 (用于时间重投影)
 * @param gd 全局着色器数据
 * @param directional_lights 方向光数据
 */
kernel void ssgi_pass(
    uint2 tid [[thread_position_in_grid]],
    texture2d<float, access::read> normal_depth_texture [[texture(0)]],
    texture2d<float, access::read> albedo_texture [[texture(1)]],
    texture2d<float, access::read> gpass_texture [[texture(2)]],
    texture2d<float, access::write> ssgi_output [[texture(3)]],
#if SSGI_ENABLE_TEMPORAL
    texture2d<float, access::read> previous_ssgi [[texture(4)]],
    texture2d<float, access::read> motion_vectors [[texture(5)]],
#endif
    device GlobalShaderData& gd [[buffer(0)]],
    device DirectionalLightParameters* directional_lights [[buffer(1)]]
) {
    // 边界检查
    if (tid.x >= gd.CameraPositionAndViewWidth.w || tid.y >= gd.CameraDirectionAndViewHeight.w) {
        return;
    }
    
    // 计算UV坐标
    float2 texel_size = 1.0f / float2(gd.CameraPositionAndViewWidth.w, gd.CameraDirectionAndViewHeight.w);
    float2 uv = (float2(tid) + 0.5f) * texel_size;
    
    // 读取G-Buffer数据
    float4 normal_depth = normal_depth_texture.read(tid);
    float4 albedo = albedo_texture.read(tid);
    
    // 提取数据
    float3 world_normal = normalize(normal_depth.xyz * 2.0f - 1.0f); //  
    float linear_depth = normal_depth.w;
    float3 surface_albedo = albedo.rgb;
    
    // 深度有效性检查
    if (linear_depth < 0.1f || linear_depth > 64.0f) {
        ssgi_output.write(float4(0.0f, 0.0f, 0.0f, 0.0f), tid);
        return;
    }
    
    // 重建世界坐标
    float3 view_pos = ReconstructViewPos(uv, linear_depth, gd);
    
    // 初始化累积变量
    float3 indirect_lighting = float3(0.0f);
    float total_weight = 0.0f;
    
    // 第一次反弹 - 直接光照采样
    for (int i = 0; i < SSGI_SAMPLE_COUNT; ++i) {
        // 生成半球采样方向
        // 生成随机种子
        float2 noise_seed = uv + float2(i);
        float3 sample_dir = sampleHemisphere(world_normal, noise_seed, i);
        
        // 计算采样位置
        float3 sample_pos = view_pos + sample_dir * SSGI_RADIUS;
        
        // 将采样位置投影到屏幕空间
        float4 sample_clip = gd.Projection * float4(sample_pos, 1.0f);
        float3 sample_ndc = sample_clip.xyz / sample_clip.w;
        
        // Metal坐标系转换
        float2 sample_uv = sample_ndc.xy * 0.5f + 0.5f;
        sample_uv.y = 1.0f - sample_uv.y; // Metal Y轴翻转
        
        // 边界检查
        if (any(sample_uv < 0.0f) || any(sample_uv > 1.0f)) {
            continue;
        }
        
        // 采样深度和法线
        uint2 sample_coord = uint2(sample_uv * float2(gd.CameraPositionAndViewWidth.w, gd.CameraDirectionAndViewHeight.w));
        float4 sample_normal_depth = normal_depth_texture.read(sample_coord);
        float4 sample_albedo = albedo_texture.read(sample_coord);
        float4 sample_gpass = gpass_texture.read(sample_coord);
        
        float sample_linear_depth = sample_normal_depth.w;
        float3 sample_normal = normalize(sample_normal_depth.xyz * 2.0f - 1.0f); //
        float3 sample_view_pos = ReconstructViewPos(sample_uv, sample_linear_depth, gd);
        
        // 深度测试
        float depth_diff = abs(sample_view_pos.z - view_pos.z);
        if (depth_diff > SSGI_THICKNESS) {
            continue;
        }
        
        // 计算权重
        float distance_weight = 1.0f / (1.0f + depth_diff * SSGI_DISTANCE_FALLOFF);
        float normal_weight = max(0.0f, dot(sample_normal, -sample_dir)) * SSGI_NORMAL_WEIGHT;
        float weight = distance_weight * normal_weight;
        
        if (weight < 0.001f) {
            continue;
        }
        
        // 计算该采样点接收到的直接光照
        float3 sample_lighting = float3(0.0f);
        
        // 处理方向光
        // for (uint light_idx = 0; light_idx < gd.NumDirectionalLights; ++light_idx) {
        //     DirectionalLightParameters light = directional_lights[light_idx];
        //     float3 light_dir = -normalize(light.DirectionAndIntensity.xyz);
        //     float light_intensity = light.DirectionAndIntensity.w;
            
        //     // 计算光照贡献
        //     float ndotl = max(0.0f, dot(sample_normal, light_dir));
        //     sample_lighting += light.Color.rgb * light_intensity * ndotl;
        // }
        sample_lighting += sample_gpass.rgb;
        
        // 应用表面反照率
        sample_lighting *= sample_albedo.rgb;
        
        // 累积间接光照
        indirect_lighting += sample_lighting * weight;
        total_weight += weight;
    }
    
    // 归一化结果
    if (total_weight > 0.0f) {
        indirect_lighting /= total_weight;
    }
    
#if SSGI_ENABLE_MULTI_BOUNCE && SSGI_MAX_BOUNCES > 1
    // 多次反弹计算
    float3 multi_bounce_lighting = float3(0.0f);
    float bounce_weight = 0.5f; // 每次反弹的权重衰减
    
    for (int bounce = 1; bounce < SSGI_MAX_BOUNCES; ++bounce) {
        float3 bounce_lighting = float3(0.0f);
        float bounce_total_weight = 0.0f;
        
        // 减少高阶反弹的采样数量以提高性能
        int bounce_samples = SSGI_SAMPLE_COUNT / (bounce + 1);
        
        for (int i = 0; i < bounce_samples; ++i) {
            float2 noise_seed = uv + float2(i);
            float3 sample_dir = sampleHemisphere(world_normal, noise_seed + float2(bounce * 0.1f), i);
            float3 sample_pos = world_pos + sample_dir * SSGI_RADIUS * (1.0f + bounce * 0.5f);
            
            // 投影和采样逻辑与第一次反弹相同
            float4 sample_clip = gd.Projection * float4(sample_pos, 1.0f);
            float3 sample_ndc = sample_clip.xyz / sample_clip.w;
            float2 sample_uv = sample_ndc.xy * 0.5f + 0.5f;
            // sample_uv.y = 1.0f - sample_uv.y;
            
            if (any(sample_uv < 0.0f) || any(sample_uv > 1.0f)) {
                continue;
            }
            
            uint2 sample_coord = uint2(sample_uv * float2(gd.CameraPositionAndViewWidth.w, gd.CameraDirectionAndViewHeight.w));
            float4 sample_normal_depth = normal_depth_texture.read(sample_coord);
            
            float sample_linear_depth = sample_normal_depth.w;
            float3 sample_normal = normalize(sample_normal_depth.xyz * 2.0f - 1.0f);
            
            float depth_diff = abs(sample_linear_depth - length(sample_pos));
            if (depth_diff > SSGI_THICKNESS) {
                continue;
            }
            
            float weight = (1.0f / (1.0f + depth_diff * SSGI_DISTANCE_FALLOFF)) * 
                          max(0.0f, dot(sample_normal, -sample_dir));
            
            if (weight > 0.01f) {
                // 使用前一次反弹的结果作为光源
                bounce_lighting += indirect_lighting * weight;
                bounce_total_weight += weight;
            }
        }
        
        if (bounce_total_weight > 0.0f) {
            multi_bounce_lighting += (bounce_lighting / bounce_total_weight) * bounce_weight;
        }
        
        bounce_weight *= 0.5f; // 每次反弹权重递减
    }
    
    indirect_lighting += multi_bounce_lighting;
#endif
    
    // 应用强度
    indirect_lighting *= SSGI_INTENSITY;
    
    // 计算置信度
    float confidence = min(1.0f, total_weight / float(SSGI_SAMPLE_COUNT));
    
#if SSGI_ENABLE_TEMPORAL
    // 时间累积
    float2 motion_vector = motion_vectors.read(tid).xy;
    float2 prev_uv = uv - motion_vector;
    
    if (all(prev_uv >= 0.0f) && all(prev_uv <= 1.0f)) {
        uint2 prev_coord = uint2(prev_uv * float2(dispatch_params.NumThreads));
        float4 prev_ssgi = previous_ssgi.read(prev_coord);
        
        // 混合当前帧和历史帧
        float temporal_weight = SSGI_TEMPORAL_WEIGHT * prev_ssgi.w; // 使用历史置信度
        indirect_lighting = mix(indirect_lighting, prev_ssgi.rgb, temporal_weight);
        confidence = mix(confidence, prev_ssgi.w, temporal_weight);
    }
#endif
    
    // 输出结果
    ssgi_output.write(float4(indirect_lighting, confidence), tid);
}

#if SSGI_ENABLE_SPATIAL_FILTER
/**
 * @brief SSGI空间滤波内核
 * @param tid 线程ID
 * @param ssgi_input 输入SSGI纹理
 * @param normal_depth_texture 法线深度纹理
 * @param ssgi_output 滤波后的输出纹理
 */
kernel void ssgi_blur(  //ssgi_spatial_filter
    uint2 tid [[thread_position_in_grid]],
    texture2d<float, access::read> ssgi_input [[texture(0)]],
    texture2d<float, access::write> ssgi_output [[texture(1)]],
    texture2d<float, access::read> normal_depth_texture [[texture(2)]],
    device GlobalShaderData& gd [[buffer(0)]]
) {
    if (tid.x >= gd.CameraPositionAndViewWidth.w || tid.y >= gd.CameraDirectionAndViewHeight.w) {
        return;
    }
    
    // 读取中心像素数据
    float4 center_ssgi = ssgi_input.read(tid);
    float4 center_normal_depth = normal_depth_texture.read(tid);
    
    float3 center_normal = normalize(center_normal_depth.xyz * 2.0f - 1.0f);
    float center_depth = center_normal_depth.w;
    
    // 如果置信度太低，直接输出
    if (center_ssgi.w < 0.1f) {
        ssgi_output.write(center_ssgi, tid);
        return;
    }
    
    // 双边滤波
    float3 filtered_color = center_ssgi.rgb * center_ssgi.w;
    float total_weight = center_ssgi.w;
    
    // 3x3滤波核
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            
            int2 sample_coord = int2(tid) + int2(dx, dy);
            
            // 边界检查
            if (sample_coord.x < 0 || sample_coord.y < 0 || 
                sample_coord.x >= gd.CameraPositionAndViewWidth.w || 
                sample_coord.y >= gd.CameraDirectionAndViewHeight.w) {
                continue;
            }
            
            float4 sample_ssgi = ssgi_input.read(uint2(sample_coord));
            float4 sample_normal_depth = normal_depth_texture.read(uint2(sample_coord));
            
            float3 sample_normal = normalize(sample_normal_depth.xyz * 2.0f - 1.0f);
            float sample_depth = sample_normal_depth.w;
            
            // 计算权重
            float normal_weight = max(0.0f, dot(center_normal, sample_normal));
            float depth_weight = exp(-abs(center_depth - sample_depth) * 10.0f);
            float confidence_weight = sample_ssgi.w;
            
            float weight = normal_weight * depth_weight * confidence_weight;
            
            if (weight > 0.01f) {
                filtered_color += sample_ssgi.rgb * weight;
                total_weight += weight;
            }
        }
    }
    
    // 归一化并输出
    if (total_weight > 0.0f) {
        filtered_color /= total_weight;
    }
    
    ssgi_output.write(float4(filtered_color, center_ssgi.w), tid);
}
#endif

// /**
//  * @brief SSGI上采样内核（从低分辨率SSGI恢复到全分辨率）
//  * @param tid 线程ID
//  * @param low_res_ssgi 低分辨率SSGI纹理
//  * @param full_res_normal_depth 全分辨率法线深度纹理
//  * @param ssgi_output 全分辨率输出纹理
//  * @param dispatch_params 调度参数
//  */
// kernel void ssgi_upsample(
//     uint2 tid [[thread_position_in_grid]],
//     texture2d<float, access::read> low_res_ssgi [[texture(0)]],
//     texture2d<float, access::read> full_res_normal_depth [[texture(1)]],
//     texture2d<float, access::write> ssgi_output [[texture(2)]],
//     device SSGIDispatchParameters& dispatch_params [[buffer(0)]]
// ) {
//     if (tid.x >= dispatch_params.NumThreads.x || tid.y >= dispatch_params.NumThreads.y) {
//         return;
//     }
    
//     // 计算低分辨率纹理坐标
//     float2 uv = (float2(tid) + 0.5f) / float2(dispatch_params.NumThreads);
//     float2 low_res_size = float2(textureSize(low_res_ssgi, 0));
//     float2 low_res_uv = uv * low_res_size - 0.5f;
    
//     // 双线性插值采样
//     int2 low_res_coord = int2(floor(low_res_uv));
//     float2 frac = low_res_uv - float2(low_res_coord);
    
//     // 读取全分辨率法线深度
//     float4 full_res_normal_depth = full_res_normal_depth.read(tid);
//     float3 full_res_normal = normalize(full_res_normal_depth.xyz * 2.0f - 1.0f);
//     float full_res_depth = full_res_normal_depth.w;
    
//     // 采样4个相邻像素
//     float4 samples[4];
//     float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    
//     for (int i = 0; i < 4; ++i) {
//         int2 offset = int2(i % 2, i / 2);
//         int2 sample_coord = low_res_coord + offset;
        
//         if (all(sample_coord >= 0) && all(sample_coord < int2(low_res_size))) {
//             samples[i] = low_res_ssgi.read(uint2(sample_coord));
            
//             // 计算基于几何相似性的权重
//             float2 sample_uv = (float2(sample_coord) + 0.5f) / low_res_size;
//             uint2 full_sample_coord = uint2(sample_uv * float2(dispatch_params.NumThreads));
            
//             if (all(full_sample_coord < dispatch_params.NumThreads)) {
//                 float4 sample_normal_depth = full_res_normal_depth.read(full_sample_coord);
//                 float3 sample_normal = normalize(sample_normal_depth.xyz * 2.0f - 1.0f);
//                 float sample_depth = sample_normal_depth.w;
                
//                 float normal_weight = max(0.0f, dot(full_res_normal, sample_normal));
//                 float depth_weight = exp(-abs(full_res_depth - sample_depth) * 5.0f);
                
//                 weights[i] = normal_weight * depth_weight * samples[i].w;
//             }
//         }
//     }
    
//     // 加权平均
//     float3 result_color = float3(0.0f);
//     float total_weight = 0.0f;
    
//     for (int i = 0; i < 4; ++i) {
//         if (weights[i] > 0.01f) {
//             result_color += samples[i].rgb * weights[i];
//             total_weight += weights[i];
//         }
//     }
    
//     if (total_weight > 0.0f) {
//         result_color /= total_weight;
//     }
    
//     float confidence = total_weight > 0.0f ? min(1.0f, total_weight) : 0.0f;
//     ssgi_output.write(float4(result_color, confidence), tid);
// }