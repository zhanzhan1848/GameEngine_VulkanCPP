
#include "../TestFramework.h"
#include "../../../Engine/Utilities/SphericalHarmonics.h"
#include "../../../Engine/Utilities/Math.h"
#include <iostream>

using namespace primal::math;
using namespace primal::math::sh;

namespace primal::test
{
    // 测试 SH 系数结构体大小和对齐
    Engine::Test::TestResult TestSHDataStructures()
    {
        TEST_ASSERT(sizeof(SH9) == 9 * sizeof(f32), "SH9 size mismatch");
        TEST_ASSERT(sizeof(SH16) == 16 * sizeof(f32), "SH16 size mismatch");
        TEST_ASSERT(sizeof(SH9Color) == 9 * sizeof(v3), "SH9Color size mismatch");
        
        // 测试初始化
        SH9 sh9{};
        for(int i=0; i<9; ++i) TEST_ASSERT(sh9.coeffs[i] == 0.0f, "SH9 init failed");
        
        return Engine::Test::TestResult::Passed;
    }

    // 测试 SH 基函数求值 (L0 - L1)
    Engine::Test::TestResult TestSHEvaluation()
    {
        v3 dir_z = {0.0f, 0.0f, 1.0f}; // Z axis
        SH9 coeffs;
        EvalSHBasis(dir_z, coeffs);

        // L0
        TEST_ASSERT(is_equal(coeffs.coeffs[0], 0.282095f, 0.001f), "SH L0 evaluation error");
        
        // L1 (Z axis)
        // m=-1 (-y) -> 0
        // m=0  (z)  -> 0.488603 * 1.0
        // m=1  (-x) -> 0
        TEST_ASSERT(is_equal(coeffs.coeffs[1], 0.0f, 0.001f), "SH L1 m-1 evaluation error"); 
        TEST_ASSERT(is_equal(coeffs.coeffs[2], 0.488603f, 0.001f), "SH L1 m0 evaluation error");
        TEST_ASSERT(is_equal(coeffs.coeffs[3], 0.0f, 0.001f), "SH L1 m1 evaluation error");

        return Engine::Test::TestResult::Passed;
    }

    // 测试重建 (Constant function)
    Engine::Test::TestResult TestSHReconstruction()
    {
        // 投影常数函数 f(s) = 1.0
        // L0 系数应该是 Integral(1.0 * Y00) = Y00 * 4*PI = 0.282095 * 12.566 = 3.5449
        
        SH9 coeffs{};
        coeffs.coeffs[0] = 3.544907f;
        
        v3 dir = {0.0f, 0.0f, 1.0f}; // Any direction
        f32 value = ReconstructSH(coeffs, dir);
        
        TEST_ASSERT(is_equal(value, 1.0f, 0.01f), "SH Reconstruction of constant failed");

        return Engine::Test::TestResult::Passed;
    }

    // 测试 Irradiance Convolution
    Engine::Test::TestResult TestSHConvolution()
    {
        // 如果输入是常数环境光 1.0
        SH9 radiance{};
        radiance.coeffs[0] = 3.544907f; // L0 coeff for 1.0

        SH9 irradiance{};
        ConvolveCosineLobe(radiance, irradiance);

        // Irradiance for uniform 1.0 should be PI
        // Irradiance L0 = Radiance L0 * A0 = 3.544907 * PI = 11.136
        // Reconstructed value should be PI ?
        // Reconstruct(IrradianceSH, dir) = Irradiance L0 * Y00 = 11.136 * 0.282 = 3.14 (PI)
        
        v3 dir = {0.0f, 1.0f, 0.0f};
        f32 value = ReconstructSH(irradiance, dir);
        
        TEST_ASSERT(is_equal(value, 3.14159f, 0.01f), "SH Irradiance Convolution failed");

        return Engine::Test::TestResult::Passed;
    }

    // 测试 SH 投影 (Monte Carlo)
    Engine::Test::TestResult TestSHProjection()
    {
        // 投影常数函数 f(s) = 1.0
        // 理论值: L0 = 2 * sqrt(pi) = 3.5449
        
        auto constant_func = [](const v3& /*dir*/) -> v3 {
            return {1.0f, 1.0f, 1.0f};
        };
        
        SH9Color result;
        ProjectFunction(constant_func, 1024, result); // 1024 samples
        
        // 验证 L0
        f32 expected_l0 = 3.544907f;
        TEST_ASSERT(is_equal(result.coeffs[0].x, expected_l0, 0.1f), "SH Projection L0 error (x)"); // 允许较大误差因为是 MC 积分
        TEST_ASSERT(is_equal(result.coeffs[0].y, expected_l0, 0.1f), "SH Projection L0 error (y)");
        TEST_ASSERT(is_equal(result.coeffs[0].z, expected_l0, 0.1f), "SH Projection L0 error (z)");
        
        // 验证 L1 (应该接近 0)
        TEST_ASSERT(is_equal(result.coeffs[1].x, 0.0f, 0.1f), "SH Projection L1 error");
        
        return Engine::Test::TestResult::Passed;
    }

    // 测试带方向的函数投影 (Clamp Cosine - 半球)
    Engine::Test::TestResult TestSHProjectionClampCosine()
    {
        // f(w) = max(0, w . n) where n = (0, 1, 0) (Y-up)
        // 这是一个Cosine Lobe，用于Irradiance
        // 理论 SH 系数 (Y-up):
        // L0 = sqrt(pi) / 2 * Y00 * 2 * sqrt(pi) ? No.
        // Analytic coefficients for clamped cosine along Z:
        // L0 = 0.8862
        // L1 = 1.0233 (Z)
        // L2 = 0.4954 (Z^2)
        // Rotating to Y:
        // L0 = 0.8862
        // L1 = 1.0233 (Y) -> coeffs[1] (-y) ? No. coeffs[1] is Y1,-1 (y).
        // Let's rely on reconstruction test.
        
        auto cosine_func = [](const v3& dir) -> v3 {
            f32 val = std::max(0.0f, dir.y);
            return {val, val, val};
        };
        
        SH9Color result;
        ProjectFunction(cosine_func, 4096, result);
        
        // Reconstruct at normal direction (0, 1, 0) -> should be close to 1.0
        v3 dir_y = {0.0f, 1.0f, 0.0f};
        v3 recon = ReconstructSH(result, dir_y);
        
        TEST_ASSERT(is_equal(recon.y, 1.0f, 0.15f), "SH Projection Cosine Reconstruction error"); // Gibbs phenomenon might cause ringing

        return Engine::Test::TestResult::Passed;
    }

    // 测试 CubeMap 采样
    Engine::Test::TestResult TestCubeMapSampling()
    {
        // 1x1 CubeMap Data (RGB f32)
        // Static data to ensure valid pointers during test
        static f32 face_px[6][3] = {
            {1.0f, 0.0f, 0.0f}, // +X Red
            {0.0f, 1.0f, 0.0f}, // -X Green
            {0.0f, 0.0f, 1.0f}, // +Y Blue
            {1.0f, 1.0f, 0.0f}, // -Y Yellow
            {0.0f, 1.0f, 1.0f}, // +Z Cyan
            {1.0f, 0.0f, 1.0f}  // -Z Magenta
        };

        CubeMapDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.stride = 3 * sizeof(f32);
        for(int i=0; i<6; ++i) desc.data[i] = face_px[i];

        // Sample +X
        v3 dir_px = {1.0f, 0.0f, 0.0f};
        v3 val_px = SampleCubeMap(desc, dir_px);
        TEST_ASSERT(is_equal(val_px.x, 1.0f, 0.001f), "CubeMap Sample +X failed (R)");
        TEST_ASSERT(is_equal(val_px.y, 0.0f, 0.001f), "CubeMap Sample +X failed (G)");
        TEST_ASSERT(is_equal(val_px.z, 0.0f, 0.001f), "CubeMap Sample +X failed (B)");

        // Sample -Y
        v3 dir_ny = {0.0f, -1.0f, 0.0f};
        v3 val_ny = SampleCubeMap(desc, dir_ny);
        TEST_ASSERT(is_equal(val_ny.x, 1.0f, 0.001f), "CubeMap Sample -Y failed (R)");
        TEST_ASSERT(is_equal(val_ny.y, 1.0f, 0.001f), "CubeMap Sample -Y failed (G)");
        TEST_ASSERT(is_equal(val_ny.z, 0.0f, 0.001f), "CubeMap Sample -Y failed (B)");

        return Engine::Test::TestResult::Passed;
    }

    // 测试 CubeMap 投影
    Engine::Test::TestResult TestCubeMapProjection()
    {
        // 构造一个简单的 CubeMap (所有面都是白色)
        static f32 white[3] = {1.0f, 1.0f, 1.0f};
        
        CubeMapDesc desc;
        desc.width = 1;
        desc.height = 1;
        desc.stride = 3 * sizeof(f32);
        for(int i=0; i<6; ++i) desc.data[i] = white;

        SH9Color result;
        ProjectCubeMap(desc, 1024, result);

        // 应该是常数 1.0 的 SH 系数 (L0 ~ 3.54)
        TEST_ASSERT(is_equal(result.coeffs[0].x, 3.5449f, 0.1f), "CubeMap Projection (White) L0 error");

        return Engine::Test::TestResult::Passed;
    }

    void RegisterSHTests(Engine::Test::TestSuite& suite)
    {
        suite.AddTestCase({"SH Data Structures", TestSHDataStructures});
        suite.AddTestCase({"SH Evaluation", TestSHEvaluation});
        suite.AddTestCase({"SH Reconstruction", TestSHReconstruction});
        suite.AddTestCase({"SH Convolution", TestSHConvolution});
        suite.AddTestCase({"SH Projection (Constant)", TestSHProjection});
        suite.AddTestCase({"SH Projection (Cosine)", TestSHProjectionClampCosine});
        suite.AddTestCase({"CubeMap Sampling", TestCubeMapSampling});
        suite.AddTestCase({"CubeMap Projection", TestCubeMapProjection});
    }
}

int main()
{
    Engine::Test::TestSuite suite("Spherical Harmonics Tests");
    
    primal::test::RegisterSHTests(suite);

    suite.RunAllTests();

    return 0;
}
