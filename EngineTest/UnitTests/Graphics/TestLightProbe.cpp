/**
 * @file TestLightProbe.cpp
 * @brief Light Probe Manager 单元测试
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-28
 * @version 0.1.0
 */

#include "../TestFramework.h"
#include "../../../Engine/Graphics/Lighting/LightProbeManager.h"
#include "../../../Engine/Utilities/SphericalHarmonics.h"
#include <iostream>

using namespace primal::math;
using namespace primal::math::sh;
using namespace primal::graphics::lighting;

namespace primal::test
{
    // 辅助函数：创建纯色 SH
    SH9Color CreateSolidColorSH(const v3& color)
    {
        SH9Color sh;
        // L0 = color * 0.282095 (C0)
        // 实际上 ProjectFunction 会得到这个结果
        // 这里手动设置 L0
        sh.coeffs[0] = color * SH_C0 * 3.544907f; // SH_C0 * sqrt(4pi) ? 
        // 简化：如果 Radiance 是常数 C，则 L0 = C * 2 * sqrt(pi) * C0 = C * 2 * sqrt(pi) * 1/(2sqrt(pi)) = C ?
        // 等等，ProjectFunction(const) -> L0 = integral(C * Y00) = C * Y00 * 4pi = C * (1/2sqrt(pi)) * 4pi = C * 2sqrt(pi) = C * 3.5449
        
        sh.coeffs[0] = color * 3.544907f; 
        return sh;
    }

    // 测试 Probe 管理和插值
    Engine::Test::TestResult TestLightProbeInterpolation()
    {
        LightProbeManager manager;
        
        // Probe A: Position (0,0,0), Color Red (1,0,0)
        LightProbe probeA;
        probeA.position = {0.0f, 0.0f, 0.0f};
        probeA.sh_coeffs = CreateSolidColorSH(v3{1.0f, 0.0f, 0.0f});
        manager.AddProbe(probeA);
        
        // Probe B: Position (10,0,0), Color Blue (0,0,1)
        LightProbe probeB;
        probeB.position = {10.0f, 0.0f, 0.0f};
        probeB.sh_coeffs = CreateSolidColorSH(v3{0.0f, 0.0f, 1.0f});
        manager.AddProbe(probeB);
        
        // Test 1: At Probe A Position -> Should be Red
        SH9Color resultA = manager.GetInterpolatedSH(v3{0.0f, 0.0f, 0.0f});
        // L0 coeff check
        f32 expected_L0 = 3.544907f;
        TEST_ASSERT(is_equal(resultA.coeffs[0].x, expected_L0, 0.01f), "Interpolation at Probe A failed (R)");
        TEST_ASSERT(is_equal(resultA.coeffs[0].z, 0.0f, 0.01f), "Interpolation at Probe A failed (B)");

        // Test 2: At Midpoint (5,0,0) -> Should be Purple (0.5, 0, 0.5)
        // Weight A = 1/25, Weight B = 1/25 -> Equal weights -> Average
        SH9Color resultMid = manager.GetInterpolatedSH(v3{5.0f, 0.0f, 0.0f});
        TEST_ASSERT(is_equal(resultMid.coeffs[0].x, expected_L0 * 0.5f, 0.01f), "Interpolation at Midpoint failed (R)");
        TEST_ASSERT(is_equal(resultMid.coeffs[0].z, expected_L0 * 0.5f, 0.01f), "Interpolation at Midpoint failed (B)");
        
        // Test 3: Closer to B (8,0,0) -> Should be more Blue
        // Dist A = 8, DistSq A = 64, W_A = 1/64
        // Dist B = 2, DistSq B = 4, W_B = 1/4 = 16/64
        // Total W = 17/64
        // Ratio B = 16/17 approx 0.94
        // Ratio A = 1/17 approx 0.06
        SH9Color resultNearB = manager.GetInterpolatedSH(v3{8.0f, 0.0f, 0.0f});
        f32 ratioB = 16.0f / 17.0f;
        f32 ratioA = 1.0f / 17.0f;
        
        TEST_ASSERT(is_equal(resultNearB.coeffs[0].x, expected_L0 * ratioA, 0.01f), "Interpolation near B failed (R)");
        TEST_ASSERT(is_equal(resultNearB.coeffs[0].z, expected_L0 * ratioB, 0.01f), "Interpolation near B failed (B)");

        return Engine::Test::TestResult::Passed;
    }
}

int main()
{
    Engine::Test::TestSuite suite("Light Probe Tests");
    suite.AddTestCase(Engine::Test::TestCase("Light Probe Interpolation", primal::test::TestLightProbeInterpolation));
    
    auto stats = suite.RunAllTests();
    return stats.failedTests > 0 ? 1 : 0;
}
