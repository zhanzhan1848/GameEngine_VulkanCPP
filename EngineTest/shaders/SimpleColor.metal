/**
 * @file SimpleColor.metal
 * @brief 基础颜色测试着色器
 * @details 用于集成测试的简单着色器，支持 MVP 变换和顶点颜色输出
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-12
 */

#include <metal_stdlib>
using namespace metal;

/**
 * @brief 顶点输入结构体
 */
struct VertexIn {
    float3 position [[attribute(0)]]; ///< 顶点位置 (Location 0)
    float3 color [[attribute(1)]];    ///< 顶点颜色 (Location 1)
};

/**
 * @brief 顶点输出结构体
 */
struct VertexOut {
    float4 position [[position]];     ///< 裁剪空间位置
    float4 color;                     ///< 传递给片段着色器的颜色
};

/**
 * @brief Uniform 数据结构体
 */
struct Uniforms {
    float4x4 viewProjectionMatrix;    ///< 视图投影矩阵
    float4x4 modelMatrix;             ///< 模型矩阵
};

/**
 * @brief 顶点着色器入口函数
 * @param in 顶点输入
 * @param uniforms Uniform 数据 (Buffer 1)
 * @return 变换后的顶点数据
 */
vertex VertexOut vertexMain(
    VertexIn in [[stage_in]],
    constant Uniforms& uniforms [[buffer(1)]])
{
    VertexOut out;
    float4 worldPos = uniforms.modelMatrix * float4(in.position, 1.0);
    out.position = uniforms.viewProjectionMatrix * worldPos;
    out.color = float4(in.color, 1.0);
    return out;
}

/**
 * @brief 片段着色器入口函数
 * @param in 顶点着色器输出插值
 * @return 最终颜色
 */
fragment float4 fragmentMain(VertexOut in [[stage_in]]) {
    return in.color;
}
