#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCCategory.h"
#include "Engine/Graphics/WFC/WFCTypes.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestWFCCategory_EnumValues() {
    TEST_ASSERT_EQ(0u, static_cast<u32>(WFCCategory::Primitive), "Primitive == 0");
    TEST_ASSERT_EQ(1u, static_cast<u32>(WFCCategory::Ruins),     "Ruins == 1");
    TEST_ASSERT_EQ(2u, static_cast<u32>(WFCCategory::Dungeon),   "Dungeon == 2");
    TEST_ASSERT_EQ(3u, static_cast<u32>(WFCCategory::Cyber),     "Cyber == 3");
    TEST_ASSERT_EQ(4u, static_cast<u32>(WFCCategory::Organic),   "Organic == 4");
    return TestResult::Passed;
}

TestResult TestWFCCategory_CategoryMaskFor() {
    TEST_ASSERT_EQ(1u << 0, CategoryMaskFor(WFCCategory::Primitive), "Primitive bit");
    TEST_ASSERT_EQ(1u << 1, CategoryMaskFor(WFCCategory::Ruins),     "Ruins bit");
    TEST_ASSERT_EQ(1u << 4, CategoryMaskFor(WFCCategory::Organic),   "Organic bit");
    return TestResult::Passed;
}

TestResult TestWFCCategory_TileDefault() {
    WFCTile t{};
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Primitive),
                   static_cast<u32>(t.category), "default category");
    return TestResult::Passed;
}

TestResult TestWFCCategory_TileFieldAssignable() {
    WFCTile t{};
    t.category = WFCCategory::Ruins;
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Ruins),
                   static_cast<u32>(t.category), "assign Ruins");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCCategory");
    TEST_CASE(suite, "EnumValues",          TestWFCCategory_EnumValues);
    TEST_CASE(suite, "CategoryMaskFor",     TestWFCCategory_CategoryMaskFor);
    TEST_CASE(suite, "TileDefault",         TestWFCCategory_TileDefault);
    TEST_CASE(suite, "TileFieldAssignable", TestWFCCategory_TileFieldAssignable);
    suite.RunAllTests();
    return 0;
}
