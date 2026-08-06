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
    suite.RunAllTests();
    return 0;
}
