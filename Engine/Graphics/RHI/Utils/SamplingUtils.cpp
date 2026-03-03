/**
 * @file SamplingUtils.cpp
 * @brief 采样工具类实现
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-16
 */

#include "SamplingUtils.h"
#include <random>
#include <algorithm>
#include <cmath>
#include <limits>

namespace primal::graphics::utils {

utl::vector<rhi::math::v2> SamplingUtils::GeneratePoissonDiskSamples(u32 count, u32 numRetries) {
    utl::vector<rhi::math::v2> samples;
    if (count == 0) return samples;
    
    samples.reserve(count);

    // 随机数生成器
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(0.0f, 1.0f);
    std::uniform_real_distribution<float> angleDis(0.0f, rhi::math::constants::TWO_PI);

    // 初始点
    // 为了简单起见，我们使用一种简化的最佳候选算法 (Best Candidate Algorithm) 
    // 或者简单的 Dart Throwing，因为标准的 Poisson Disk 算法 (Bridson's) 需要网格加速结构且很难精确控制数量
    // 这里我们使用 Best Candidate 变体来逼近 Poisson 分布，这对于 Shader 采样通常足够好
    
    // 生成第一个点
    /*
     * 策略：
     * 既然我们需要固定数量的样本，我们可以使用 Mitchell's Best-Candidate Algorithm
     * 对于每个新样本，生成 numRetries 个候选点，选择距离现有样本最远的那个。
     */

    for (u32 i = 0; i < count; ++i) {
        rhi::math::v2 bestCandidate{0.0f, 0.0f};
        float bestDistSq = -1.0f;
        
        for (u32 k = 0; k < numRetries; ++k) {
            // 在单位圆内生成随机点: r = sqrt(random), theta = random
            float r = std::sqrt(dis(gen));
            float theta = angleDis(gen);
            
            rhi::math::v2 candidate{r * std::cos(theta), r * std::sin(theta)};
            
            // 计算到最近现有样本的距离
            float minDistSq = std::numeric_limits<float>::max();
            
            if (i == 0) {
                minDistSq = 1.0f; // 第一个点总是接受
            } else {
                for (const auto& existing : samples) {
                    float dx = candidate.x - existing.x;
                    float dy = candidate.y - existing.y;
                    float dSq = dx*dx + dy*dy;
                    if (dSq < minDistSq) {
                        minDistSq = dSq;
                    }
                }
            }
            
            if (minDistSq > bestDistSq) {
                bestDistSq = minDistSq;
                bestCandidate = candidate;
            }
        }
        
        samples.push_back(bestCandidate);
    }

    return samples;
}

utl::vector<u8> SamplingUtils::GenerateNoiseTexture(u32 size) {
    utl::vector<u8> data;
    data.resize(size * size * 4); // RG16_SNorm = 4 bytes per pixel

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> angleDis(0.0f, rhi::math::constants::TWO_PI);

    int16_t* ptr = reinterpret_cast<int16_t*>(data.data());

    for (u32 i = 0; i < size * size; ++i) {
        float angle = angleDis(gen);
        
        // 旋转向量 (cos, sin)
        // 映射到 [-1, 1] -> int16 [-32767, 32767]
        float c = std::cos(angle);
        float s = std::sin(angle);
        
        ptr[2 * i + 0] = static_cast<int16_t>(c * 32767.0f);
        ptr[2 * i + 1] = static_cast<int16_t>(s * 32767.0f);
    }

    return data;
}

} // namespace primal::graphics::utils
