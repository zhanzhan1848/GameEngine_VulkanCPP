#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCRandom.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestWFCRandom_Same_Seed_Produces_Same_Sequence() {
    WFCRandom a{42};
    WFCRandom b{42};
    for (u32 i = 0; i < 10; ++i) {
        TEST_ASSERT_EQ(a.NextU32(), b.NextU32(), "Same seed -> same sequence");
    }
    return TestResult::Passed;
}

TestResult TestWFCRandom_Different_Seeds_Produce_Different_Sequence() {
    WFCRandom a{42};
    WFCRandom b{1337};
    u32 diffs = 0;
    for (u32 i = 0; i < 10; ++i) {
        if (a.NextU32() != b.NextU32()) ++diffs;
    }
    TEST_ASSERT_EQ(10u, diffs, "Different seeds -> different values (10/10)");
    return TestResult::Passed;
}

TestResult TestWFCRandom_NextRange_Bounded() {
    WFCRandom r{7};
    for (u32 i = 0; i < 1000; ++i) {
        u32 v = r.NextRange(10);
        TEST_ASSERT(v < 10, "NextRange(10) returns value < 10");
    }
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCRandom");
    TEST_CASE(suite, "Same_Seed_Produces_Same_Sequence", TestWFCRandom_Same_Seed_Produces_Same_Sequence);
    TEST_CASE(suite, "Different_Seeds_Produce_Different_Sequence", TestWFCRandom_Different_Seeds_Produce_Different_Sequence);
    TEST_CASE(suite, "NextRange_Bounded", TestWFCRandom_NextRange_Bounded);
    suite.RunAllTests();
    return 0;
}
