#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCFaceCorners.h"
#include <cmath>
#include <cstring>

using namespace primal::graphics::wfc;
using namespace Engine::Test;

// Produce NaN/Inf via bit pattern so we don't trip -Wnan-infinity-disabled,
// which fires when the compiler can statically observe a NaN/Inf literal.
static f32 MakeNaN() {
    u32 bits = 0x7FC00000u; // quiet NaN
    f32 out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}
static f32 MakePosInf() {
    u32 bits = 0x7F800000u; // +Inf
    f32 out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

TestResult TestGetFaceCorners_AllFaces() {
    primal::math::v3 extent{1.0f, 2.0f, 3.0f};
    primal::math::v3 c[4]{};
    const f32 eps = 0.001f;

    // +X face: all 4 corners must have x == +hx
    GetFaceCorners(extent, WFCFace::PosX, c);
    for (u32 i = 0; i < 4; ++i) {
        TEST_ASSERT(std::abs(c[i].x - (+1.0f)) < eps, "+X face corner off plane");
    }
    // -X face: all 4 corners must have x == -hx
    GetFaceCorners(extent, WFCFace::NegX, c);
    for (u32 i = 0; i < 4; ++i) {
        TEST_ASSERT(std::abs(c[i].x - (-1.0f)) < eps, "-X face corner off plane");
    }
    // +Y face: all 4 corners must have y == +hy
    GetFaceCorners(extent, WFCFace::PosY, c);
    for (u32 i = 0; i < 4; ++i) {
        TEST_ASSERT(std::abs(c[i].y - (+2.0f)) < eps, "+Y face corner off plane");
    }
    // -Y face: all 4 corners must have y == -hy
    GetFaceCorners(extent, WFCFace::NegY, c);
    for (u32 i = 0; i < 4; ++i) {
        TEST_ASSERT(std::abs(c[i].y - (-2.0f)) < eps, "-Y face corner off plane");
    }
    // +Z face: all 4 corners must have z == +hz
    GetFaceCorners(extent, WFCFace::PosZ, c);
    for (u32 i = 0; i < 4; ++i) {
        TEST_ASSERT(std::abs(c[i].z - (+3.0f)) < eps, "+Z face corner off plane");
    }
    // -Z face: all 4 corners must have z == -hz
    GetFaceCorners(extent, WFCFace::NegZ, c);
    for (u32 i = 0; i < 4; ++i) {
        TEST_ASSERT(std::abs(c[i].z - (-3.0f)) < eps, "-Z face corner off plane");
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

TestResult TestQuantizeTo2Bit_OutOfRange() {
    const f32 kNaN  = MakeNaN();
    const f32 kInf  = MakePosInf();
    // NaN: !(NaN > 0.0f) is true -> 0
    TEST_ASSERT_EQ(0u, QuantizeTo2Bit(kNaN), "NaN -> 0");
    // negatives clamp to 0
    TEST_ASSERT_EQ(0u, QuantizeTo2Bit(-0.5f), "-0.5 -> 0");
    TEST_ASSERT_EQ(0u, QuantizeTo2Bit(-1.0f), "-1.0 -> 0");
    // above 1.0 clamp to 3
    TEST_ASSERT_EQ(3u, QuantizeTo2Bit(1.5f), "1.5 -> 3");
    TEST_ASSERT_EQ(3u, QuantizeTo2Bit(100.0f), "100.0 -> 3");
    // +Inf -> 3
    TEST_ASSERT_EQ(3u, QuantizeTo2Bit(kInf), "+Inf -> 3");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCSocketOps");
    TEST_CASE(suite, "GetFaceCorners_AllFaces",  TestGetFaceCorners_AllFaces);
    TEST_CASE(suite, "QuantizeTo2Bit_Boundaries", TestQuantizeTo2Bit_Boundaries);
    TEST_CASE(suite, "QuantizeTo2Bit_OutOfRange", TestQuantizeTo2Bit_OutOfRange);
    suite.RunAllTests();
    return 0;
}
