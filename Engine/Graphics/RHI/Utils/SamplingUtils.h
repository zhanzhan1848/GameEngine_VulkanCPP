/**
 * @file SamplingUtils.h
 * @brief 采样工具类
 * @details 提供 Poisson Disk 采样生成和 Noise Texture 生成功能
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-16
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include <cstdint>

namespace primal::graphics::utils {

/**
 * @brief 采样工具类
 */
class SamplingUtils {
public:
    /**
     * @brief 生成 Poisson Disk 采样点
     * @details 在单位圆内生成均匀分布且满足泊松圆盘特性的采样点
     * @param count 目标采样点数量
     * @param numRetries 候选点重试次数，越高分布越均匀，但生成越慢
     * @return 采样点列表 (坐标范围 [-1, 1])
     */
    static utl::vector<rhi::math::v2> GeneratePoissonDiskSamples(u32 count, u32 numRetries = 30);

    /**
     * @brief 生成随机旋转 Noise Texture 数据
     * @details 生成用于随机旋转 Poisson Kernel 的纹理数据
     *          格式为 RG16_SNorm (对应 RHI 的 DataFormat::RG16_SNorm)
     *          R 通道: cos(theta) * 32767
     *          G 通道: sin(theta) * 32767
     *          数据布局: 行优先
     * @param size 纹理尺寸 (例如 4, 16, 64)
     * @return 纹理数据字节数组 (大小 = size * size * 4)
     */
    static utl::vector<u8> GenerateNoiseTexture(u32 size);
};

} // namespace primal::graphics::utils
