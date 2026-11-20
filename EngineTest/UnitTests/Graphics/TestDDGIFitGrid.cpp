#include "../TestFramework.h"
#include "../../../Engine/Graphics/Lumen/DDGIFitGrid.h"

using namespace primal::graphics::lumen;

namespace primal::test
{
    // 计划 §6.1：Sponza 几何 AABB（37.2×15.6×22.9）@ spacing 3.0 的逐轴推导
    Engine::Test::TestResult TestFitCountsSponzaSpacing3()
    {
        // ceil(E/S)+1：37.2/3→ceil(12.4)=13+1=14；15.6/3→6+1=7；22.9/3→8+1=9
        TEST_ASSERT(DDGIFitCountForAxis(37.2f, 3.0f) == 14, "x axis 37.2/3.0 must derive 14");
        TEST_ASSERT(DDGIFitCountForAxis(15.6f, 3.0f) == 7,  "y axis 15.6/3.0 must derive 7");
        TEST_ASSERT(DDGIFitCountForAxis(22.9f, 3.0f) == 9,  "z axis 22.9/3.0 must derive 9");
        return Engine::Test::TestResult::Passed;
    }

    // 整盒推导：origin = fit_min 角点、covered = (N-1)*S >= E
    Engine::Test::TestResult TestFitGridFromBounds()
    {
        const math::v3 bmin{-19.21f, -1.26f, -11.83f};
        const math::v3 bmax{ 18.00f, 14.29f,  11.05f};
        const DDGIFitGrid g = DDGIFitGridFromBounds(bmin, bmax, 3.0f);

        TEST_ASSERT(g.count_x == 14 && g.count_y == 7 && g.count_z == 9,
                    "Sponza bbox @3.0 must derive 14x7x9 (plan §3.1)");
        TEST_ASSERT(g.origin.x == bmin.x && g.origin.y == bmin.y && g.origin.z == bmin.z,
                    "probe[0] must sit on the fit box min corner");
        TEST_ASSERT(g.covered.x >= 37.2f && g.covered.y >= 15.6f && g.covered.z >= 22.9f,
                    "(N-1)*S must cover the fit box on every axis");
        TEST_ASSERT_FLOAT_EQ(g.covered.x, 39.0f, 1e-4f, "x covered = 13*3");
        TEST_ASSERT_FLOAT_EQ(g.covered.y, 18.0f, 1e-4f, "y covered = 6*3");
        TEST_ASSERT_FLOAT_EQ(g.covered.z, 24.0f, 1e-4f, "z covered = 8*3");

        // 882 probes：与计划 §3.1 的 probe 数一致
        TEST_ASSERT(g.count_x * g.count_y * g.count_z == 882u, "total probes must be 882");
        return Engine::Test::TestResult::Passed;
    }

    // margin 外扩：每 side +margin 后再推导
    Engine::Test::TestResult TestFitGridMargin()
    {
        const math::v3 bmin{0.0f, 0.0f, 0.0f};
        const math::v3 bmax{30.0f, 30.0f, 30.0f};
        // margin 0: ceil(30/3)+1 = 11
        const DDGIFitGrid g0 = DDGIFitGridFromBounds(bmin, bmax, 3.0f, 0.0f);
        TEST_ASSERT(g0.count_x == 11, "30/3.0 without margin must derive 11");
        // margin 1.5 → extent 33 → ceil(11)+1 = 12
        const DDGIFitGrid g1 = DDGIFitGridFromBounds(bmin, bmax, 3.0f, 1.5f);
        TEST_ASSERT(g1.count_x == 12, "margin 1.5 must expand 30→33 and derive 12");
        TEST_ASSERT_FLOAT_EQ(g1.origin.x, -1.5f, 1e-5f, "margin must move origin outward");
        TEST_ASSERT_FLOAT_EQ(g1.covered.x, 33.0f, 1e-4f, "covered must reach the expanded box");
        return Engine::Test::TestResult::Passed;
    }

    // clamp 边界：[4, 64]（GIEditorPanel 既有钳制范围）
    Engine::Test::TestResult TestFitCountClamp()
    {
        TEST_ASSERT(DDGIFitCountForAxis(0.0f, 3.0f) == 4, "zero extent clamps to min 4");
        TEST_ASSERT(DDGIFitCountForAxis(1.0f, 3.0f) == 4, "tiny extent clamps to min 4");
        TEST_ASSERT(DDGIFitCountForAxis(1e6f, 0.5f) == 64, "huge extent clamps to max 64");
        // 负 extent（反转盒）按 0 处理 → min
        TEST_ASSERT(DDGIFitCountForAxis(-5.0f, 3.0f) == 4, "negative extent clamps to min 4");
        // spacing <= 0 防御：回退 1.0，不产生退化网格
        TEST_ASSERT(DDGIFitCountForAxis(10.0f, 0.0f) == 11, "spacing 0 falls back to 1.0");
        TEST_ASSERT(DDGIFitCountForAxis(10.0f, -2.0f) == 11, "negative spacing falls back to 1.0");
        return Engine::Test::TestResult::Passed;
    }

    // 排除 float 上取整误判：E/S 恰为整数时不得多加一层
    Engine::Test::TestResult TestFitCountExactDivision()
    {
        TEST_ASSERT(DDGIFitCountForAxis(30.0f, 3.0f) == 11, "exact division 30/3 needs ceil=10+1=11");
        TEST_ASSERT(DDGIFitCountForAxis(36.0f, 3.0f) == 13, "exact division 36/3 needs ceil=12+1=13");
        return Engine::Test::TestResult::Passed;
    }

    void RegisterDDGIFitGridTests(Engine::Test::TestSuite& suite)
    {
        suite.AddTestCase({"FitCounts Sponza Spacing3", TestFitCountsSponzaSpacing3});
        suite.AddTestCase({"FitGrid FromBounds", TestFitGridFromBounds});
        suite.AddTestCase({"FitGrid Margin", TestFitGridMargin});
        suite.AddTestCase({"FitCount Clamp", TestFitCountClamp});
        suite.AddTestCase({"FitCount ExactDivision", TestFitCountExactDivision});
    }
}

int main()
{
    Engine::Test::TestSuite suite("DDGI Fit Grid Tests");

    primal::test::RegisterDDGIFitGridTests(suite);

    const auto runStats = suite.RunAllTests();

    return runStats.failedTests == 0 ? 0 : 1;
}
