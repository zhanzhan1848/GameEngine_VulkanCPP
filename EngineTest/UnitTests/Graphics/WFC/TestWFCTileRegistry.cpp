#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestWFCTileRegistry_Register_And_Get() {
    WFCTileRegistry registry;
    WFCTile tile{};
    tile.name = "test_tile";
    tile.variant_count = 1;
    tile.sockets[0] = 0;

    wfc_tile_id id = registry.Register(tile);
    TEST_ASSERT(static_cast<u32>(id) >= 0, "Should return valid id");

    const WFCTile& fetched = registry.Get(id);
    TEST_ASSERT_STR_EQ("test_tile", fetched.name, "Fetched tile name matches");  // FIXED: was TEST_ASSERT_EQ_STR
    return TestResult::Passed;
}

TestResult TestWFCTileRegistry_MaxVariants_Across_Tiles() {
    WFCTileRegistry registry;
    WFCTile a{};
    a.variant_count = 2;
    WFCTile b{};
    b.variant_count = 4;  // MaxVariantsPerTile cap; assert in Register rejects > 4.
    registry.Register(a);
    registry.Register(b);
    TEST_ASSERT_EQ(4u, registry.MaxVariants(), "Max variants = max across registered tiles");
    return TestResult::Passed;
}

TestResult TestWFCTileRegistry_Count_Grows() {
    WFCTileRegistry registry;
    WFCTile t{};
    t.variant_count = 1;
    registry.Register(t);
    registry.Register(t);
    registry.Register(t);
    TEST_ASSERT_EQ(3u, registry.Count(), "3 tiles registered");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCTileRegistry");
    TEST_CASE(suite, "Register_And_Get", TestWFCTileRegistry_Register_And_Get);
    TEST_CASE(suite, "MaxVariants_Across_Tiles", TestWFCTileRegistry_MaxVariants_Across_Tiles);
    TEST_CASE(suite, "Count_Grows", TestWFCTileRegistry_Count_Grows);
    suite.RunAllTests();
    return 0;
}
