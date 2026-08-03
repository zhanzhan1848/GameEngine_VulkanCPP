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

// ============================================================================
// Phase C.1 Task 15: DeriveSocketEncoding tests
// ============================================================================
//
// DeriveSocketEncoding packs 6 face signatures (8 bits/face) into a u64:
//   layout: [posX:8][negX:8][posY:8][negY:8][posZ:8][negZ:8][reserved:16]
//   byte at index f (0..5) corresponds to WFCFace f.
// Top 16 bits are reserved (must be 0 today).
//
// Geometric derivation for unit cube {1,1,1} (half-extent hy=1):
//   normalized_y = (y + 1)/2 -> y=-1 maps to 0, y=+1 maps to 1.
//
//   PosX corners (per GetFaceCorners): {(+1,-1,-1),(+1,+1,-1),(+1,+1,+1),(+1,-1,+1)}
//     y=[-1,+1,+1,-1] -> quartiles [0,3,3,0] -> 0b 00_11_11_00 = 0x3C
//   NegX corners: {(-1,-1,+1),(-1,+1,+1),(-1,+1,-1),(-1,-1,-1)}
//     y=[-1,+1,+1,-1] -> [0,3,3,0] -> 0x3C
//   PosY corners: all y=+1 -> all quartile 3 -> 0xFF
//   NegY corners: all y=-1 -> all quartile 0 -> 0x00
//   PosZ corners: {(-1,-1,+1),(-1,+1,+1),(+1,+1,+1),(+1,-1,+1)}
//     y=[-1,+1,+1,-1] -> [0,3,3,0] -> 0x3C
//   NegZ corners: {(+1,-1,-1),(+1,+1,-1),(-1,+1,-1),(-1,-1,-1)}
//     y=[-1,+1,+1,-1] -> [0,3,3,0] -> 0x3C
//
// Plan bug note: The Phase C.1 plan claimed the unit cube would produce
// SocketEncoding = 0xFFFFFFFFFFFF (all 6 faces 0xFF each). This is
// geometrically impossible for an axis-aligned cube — only the +Y face has
// all-high corners; the 4 side faces each have 2 high + 2 low corners, and
// the -Y face has all-low corners. The actual cube encoding is therefore
// 0x00003C3CFF3C3C (bytes [0..5] = 3C,3C,FF,00,3C,3C, top 16 bits zero).
TestResult TestDeriveSocketEncoding_CubeKnownLayout() {
    WFCTile tile{};
    tile.bounds_extents = primal::math::v3{1.0f, 1.0f, 1.0f};
    SocketEncoding enc = DeriveSocketEncoding(tile, /*variant*/ 0);

    // Unpack byte f into bytes[f] (byte 0 = PosX, ..., byte 5 = NegZ).
    u8 bytes[6];
    for (u32 i = 0; i < 6; ++i) {
        bytes[i] = static_cast<u8>((enc >> (i * 8)) & 0xFF);
    }

    // +Y (index 2) and -Y (index 3) are deterministic: cube top all-high,
    // cube bottom all-low.
    TEST_ASSERT_EQ(0xFFu, static_cast<u32>(bytes[2]), "posY byte (all high)");
    TEST_ASSERT_EQ(0x00u, static_cast<u32>(bytes[3]), "negY byte (all low)");

    // Mixed side faces: 2 high + 2 low corners each -> 0x3C (see comment above).
    TEST_ASSERT_EQ(0x3Cu, static_cast<u32>(bytes[0]), "posX byte (mixed 2 high + 2 low)");
    // Cube is symmetric: all 4 side faces produce the same signature.
    TEST_ASSERT_EQ(static_cast<u32>(bytes[0]), static_cast<u32>(bytes[1]), "posX == negX");
    TEST_ASSERT_EQ(static_cast<u32>(bytes[0]), static_cast<u32>(bytes[4]), "posX == posZ");
    TEST_ASSERT_EQ(static_cast<u32>(bytes[0]), static_cast<u32>(bytes[5]), "posX == negZ");

    return TestResult::Passed;
}

// Top 16 bits are reserved for future use (e.g. half-tile or sub-face variants).
// They must be 0 today so the encoding is exactly 48 bits + 16 reserved.
TestResult TestDeriveSocketEncoding_Top16BitsReserved() {
    WFCTile tile{};
    tile.bounds_extents = primal::math::v3{1.0f, 1.0f, 1.0f};
    SocketEncoding enc = DeriveSocketEncoding(tile, /*variant*/ 0);
    TEST_ASSERT_EQ(0u, static_cast<u32>(enc >> 48), "top 16 bits reserved");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCSocketOps");
    TEST_CASE(suite, "GetFaceCorners_AllFaces",  TestGetFaceCorners_AllFaces);
    TEST_CASE(suite, "QuantizeTo2Bit_Boundaries", TestQuantizeTo2Bit_Boundaries);
    TEST_CASE(suite, "QuantizeTo2Bit_OutOfRange", TestQuantizeTo2Bit_OutOfRange);
    TEST_CASE(suite, "ComputeFaceSignature_CubePosZ_KnownPattern", TestComputeFaceSignature_CubePosZ_KnownPattern);
    TEST_CASE(suite, "ComputeFaceSignature_CubeVariantInvariant",  TestComputeFaceSignature_CubeVariantInvariant);
    TEST_CASE(suite, "DeriveSocketEncoding_CubeKnownLayout",  TestDeriveSocketEncoding_CubeKnownLayout);
    TEST_CASE(suite, "DeriveSocketEncoding_Top16BitsReserved", TestDeriveSocketEncoding_Top16BitsReserved);
    suite.RunAllTests();
    return 0;
}
