#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCFaceCorners.h"

using namespace primal::graphics::wfc;
using namespace Engine::Test;

TestResult TestGetFaceCorners_PosZ() {
    primal::math::v3 extent{1.0f, 1.0f, 1.0f};
    primal::math::v3 c[4]{};
    GetFaceCorners(extent, WFCFace::PosZ, c);
    for (u32 i = 0; i < 4; ++i) {
        TEST_ASSERT(std::abs(c[i].z - 1.0f) < 0.001f, "corner on +Z plane");
    }
    return TestResult::Passed;
}

TestResult TestQuantizeTo2Bit_Boundaries() {
    TEST_ASSERT_EQ(0u, QuantizeTo2Bit(0.0f),  "0.0 -> 0");
    TEST_ASSERT_EQ(0u, QuantizeTo2Bit(0.24f), "0.24 -> 0");
    TEST_ASSERT_EQ(1u, QuantizeTo2Bit(0.25f), "0.25 -> 1");
    TEST_ASSERT_EQ(1u, QuantizeTo2Bit(0.49f), "0.49 -> 1");
    TEST_ASSERT_EQ(2u, QuantizeTo2Bit(0.50f), "0.50 -> 2");
    TEST_ASSERT_EQ(2u, QuantizeTo2Bit(0.74f), "0.74 -> 2");
    TEST_ASSERT_EQ(3u, QuantizeTo2Bit(0.75f), "0.75 -> 3");
    TEST_ASSERT_EQ(3u, QuantizeTo2Bit(1.0f),  "1.0 -> 3");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCSocketOps");
    TEST_CASE(suite, "GetFaceCorners_PosZ",  TestGetFaceCorners_PosZ);
    TEST_CASE(suite, "QuantizeTo2Bit_Boundaries", TestQuantizeTo2Bit_Boundaries);
    suite.RunAllTests();
    return 0;
}
