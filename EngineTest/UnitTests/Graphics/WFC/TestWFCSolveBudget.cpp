#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCSolveBudget.h"

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

int main() {
    TestSuite suite("WFCSolveBudget");
    TEST_CASE(suite, "Initial_State_Allows_Continue", TestWFCSolveBudget_Initial_State_Allows_Continue);
    TEST_CASE(suite, "Reset_Clears_State", TestWFCSolveBudget_Reset_Clears_State);
    suite.RunAllTests();
    return 0;
}
