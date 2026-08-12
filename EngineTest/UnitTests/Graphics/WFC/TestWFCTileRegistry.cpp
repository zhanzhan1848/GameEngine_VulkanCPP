#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"

#include <vector>

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

// Phase C.1 Task 10: RegisterFromCatalog overwrites per-tile category with the
// catalog's category. Two tiles fed in → both registered, both tagged.
TestResult TestRegisterFromCatalogSetsCategory() {
    WFCTileRegistry registry;
    std::vector<WFCTile> fake(2);
    fake[0].name = "test_a";
    fake[0].variant_count = 1;
    fake[1].name = "test_b";
    fake[1].variant_count = 1;

    registry.RegisterFromCatalog(fake, WFCCategory::Dungeon);
    TEST_ASSERT_EQ(2u, registry.Count(), "registered 2");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Dungeon),
                   static_cast<u32>(registry.Get(wfc_tile_id{0}).category),
                   "tile 0 tagged Dungeon");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Dungeon),
                   static_cast<u32>(registry.Get(wfc_tile_id{1}).category),
                   "tile 1 tagged Dungeon");
    return TestResult::Passed;
}

// Compose two catalogs (Dungeon + Ruins). Tiles land in registration order,
// each tagged with its own catalog's category — the typical multi-source use case.
TestResult TestMultiCatalogAppend() {
    WFCTileRegistry registry;
    std::vector<WFCTile> a(1), b(1);
    a[0].name = "a0"; a[0].variant_count = 1;
    b[0].name = "b0"; b[0].variant_count = 1;

    registry.RegisterFromCatalog(a, WFCCategory::Dungeon);
    registry.RegisterFromCatalog(b, WFCCategory::Ruins);

    TEST_ASSERT_EQ(2u, registry.Count(), "both registered");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Dungeon),
                   static_cast<u32>(registry.Get(wfc_tile_id{0}).category),
                   "first is Dungeon");
    TEST_ASSERT_EQ(static_cast<u32>(WFCCategory::Ruins),
                   static_cast<u32>(registry.Get(wfc_tile_id{1}).category),
                   "second is Ruins");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCTileRegistry");
    TEST_CASE(suite, "Register_And_Get", TestWFCTileRegistry_Register_And_Get);
    TEST_CASE(suite, "MaxVariants_Across_Tiles", TestWFCTileRegistry_MaxVariants_Across_Tiles);
    TEST_CASE(suite, "Count_Grows", TestWFCTileRegistry_Count_Grows);
    TEST_CASE(suite, "RegisterFromCatalogSetsCategory", TestRegisterFromCatalogSetsCategory);
    TEST_CASE(suite, "MultiCatalogAppend", TestMultiCatalogAppend);
    suite.RunAllTests();
    return 0;
}
