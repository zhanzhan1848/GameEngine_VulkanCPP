/**
 * @file TestSamplingUtils.cpp
 * @brief SamplingUtils 单元测试
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-16
 */

#include "UnitTests/TestFramework.h"
#include "Graphics/RHI/Utils/SamplingUtils.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include <cmath>
#include <vector>

using namespace primal::graphics::utils;
using namespace primal::graphics::rhi::math;
using namespace Engine::Test;

TestResult TestGeneratePoissonDiskSamples_Count() {
    uint32_t count = 16;
    auto samples = SamplingUtils::GeneratePoissonDiskSamples(count);
    
    TEST_ASSERT(samples.size() == count, "Sample count mismatch");
    return TestResult::Passed;
}

TestResult TestGeneratePoissonDiskSamples_WithinUnitCircle() {
    uint32_t count = 64;
    auto samples = SamplingUtils::GeneratePoissonDiskSamples(count);
    
    for (const auto& p : samples) {
        float lenSq = p.x * p.x + p.y * p.y;
        TEST_ASSERT(lenSq <= 1.0f + 1e-4f, "Sample outside unit circle");
    }
    return TestResult::Passed;
}

TestResult TestGeneratePoissonDiskSamples_Distribution() {
    uint32_t count = 32;
    auto samples = SamplingUtils::GeneratePoissonDiskSamples(count);
    
    float minTotalDist = 0.0f;
    for (size_t i = 0; i < samples.size(); ++i) {
        float minDist = 100.0f;
        for (size_t j = 0; j < samples.size(); ++j) {
            if (i == j) continue;
            float dx = samples[i].x - samples[j].x;
            float dy = samples[i].y - samples[j].y;
            float dist = std::sqrt(dx*dx + dy*dy);
            if (dist < minDist) minDist = dist;
        }
        minTotalDist += minDist;
    }
    float avgMinDist = minTotalDist / count;
    
    // 对于 32 个点在单位圆内，平均最小距离应该在一定范围内 (例如 > 0.1)
    TEST_ASSERT(avgMinDist > 0.1f, "Samples are too clustered");
    return TestResult::Passed;
}

TestResult TestGenerateNoiseTexture_Size() {
    uint32_t size = 4;
    auto noiseData = SamplingUtils::GenerateNoiseTexture(size);
    // RG16_SNorm: 2 bytes * 2 channels = 4 bytes per pixel
    TEST_ASSERT(noiseData.size() == size * size * 4, "Noise texture size mismatch");
    return TestResult::Passed;
}

TestResult TestGenerateNoiseTexture_Values() {
    uint32_t size = 4;
    auto noiseData = SamplingUtils::GenerateNoiseTexture(size);
    
    const int16_t* ptr = reinterpret_cast<const int16_t*>(noiseData.data());
    
    for (uint32_t i = 0; i < size * size; ++i) {
        int16_t r = ptr[2 * i + 0];
        int16_t g = ptr[2 * i + 1];
        
        float fr = r / 32767.0f;
        float fg = g / 32767.0f;
        
        float len = std::sqrt(fr*fr + fg*fg);
        TEST_ASSERT(std::abs(len - 1.0f) < 0.05f, "Noise vector not normalized");
    }
    return TestResult::Passed;
}

int main() {
    TestSuite suite("SamplingUtils Tests");
    
    suite.AddTestCase(TestCase("GeneratePoissonDiskSamples_Count", TestGeneratePoissonDiskSamples_Count));
    suite.AddTestCase(TestCase("GeneratePoissonDiskSamples_WithinUnitCircle", TestGeneratePoissonDiskSamples_WithinUnitCircle));
    suite.AddTestCase(TestCase("GeneratePoissonDiskSamples_Distribution", TestGeneratePoissonDiskSamples_Distribution));
    suite.AddTestCase(TestCase("GenerateNoiseTexture_Size", TestGenerateNoiseTexture_Size));
    suite.AddTestCase(TestCase("GenerateNoiseTexture_Values", TestGenerateNoiseTexture_Values));
    
    TestStats stats = suite.RunAllTests();
    
    return stats.failedTests == 0 ? 0 : 1;
}
