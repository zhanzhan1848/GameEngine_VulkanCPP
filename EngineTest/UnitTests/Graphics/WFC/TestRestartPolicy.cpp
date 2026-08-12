#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/RestartPolicy.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestRestartPolicy_First_Contradiction_Returns_Restart() {
    RestartPolicy policy(/*max_generations=*/4);
    auto decision = policy.OnContradiction({1, 2, 3}, wfc_tile_id{0}, /*generation=*/0);
    TEST_ASSERT(decision == RestartPolicy::Decision::Restart, "First contradiction should restart");
    return TestResult::Passed;
}

TestResult TestRestartPolicy_Exceeds_Max_Generations_Returns_GiveUp() {
    RestartPolicy policy(/*max_generations=*/3);
    // Burn through 3 restarts
    policy.OnContradiction({0, 0, 0}, wfc_tile_id{0}, 0);
    policy.OnContradiction({0, 0, 0}, wfc_tile_id{0}, 1);
    policy.OnContradiction({0, 0, 0}, wfc_tile_id{0}, 2);
    auto decision = policy.OnContradiction({0, 0, 0}, wfc_tile_id{0}, 3);
    TEST_ASSERT(decision == RestartPolicy::Decision::GiveUp, "After max_generations should give up");
    return TestResult::Passed;
}

TestResult TestRestartPolicy_SnapshotConflicts_Grows_Memory() {
    RestartPolicy policy(/*max_generations=*/8);
    policy.OnContradiction({1, 1, 1}, wfc_tile_id{0}, 0);
    policy.OnContradiction({2, 2, 2}, wfc_tile_id{1}, 1);
    TEST_ASSERT_EQ(2u, policy.ConflictCount(), "Two conflicts recorded");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("RestartPolicy");
    TEST_CASE(suite, "First_Contradiction_Returns_Restart", TestRestartPolicy_First_Contradiction_Returns_Restart);
    TEST_CASE(suite, "Exceeds_Max_Generations_Returns_GiveUp", TestRestartPolicy_Exceeds_Max_Generations_Returns_GiveUp);
    TEST_CASE(suite, "SnapshotConflicts_Grows_Memory", TestRestartPolicy_SnapshotConflicts_Grows_Memory);
    suite.RunAllTests();
    return 0;
}
