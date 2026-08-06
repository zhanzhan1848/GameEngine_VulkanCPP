// EngineTest/UnitTests/Graphics/WFC/TestConflictSeedMemory.cpp
//
// Phase C.1 Task 12: RestartPolicy tracks full ConflictRecord (coord + tile +
// occurrence_count), deduped by (coord, tile). Phase C.1 Task 13 adds bias
// weighting + decay on top of these records.
#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/RestartPolicy.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

// Same (coord, tile) called twice should dedupe — one record, occurrence_count=2.
// This is the core invariant ConflictSeedMemory relies on: re-failing the same
// cell with the same tile strengthens the bias against it, doesn't bloat the
// record set.
TestResult TestRecordConflictIncrementsOccurrence() {
    RestartPolicy rp(/*max_generations=*/8);
    rp.OnContradiction(WFCGridCoord{1, 2, 3}, wfc_tile_id{5}, /*gen=*/0);
    rp.OnContradiction(WFCGridCoord{1, 2, 3}, wfc_tile_id{5}, /*gen=*/0);
    auto records = rp.ConflictRecords();
    TEST_ASSERT_EQ(1u, records.size(), "one record deduped");
    TEST_ASSERT_EQ(2u, records[0].occurrence_count, "two occurrences");
    return TestResult::Passed;
}

// Tile id is part of the record key — Phase B observer needs it to bias against
// specific tile choices at a cell, not just the cell itself.
TestResult TestConflictRecordsTrackTile() {
    RestartPolicy rp(/*max_generations=*/8);
    rp.OnContradiction(WFCGridCoord{0, 0, 0}, wfc_tile_id{2}, /*gen=*/0);
    auto records = rp.ConflictRecords();
    TEST_ASSERT_EQ(1u, records.size(), "one record");
    TEST_ASSERT_EQ(static_cast<u32>(2u),
                   static_cast<u32>(records[0].tile),
                   "tile tracked");
    return TestResult::Passed;
}

// Same coord, different tile → two distinct records. The observer needs both:
// "this cell fails on tile A but might succeed on tile B" must not collapse to
// a single cell-level record.
TestResult TestConflictRecordsDedupByCoordAndTile() {
    RestartPolicy rp(/*max_generations=*/8);
    rp.OnContradiction(WFCGridCoord{1, 1, 1}, wfc_tile_id{2}, /*gen=*/0);
    rp.OnContradiction(WFCGridCoord{1, 1, 1}, wfc_tile_id{7}, /*gen=*/0);
    TEST_ASSERT_EQ(2u, rp.ConflictRecords().size(), "two distinct records");
    return TestResult::Passed;
}

// ConflictCount() returns the number of distinct (coord, tile) records.
// Tests that already relied on this behaviour (TestRestartPolicy) still see a
// monotonically growing count when fed distinct pairs.
TestResult TestConflictCountCountsDistinctRecords() {
    RestartPolicy rp(/*max_generations=*/8);
    rp.OnContradiction(WFCGridCoord{1, 1, 1}, wfc_tile_id{0}, /*gen=*/0);
    rp.OnContradiction(WFCGridCoord{2, 2, 2}, wfc_tile_id{1}, /*gen=*/0);
    TEST_ASSERT_EQ(2u, rp.ConflictCount(), "two distinct pairs");
    return TestResult::Passed;
}

// Reset() must clear records so a fresh solve starts with no bias memory.
TestResult TestResetClearsConflictRecords() {
    RestartPolicy rp(/*max_generations=*/8);
    rp.OnContradiction(WFCGridCoord{1, 2, 3}, wfc_tile_id{5}, /*gen=*/0);
    rp.Reset();
    TEST_ASSERT_EQ(0u, rp.ConflictRecords().size(), "records cleared");
    TEST_ASSERT_EQ(0u, rp.ConflictCount(), "count is zero");
    return TestResult::Passed;
}

// --- Task 13: ApplyBias + Decay ---

// BiasForCell sums occurrence_count over all (coord, *) records — a cell that
// has failed on multiple tiles accumulates a larger penalty than one that
// failed on a single tile. The observer adds this to base entropy so
// conflict-prone cells get picked later (more entropy = picked later under
// the lowest-entropy heuristic).
TestResult TestBiasForCellSumsAcrossTiles() {
    RestartPolicy rp(/*max_generations=*/8);
    rp.OnContradiction(WFCGridCoord{1, 2, 3}, wfc_tile_id{5}, /*gen=*/0);
    rp.OnContradiction(WFCGridCoord{1, 2, 3}, wfc_tile_id{5}, /*gen=*/0);
    rp.OnContradiction(WFCGridCoord{1, 2, 3}, wfc_tile_id{6}, /*gen=*/0);
    // 2 occurrences on tile 5 + 1 on tile 6 = 3 total
    TEST_ASSERT_EQ(3.0f, rp.BiasForCell(WFCGridCoord{1, 2, 3}), "cell bias sums");
    // An unrelated cell must read zero.
    TEST_ASSERT_EQ(0.0f, rp.BiasForCell(WFCGridCoord{9, 9, 9}),
                   "no bias for untracked cell");
    return TestResult::Passed;
}

// BiasForTileInCell returns the occurrence_count for a specific (cell, tile)
// pair, or 0 if the pair has never failed. The observer subtracts this from
// the candidate's weight so the picker avoids re-selecting the same failed tile.
TestResult TestBiasForTileInCell() {
    RestartPolicy rp(/*max_generations=*/8);
    rp.OnContradiction(WFCGridCoord{1, 2, 3}, wfc_tile_id{5}, /*gen=*/0);
    rp.OnContradiction(WFCGridCoord{1, 2, 3}, wfc_tile_id{5}, /*gen=*/0);
    TEST_ASSERT_EQ(2.0f, rp.BiasForTileInCell(WFCGridCoord{1, 2, 3}, wfc_tile_id{5}),
                   "tile bias matches occurrence_count");
    TEST_ASSERT_EQ(0.0f, rp.BiasForTileInCell(WFCGridCoord{1, 2, 3}, wfc_tile_id{6}),
                   "untracked tile bias zero");
    return TestResult::Passed;
}

// DecayAll halves every occurrence_count (integer division) and drops records
// that hit zero. Called at the start of each restart so old conflicts fade —
// transient failures don't permanently block a tile from re-selection.
TestResult TestDecayHalvesAndDropsZero() {
    RestartPolicy rp(/*max_generations=*/8);
    rp.OnContradiction(WFCGridCoord{1, 2, 3}, wfc_tile_id{5}, /*gen=*/0);
    rp.OnContradiction(WFCGridCoord{1, 2, 3}, wfc_tile_id{5}, /*gen=*/0);  // count=2
    rp.OnContradiction(WFCGridCoord{4, 5, 6}, wfc_tile_id{7}, /*gen=*/0);  // count=1
    rp.DecayAll();
    auto records = rp.ConflictRecords();
    TEST_ASSERT_EQ(1u, records.size(), "single-occurrence record decayed to 0 and dropped");
    TEST_ASSERT_EQ(1u, records[0].occurrence_count, "count=2 halved to 1");
    return TestResult::Passed;
}

// Idempotent decay: decaying an empty policy is a no-op (no crash, no records
// appear). Decaying twice in a row should be the same as decaying once when
// there's only a count=1 record (already gone after first call).
TestResult TestDecayIdempotentOnEmpty() {
    RestartPolicy rp(/*max_generations=*/8);
    rp.DecayAll();
    TEST_ASSERT_EQ(0u, rp.ConflictCount(), "empty stays empty");
    rp.OnContradiction(WFCGridCoord{0, 0, 0}, wfc_tile_id{1}, /*gen=*/0);
    rp.DecayAll();
    rp.DecayAll();
    TEST_ASSERT_EQ(0u, rp.ConflictCount(), "count=1 gone after one decay");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("ConflictSeedMemory");
    TEST_CASE(suite, "RecordConflictIncrementsOccurrence",
              TestRecordConflictIncrementsOccurrence);
    TEST_CASE(suite, "ConflictRecordsTrackTile",
              TestConflictRecordsTrackTile);
    TEST_CASE(suite, "ConflictRecordsDedupByCoordAndTile",
              TestConflictRecordsDedupByCoordAndTile);
    TEST_CASE(suite, "ConflictCountCountsDistinctRecords",
              TestConflictCountCountsDistinctRecords);
    TEST_CASE(suite, "ResetClearsConflictRecords",
              TestResetClearsConflictRecords);
    TEST_CASE(suite, "BiasForCellSumsAcrossTiles",
              TestBiasForCellSumsAcrossTiles);
    TEST_CASE(suite, "BiasForTileInCell",
              TestBiasForTileInCell);
    TEST_CASE(suite, "DecayHalvesAndDropsZero",
              TestDecayHalvesAndDropsZero);
    TEST_CASE(suite, "DecayIdempotentOnEmpty",
              TestDecayIdempotentOnEmpty);
    suite.RunAllTests();
    return 0;
}
