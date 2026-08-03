#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCFaceCorners.h"
#include "Engine/Graphics/WFC/WFCSocketOps.h"
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

// ============================================================================
// Phase C.1 Task 14: ComputeFaceSignature tests
// ============================================================================

// Unit cube {1,1,1} (half-extent convention; corners in [-1,+1]).
// GetFaceCorners(PosZ) returns corners in this order:
//   [0] = {-1,-1,+1}  y=-1 -> normalized 0 -> quartile 0 -> bits[0:1]=00
//   [1] = {-1,+1,+1}  y=+1 -> normalized 1 -> quartile 3 -> bits[2:3]=11
//   [2] = {+1,+1,+1}  y=+1 -> normalized 1 -> quartile 3 -> bits[4:5]=11
//   [3] = {+1,-1,+1}  y=-1 -> normalized 0 -> quartile 0 -> bits[6:7]=00
// Packed little-endian (corner 0 in low 2 bits): 0b 00 11 11 00 = 0x3C
//
// Note: The Phase C.1 plan claimed cube +Z = 0xFF (all four corners high).
// That is geometrically impossible for a cube — a +Z face has 2 corners at
// y=-hy and 2 at y=+hy by construction. The plan's 0xFF was a documentation
// bug; this test encodes the actual correct value 0x3C and the corner-walk
// derivation above so future readers can audit it.
TestResult TestComputeFaceSignature_CubePosZ_KnownPattern() {
    WFCTile tile{};
    tile.bounds_extents = primal::math::v3{1.0f, 1.0f, 1.0f};
    u8 sig = ComputeFaceSignature(tile, /*variant*/ 0, WFCFace::PosZ);
    TEST_ASSERT_EQ(0x3Cu, static_cast<u32>(sig), "cube +Z = 0x3C (2 high + 2 low)");
    return TestResult::Passed;
}

// A cube is rotationally symmetric about Y; the Y values of any face's 4
// corners are unchanged by Y-axis rotation, so the cube's signature must be
// invariant across all 4 elements of the rotation group. This verifies the
// 4-element invariance (not rotation correctness per se — Y rotation cannot
// change corner Y values for an axis-aligned box, by definition).
TestResult TestComputeFaceSignature_CubeVariantInvariant() {
    WFCTile tile{};
    tile.bounds_extents = primal::math::v3{1.0f, 1.0f, 1.0f};
    u8 sig_v0 = ComputeFaceSignature(tile, 0, WFCFace::PosZ);
    u8 sig_v1 = ComputeFaceSignature(tile, 1, WFCFace::PosZ);
    u8 sig_v2 = ComputeFaceSignature(tile, 2, WFCFace::PosZ);
    u8 sig_v3 = ComputeFaceSignature(tile, 3, WFCFace::PosZ);
    TEST_ASSERT_EQ(static_cast<u32>(sig_v0), static_cast<u32>(sig_v1),
                   "cube v0 == v1 (Y-preservation)");
    TEST_ASSERT_EQ(static_cast<u32>(sig_v0), static_cast<u32>(sig_v2),
                   "cube v0 == v2 (Y-preservation)");
    TEST_ASSERT_EQ(static_cast<u32>(sig_v0), static_cast<u32>(sig_v3),
                   "cube v0 == v3 (Y-preservation)");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCSocketOps");
    TEST_CASE(suite, "GetFaceCorners_AllFaces",  TestGetFaceCorners_AllFaces);
    TEST_CASE(suite, "QuantizeTo2Bit_Boundaries", TestQuantizeTo2Bit_Boundaries);
    TEST_CASE(suite, "QuantizeTo2Bit_OutOfRange", TestQuantizeTo2Bit_OutOfRange);
    TEST_CASE(suite, "ComputeFaceSignature_CubePosZ_KnownPattern", TestComputeFaceSignature_CubePosZ_KnownPattern);
    TEST_CASE(suite, "ComputeFaceSignature_CubeVariantInvariant",  TestComputeFaceSignature_CubeVariantInvariant);
    suite.RunAllTests();
    return 0;
}
