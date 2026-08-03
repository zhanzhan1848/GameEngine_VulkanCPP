#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/RuinsMaterialPalette.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestPalette_Has7Entries() {
    // Name kept for stable test registration; count grew to 8 in T12 (vine_cube_avg).
    TEST_ASSERT_EQ(8u, kRuinsPaletteCount, "palette size");
    return TestResult::Passed;
}

TestResult TestPalette_StoneGrayValues() {
    const auto& m = kRuinsPalette[0];
    TEST_ASSERT_EQ(0.0f, m.metallic,    "stone_gray metallic");
    TEST_ASSERT_EQ(0.85f, m.roughness,  "stone_gray roughness");
    TEST_ASSERT(std::abs(m.albedo_tint.x - 0.45f) < 0.001f, "stone_gray r");
    TEST_ASSERT(std::abs(m.albedo_tint.y - 0.45f) < 0.001f, "stone_gray g");
    TEST_ASSERT(std::abs(m.albedo_tint.z - 0.45f) < 0.001f, "stone_gray b");
    return TestResult::Passed;
}

TestResult TestPalette_GetRuinsMaterialId_Stable() {
    u32 a = GetRuinsMaterialId(0);
    u32 b = GetRuinsMaterialId(0);
    TEST_ASSERT_EQ(a, b, "id stable across calls");
    return TestResult::Passed;
}

TestResult TestPalette_VineCubeAverageColor() {
    const auto& stone = kRuinsPalette[0];
    const auto& moss  = kRuinsPalette[2];
    primal::math::v3 expected_avg{
        (stone.albedo_tint.x + moss.albedo_tint.x) * 0.5f,
        (stone.albedo_tint.y + moss.albedo_tint.y) * 0.5f,
        (stone.albedo_tint.z + moss.albedo_tint.z) * 0.5f,
    };
    TEST_ASSERT_EQ(8u, kRuinsPaletteCount, "vine entry added");
    const auto& vine = kRuinsPalette[7];
    TEST_ASSERT(std::abs(vine.albedo_tint.x - expected_avg.x) < 0.001f, "vine r");
    TEST_ASSERT(std::abs(vine.albedo_tint.y - expected_avg.y) < 0.001f, "vine g");
    TEST_ASSERT(std::abs(vine.albedo_tint.z - expected_avg.z) < 0.001f, "vine b");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCRuinsMaterialPalette");
    TEST_CASE(suite, "Has7Entries",               TestPalette_Has7Entries);
    TEST_CASE(suite, "StoneGrayValues",           TestPalette_StoneGrayValues);
    TEST_CASE(suite, "GetRuinsMaterialId_Stable", TestPalette_GetRuinsMaterialId_Stable);
    TEST_CASE(suite, "VineCubeAverageColor",      TestPalette_VineCubeAverageColor);
    suite.RunAllTests();
    return 0;
}
