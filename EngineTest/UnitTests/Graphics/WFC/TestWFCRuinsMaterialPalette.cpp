#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/RuinsMaterialPalette.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestPalette_Has7Entries() {
    TEST_ASSERT_EQ(7u, kRuinsPaletteCount, "palette size");
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

int main() {
    TestSuite suite("WFCRuinsMaterialPalette");
    TEST_CASE(suite, "Has7Entries",               TestPalette_Has7Entries);
    TEST_CASE(suite, "StoneGrayValues",           TestPalette_StoneGrayValues);
    TEST_CASE(suite, "GetRuinsMaterialId_Stable", TestPalette_GetRuinsMaterialId_Stable);
    suite.RunAllTests();
    return 0;
}
