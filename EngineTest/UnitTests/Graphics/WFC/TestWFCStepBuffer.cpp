#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCStepBuffer.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestWFCStepBuffer_Push_And_Consume_Single() {
    WFCStepBuffer buf;
    WFCStep in{};
    in.kind = WFCStepKind::Collapse;
    in.coord = {1, 2, 3};
    in.tile = wfc_tile_id{7};
    in.variant = 2;
    buf.Push(in);

    TEST_ASSERT(!buf.Empty(), "Buffer non-empty after Push");

    WFCStep out[4];
    u32 consumed = buf.Consume(out, 4);
    TEST_ASSERT_EQ(1u, consumed, "Consume returns 1");
    TEST_ASSERT(out[0].kind == WFCStepKind::Collapse, "Kind matches");
    TEST_ASSERT_EQ(1, out[0].coord.x, "Coord X matches");
    TEST_ASSERT_EQ(7u, static_cast<u32>(out[0].tile), "Tile matches");
    TEST_ASSERT(buf.Empty(), "Buffer empty after Consume");
    return TestResult::Passed;
}

TestResult TestWFCStepBuffer_Consume_Respects_Max_Count() {
    WFCStepBuffer buf;
    for (u32 i = 0; i < 5; ++i) {
        WFCStep s{};
        s.kind = WFCStepKind::Collapse;
        s.coord = {static_cast<s32>(i), 0, 0};
        buf.Push(s);
    }
    WFCStep out[3];
    u32 consumed = buf.Consume(out, 3);
    TEST_ASSERT_EQ(3u, consumed, "Should return max_count when more available");
    TEST_ASSERT(!buf.Empty(), "Buffer still has 2 items");
    return TestResult::Passed;
}

TestResult TestWFCStepBuffer_Empty_Consume_Returns_Zero() {
    WFCStepBuffer buf;
    WFCStep out[4];
    u32 consumed = buf.Consume(out, 4);
    TEST_ASSERT_EQ(0u, consumed, "Empty consume returns 0");
    TEST_ASSERT(buf.Empty(), "Buffer reports empty");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCStepBuffer");
    TEST_CASE(suite, "Push_And_Consume_Single", TestWFCStepBuffer_Push_And_Consume_Single);
    TEST_CASE(suite, "Consume_Respects_Max_Count", TestWFCStepBuffer_Consume_Respects_Max_Count);
    TEST_CASE(suite, "Empty_Consume_Returns_Zero", TestWFCStepBuffer_Empty_Consume_Returns_Zero);
    suite.RunAllTests();
    return 0;
}
