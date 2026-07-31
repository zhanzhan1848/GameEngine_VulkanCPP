#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestBitForTileVariant_16x4() {
    TEST_ASSERT_EQ(0u,  WFCTileRegistry::BitForTileVariant(wfc_tile_id{0},  0), "t0 v0 → bit 0");
    TEST_ASSERT_EQ(3u,  WFCTileRegistry::BitForTileVariant(wfc_tile_id{0},  3), "t0 v3 → bit 3");
    TEST_ASSERT_EQ(4u,  WFCTileRegistry::BitForTileVariant(wfc_tile_id{1},  0), "t1 v0 → bit 4");
    TEST_ASSERT_EQ(15u, WFCTileRegistry::BitForTileVariant(wfc_tile_id{3},  3), "t3 v3 → bit 15");
    TEST_ASSERT_EQ(63u, WFCTileRegistry::BitForTileVariant(wfc_tile_id{15}, 3), "t15 v3 → bit 63");
    return TestResult::Passed;
}

TestResult TestTileForBit_16x4() {
    TEST_ASSERT_EQ(0u,  static_cast<u32>(WFCTileRegistry::TileForBit(0)),  "bit 0 → t0");
    TEST_ASSERT_EQ(0u,  static_cast<u32>(WFCTileRegistry::TileForBit(3)),  "bit 3 → t0");
    TEST_ASSERT_EQ(1u,  static_cast<u32>(WFCTileRegistry::TileForBit(4)),  "bit 4 → t1");
    TEST_ASSERT_EQ(15u, static_cast<u32>(WFCTileRegistry::TileForBit(63)), "bit 63 → t15");
    return TestResult::Passed;
}

TestResult TestVariantForBit_16x4() {
    TEST_ASSERT_EQ(0u, WFCTileRegistry::VariantForBit(0),  "bit 0 → v0");
    TEST_ASSERT_EQ(3u, WFCTileRegistry::VariantForBit(3),  "bit 3 → v3");
    TEST_ASSERT_EQ(0u, WFCTileRegistry::VariantForBit(4),  "bit 4 → v0");
    TEST_ASSERT_EQ(3u, WFCTileRegistry::VariantForBit(63), "bit 63 → v3");
    return TestResult::Passed;
}

TestResult TestRoundTrip_16x4() {
    for (u32 t = 0; t < 16; ++t) {
        for (u32 v = 0; v < 4; ++v) {
            u32 bit = WFCTileRegistry::BitForTileVariant(wfc_tile_id{t}, v);
            TEST_ASSERT(bit < 64, "bit in range");
            TEST_ASSERT_EQ(t, static_cast<u32>(WFCTileRegistry::TileForBit(bit)), "tile roundtrip");
            TEST_ASSERT_EQ(v, WFCTileRegistry::VariantForBit(bit), "variant roundtrip");
        }
    }
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCTileRegistryPacking_16x4");
    TEST_CASE(suite, "BitForTileVariant", TestBitForTileVariant_16x4);
    TEST_CASE(suite, "TileForBit",        TestTileForBit_16x4);
    TEST_CASE(suite, "VariantForBit",     TestVariantForBit_16x4);
    TEST_CASE(suite, "RoundTrip",         TestRoundTrip_16x4);
    suite.RunAllTests();
    return 0;
}
