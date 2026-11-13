#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCSolveBudget.h"

#include <thread>
#include <chrono>

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestWFCSolveBudget_Initial_State_Allows_Continue() {
    WFCSolveBudget budget(/*max_cells_per_frame=*/64, /*max_ms_per_frame=*/4);
    TEST_ASSERT(budget.ShouldContinue(), "Fresh budget should allow continuation");
    return TestResult::Passed;
}

TestResult TestWFCSolveBudget_Reset_Clears_State() {
    WFCSolveBudget budget(/*max_cells_per_frame=*/2, /*max_ms_per_frame=*/100);
    budget.OnCellCollapsed();  // consume 1 cell
    budget.OnCellCollapsed();  // consume 2 cells
    TEST_ASSERT(!budget.ShouldContinue(), "Should be exhausted after 2 cells");
    budget.Reset();
    TEST_ASSERT(budget.ShouldContinue(), "Reset should clear exhaustion");
    return TestResult::Passed;
}

TestResult TestWFCSolveBudget_Cell_Limit_Stops() {
    WFCSolveBudget budget(/*max_cells_per_frame=*/3, /*max_ms_per_frame=*/1000);
    budget.OnCellCollapsed();
    TEST_ASSERT(budget.ShouldContinue(), "1/3 cells OK");
    budget.OnCellCollapsed();
    TEST_ASSERT(budget.ShouldContinue(), "2/3 cells OK");
    budget.OnCellCollapsed();
    TEST_ASSERT(!budget.ShouldContinue(), "3/3 cells: should stop");
    return TestResult::Passed;
}

TestResult TestWFCSolveBudget_Time_Limit_Stops() {
    WFCSolveBudget budget(/*max_cells_per_frame=*/1000000, /*max_ms_per_frame=*/1);
    budget.Reset();
    // Sleep 5ms to exceed 1ms budget
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    TEST_ASSERT(!budget.ShouldContinue(), "After 5ms with 1ms budget, should stop");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCSolveBudget");
    TEST_CASE(suite, "Initial_State_Allows_Continue", TestWFCSolveBudget_Initial_State_Allows_Continue);
    TEST_CASE(suite, "Reset_Clears_State", TestWFCSolveBudget_Reset_Clears_State);
    TEST_CASE(suite, "Cell_Limit_Stops", TestWFCSolveBudget_Cell_Limit_Stops);
    TEST_CASE(suite, "Time_Limit_Stops", TestWFCSolveBudget_Time_Limit_Stops);
    suite.RunAllTests();
    return 0;
}
