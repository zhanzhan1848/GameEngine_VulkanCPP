// TestWFCCandidateLayout.cpp
// Phase C.1 Task 1: Verify the widened candidate_mask layout (u64[4] = 256 bits)
// and the bumped MaxTiles constant (16 → 64). This is the layout-only canary;
// propagation/solver refactors come in Tasks 2-3.
#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCTypes.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestMaskArraySize() {
    WFCCell c{};
    static_assert(sizeof(c.candidate_mask) == sizeof(u64[4]),
                  "candidate_mask must be u64[4] (256 bits)");
    return TestResult::Passed;
}

TestResult TestMaxTilesBumped() {
    TEST_ASSERT(WFCTileRegistry::MaxTiles >= 64u, "MaxTiles must be >= 64");
    TEST_ASSERT_EQ(WFCTileRegistry::MaxTiles * WFCTileRegistry::MaxVariantsPerTile,
                   256u, "tile*variant must equal 256 (mask capacity)");
    return TestResult::Passed;
}

TestResult TestBitForTileVariantInRange() {
    // Tile 63, variant 3 → bit 255 (top of last u64 word).
    u32 bit = WFCTileRegistry::BitForTileVariant(wfc_tile_id{63}, 3);
    TEST_ASSERT_EQ(255u, bit, "tile 63 var 3 = bit 255");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCCandidateLayout");
    TEST_CASE(suite, "MaskArraySize",             TestMaskArraySize);
    TEST_CASE(suite, "MaxTilesBumped",            TestMaxTilesBumped);
    TEST_CASE(suite, "BitForTileVariantInRange",  TestBitForTileVariantInRange);
    suite.RunAllTests();
    return 0;
}
