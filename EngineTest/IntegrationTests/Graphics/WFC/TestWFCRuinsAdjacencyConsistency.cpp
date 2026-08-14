// EngineTest/IntegrationTests/Graphics/WFC/TestWFCRuinsAdjacencyConsistency.cpp
// Phase C.1 T24: verifies every (tile, variant, face) in the populated catalog
// has at least one compatible partner. A zero-partner tuple would dead-lock
// the solver when it tries to propagate from that cell — this test prevents
// that failure mode.
#include "../../../UnitTests/TestFramework.h"
#include "Engine/Graphics/WFC/WFCConfig.h"
#include "Engine/Graphics/WFC/WFCTileCatalog.h"
#include "Engine/Graphics/WFC/WFCTileRegistry.h"
#include "Engine/Graphics/WFC/TileAdjacency.h"
#include "Engine/Graphics/WFC/WFCTypes.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestRuinsAdjacency_NoDeadEnd() {
    WFCTileRegistry reg;
    TileAdjacencyTable adj;
    WFCTileCatalog::Populate(reg, adj);

    // Iterate every (tile, variant, face). Each must have >= 1 partner.
    // For the 15-tile Phase C.1 catalog this is 15 tiles * 4 variants * 6
    // faces = 360 HasAnyPair calls; each scans the unordered_set so the
    // whole test is well under a second.
    for (u32 ta = 0; ta < reg.Count(); ++ta) {
        const WFCTile& tile_a = reg.Get(wfc_tile_id{ta});
        for (u32 va = 0; va < tile_a.variant_count; ++va) {
            for (u32 f = 0; f < WFC_FACE_COUNT_3D; ++f) {
                WFCFace face = static_cast<WFCFace>(f);
                bool has_pair = adj.HasAnyPair(wfc_tile_id{ta}, va, face);
                TEST_ASSERT(has_pair,
                            "every (tile, variant, face) has >= 1 compatible pair");
            }
        }
    }
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCRuinsAdjacencyConsistency");
    TEST_CASE(suite, "RuinsAdjacency_NoDeadEnd", TestRuinsAdjacency_NoDeadEnd);
    suite.RunAllTests();
    return 0;
}
