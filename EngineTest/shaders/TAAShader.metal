/**
 * @file TAAShader.metal
 * @brief Temporal Anti-Aliasing (TAA) 着色器实现
 * @details 利用 时间域信息（多帧结果叠加） 来实现高质量的抗锯齿
 * @author GameEngine Team
 * @date 2025-09-10
 */

#include "Common.h"

constant float texel = 0.5f;
constant float depthThreshold = 0.5f;
constant float normalThreshold = 0.5f;

struct VertexOutput
{
    float4 position [[position]];
    float2 uv;
};

fragment float4 taa_pass(VertexOutput in [[stage_in]],
                          texture2d<float> gpass_texture [[texture(0)]],
                          texture2d<float> motion_vector_texture [[texture(1)]],
                          texture2d<float> normal_depth_texture [[texture(2)]],
                          texture2d<float> prev_taa_texture [[texture(3)]],
                          sampler s [[sampler(0)]])
{
    float2 uv = in.uv;
    uv.y = 1.f - uv.y;
    // ---- Step 1: motion vector 重投影 ----
    // 读取当前帧的运动向量
    float4 motion_vector = motion_vector_texture.sample(s, uv);
    float2 motion = motion_vector.xy;
    // 运动向量表示从当前帧到上一帧的位移，所以是减去运动向量
    // 获取纹理尺寸用于边界检查
    // 计算上一帧的UV坐标并进行边界检查
    float2 prevUVFloat = uv - motion;
    float2 prevUV = clamp(prevUVFloat, float2(0.0f, 0.0f), float2(1.0f, 1.0f));

    // ---- Step 2: 采样历史数据 ----
    float3 historyColor = prev_taa_texture.sample(s, prevUV).rgb;
    float historyDepth  = normal_depth_texture.sample(s, prevUV).w;
    float3 historyNormal= normal_depth_texture.sample(s, prevUV).rgb;

    // ---- Step 3: 当前数据 ----
    // 读取当前帧的渲染结果，包含直接光影计算
    float3 currColor   = gpass_texture.sample(s, uv).rgb;
    float currDepth    = normal_depth_texture.sample(s, uv).w;
    float3 currNormal  = normal_depth_texture.sample(s, uv).rgb;

    // ---- Step 4: 验证几何一致性 ----
    // 深度一致性检测
    bool depthValid = abs(currDepth - historyDepth) < depthThreshold;

    // 法线一致性检测
    bool normalValid = dot(currNormal, historyNormal) > normalThreshold;

    // 如果几何不一致，直接丢弃历史颜色
    if (!depthValid || !normalValid) {
        historyColor = currColor;
    }

    // 获取当前帧邻域的 min/max
    float3 minColor = +9999.0f;
    float3 maxColor = -9999.0f;
    
    for(int i = -1; i <= 1; i++)
    {
        for(int j = -1; j <= 1; j++)
        {
            float2 offset = float2(i, j) * texel;
            float2 samplePos = clamp(uv + offset, float2(0.0f, 0.0f), float2(1.0f, 1.0f));
            float3 color = gpass_texture.sample(s, samplePos).rgb;
            minColor = min(minColor, color);
            maxColor = max(maxColor, color);
        }
    }

    float3 history = clamp(historyColor, minColor, maxColor);

    // 0.05 ~ 0.2
    float blend = 0.1f;
    // 融合效果
    float3 blended = mix(history, currColor, blend);

    return float4(blended, 1.0);
}
