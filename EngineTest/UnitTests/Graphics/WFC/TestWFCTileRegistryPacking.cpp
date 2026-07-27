#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestWFCTileRegistryPacking_Bit_For_Tile_Variant() {
    WFCTileRegistry reg;
    TEST_ASSERT_EQ(0u, reg.BitForTileVariant(wfc_tile_id{0}, 0), "tile 0 var 0 → bit 0");
    TEST_ASSERT_EQ(7u, reg.BitForTileVariant(wfc_tile_id{0}, 7), "tile 0 var 7 → bit 7");
    TEST_ASSERT_EQ(8u, reg.BitForTileVariant(wfc_tile_id{1}, 0), "tile 1 var 0 → bit 8");
    TEST_ASSERT_EQ(63u, reg.BitForTileVariant(wfc_tile_id{7}, 7), "tile 7 var 7 → bit 63");
    return TestResult::Passed;
}

TestResult TestWFCTileRegistryPacking_Tile_For_Bit() {
    WFCTileRegistry reg;
    TEST_ASSERT_EQ(0u, static_cast<u32>(reg.TileForBit(0)), "bit 0 → tile 0");
    TEST_ASSERT_EQ(0u, static_cast<u32>(reg.TileForBit(7)), "bit 7 → tile 0");
    TEST_ASSERT_EQ(1u, static_cast<u32>(reg.TileForBit(8)), "bit 8 → tile 1");
    TEST_ASSERT_EQ(7u, static_cast<u32>(reg.TileForBit(63)), "bit 63 → tile 7");
    return TestResult::Passed;
}

TestResult TestWFCTileRegistryPacking_Variant_For_Bit() {
    WFCTileRegistry reg;
    TEST_ASSERT_EQ(0u, reg.VariantForBit(0), "bit 0 → variant 0");
    TEST_ASSERT_EQ(7u, reg.VariantForBit(7), "bit 7 → variant 7");
    TEST_ASSERT_EQ(0u, reg.VariantForBit(8), "bit 8 → variant 0");
    TEST_ASSERT_EQ(7u, reg.VariantForBit(63), "bit 63 → variant 7");
    return TestResult::Passed;
}

TestResult TestWFCTileRegistryPacking_Round_Trip() {
    WFCTileRegistry reg;
    for (u32 t = 0; t < 8; ++t) {
        for (u32 v = 0; v < 8; ++v) {
            u32 bit = reg.BitForTileVariant(wfc_tile_id{t}, v);
            TEST_ASSERT_EQ(t, static_cast<u32>(reg.TileForBit(bit)), "round-trip tile");
            TEST_ASSERT_EQ(v, reg.VariantForBit(bit), "round-trip variant");
        }
    }
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCTileRegistryPacking");
    TEST_CASE(suite, "Bit_For_Tile_Variant", TestWFCTileRegistryPacking_Bit_For_Tile_Variant);
    TEST_CASE(suite, "Tile_For_Bit", TestWFCTileRegistryPacking_Tile_For_Bit);
    TEST_CASE(suite, "Variant_For_Bit", TestWFCTileRegistryPacking_Variant_For_Bit);
    TEST_CASE(suite, "Round_Trip", TestWFCTileRegistryPacking_Round_Trip);
    suite.RunAllTests();
    return 0;
}
