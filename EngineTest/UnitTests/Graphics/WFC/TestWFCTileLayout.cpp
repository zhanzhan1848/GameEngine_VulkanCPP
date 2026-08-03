#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCTypes.h"

using namespace primal;
using namespace primal::graphics::wfc;
using namespace primal::geometry;
using namespace Engine::Test;

TestResult TestWFCTile_HasMeshHandlesArray() {
    WFCTile t{};
    // 4 variant slots available
    for (u32 i = 0; i < 4u; ++i) {
        t.mesh_handles[i] = geometry_id{100u + i};
    }
    TEST_ASSERT_EQ(100u, static_cast<u32>(t.mesh_handles[0]), "slot 0");
    TEST_ASSERT_EQ(103u, static_cast<u32>(t.mesh_handles[3]), "slot 3");
    return TestResult::Passed;
}

TestResult TestWFCTile_MaxVariantsIs4() {
    TEST_ASSERT_EQ(4u, WFCTile::MaxVariants, "Phase C.1 MaxVariants cap");
    return TestResult::Passed;
}

TestResult TestWFCTile_SizeMatchesStaticAssert() {
    // If this compiles, the static_assert in WFCTypes.h is consistent.
    TEST_ASSERT_EQ(sizeof(WFCTile), sizeof(WFCTile), "tautology — forces layout to be defined");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCTileLayout");
    TEST_CASE(suite, "HasMeshHandlesArray",  TestWFCTile_HasMeshHandlesArray);
    TEST_CASE(suite, "MaxVariantsIs4",       TestWFCTile_MaxVariantsIs4);
    TEST_CASE(suite, "SizeMatchesStaticAssert", TestWFCTile_SizeMatchesStaticAssert);
    suite.RunAllTests();
    return 0;
}
