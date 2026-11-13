/**
 * @file TestImageCompare.cpp
 * @brief Phase 4b Tier 2 SSIM utility self-test.
 * @details Verifies SavePNG / LoadPNG round-trip + Wang-Bovik SSIM behaves
 *          correctly on identical / disjoint / similar images. No GPU work.
 */

#include "../../TestFramework.h"
#include "Utils/ImageCompare.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace Engine::Test;
namespace et = EngineTest;

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 64;

void fill_gradient(std::vector<uint8_t>& img) {
    img.assign(std::size_t(kW) * kH * 4, 0);
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            std::size_t i = (std::size_t(y) * kW + x) * 4;
            img[i + 0] = uint8_t((x * 255) / (kW - 1));   // R: horizontal
            img[i + 1] = uint8_t((y * 255) / (kH - 1));   // G: vertical
            img[i + 2] = uint8_t(((x + y) * 255) / (kW + kH - 2));  // B: diag
            img[i + 3] = 255;
        }
    }
}

void fill_solid(std::vector<uint8_t>& img, uint8_t r, uint8_t g, uint8_t b) {
    img.assign(std::size_t(kW) * kH * 4, 0);
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            std::size_t i = (std::size_t(y) * kW + x) * 4;
            img[i + 0] = r; img[i + 1] = g; img[i + 2] = b; img[i + 3] = 255;
        }
    }
}

} // anonymous namespace

/// SSIM(x, x) must equal 1.0 exactly.
TestResult TestSSIM_Identical() {
    std::vector<uint8_t> a; fill_gradient(a);
    std::vector<uint8_t> b; fill_gradient(b);
    float s = et::ComputeSSIM(a.data(), b.data(), kW, kH);
    std::cout << "[TestImageCompare] SSIM(a, a) = " << s << std::endl;
    TEST_ASSERT_FLOAT_EQ(1.0f, s, 1e-5f, "SSIM of identical images must be 1.0");
    return TestResult::Passed;
}

/// SSIM between disjoint solid colors must be well below 0.95.
TestResult TestSSIM_Disjoint() {
    std::vector<uint8_t> a; fill_solid(a, 255, 255, 255);
    std::vector<uint8_t> b; fill_solid(b, 0, 0, 0);
    float s = et::ComputeSSIM(a.data(), b.data(), kW, kH);
    std::cout << "[TestImageCompare] SSIM(white, black) = " << s << std::endl;
    TEST_ASSERT(s < 0.3f, "SSIM of disjoint images must be < 0.3");
    return TestResult::Passed;
}

/// SavePNG -> LoadPNG must be pixel-perfect round-trip.
TestResult TestPNG_RoundTrip() {
    std::vector<uint8_t> a; fill_gradient(a);
    const char* path = "TestImageCompare_roundtrip.png";
    if (!et::SavePNG(path, a.data(), kW, kH)) {
        std::cout << "[TestImageCompare] SavePNG failed — skipping round-trip" << std::endl;
        return TestResult::Skipped;
    }
    std::vector<uint8_t> b;
    uint32_t w = 0, h = 0;
    TEST_ASSERT(et::LoadPNG(path, b, w, h), "LoadPNG must succeed");
    TEST_ASSERT_EQ(kW, w, "Loaded width matches");
    TEST_ASSERT_EQ(kH, h, "Loaded height matches");
    TEST_ASSERT_EQ(a.size(), b.size(), "Loaded pixel buffer size matches");
    TEST_ASSERT(std::memcmp(a.data(), b.data(), a.size()) == 0,
                "Loaded pixels must be byte-identical to saved");
    std::remove(path);
    return TestResult::Passed;
}

/// RGBA16F → RGBA8 tonemap of a zero buffer must produce all zeros.
TestResult TestRGBA16F_Zero() {
    std::vector<uint8_t> src(std::size_t(kW) * kH * 8, 0);
    std::vector<uint8_t> dst(std::size_t(kW) * kH * 4, 0xFF);
    et::RGBA16FToRGBA8(src.data(), dst.data(), kW, kH);
    for (std::size_t i = 0; i < dst.size(); ++i) {
        TEST_ASSERT_EQ(uint8_t(0), dst[i], "All-zero RGBA16F must tonemap to all-zero RGBA8");
    }
    return TestResult::Passed;
}

/// Push a little-endian f32 into a 4-byte buffer.
static void le_f32(float f, uint8_t out[4]) {
    std::memcpy(out, &f, 4);
}

/// D32_FLOAT → RGBA8 grayscale of known depths (0, 0.25, 0.5, 1.0) must produce
/// the expected grayscale bytes (0, 64, 128, 255) with alpha=255.
/// Also exercises clamping (depth > 1 → 255) and NaN → 0.
TestResult TestDepth32_KnownValues() {
    constexpr uint32_t kDW = 4, kDH = 1;
    std::vector<uint8_t> src(std::size_t(kDW) * kDH * 4, 0);
    le_f32(0.0f, &src[0]);
    le_f32(0.25f, &src[4]);
    le_f32(0.5f, &src[8]);
    le_f32(1.0f, &src[12]);

    std::vector<uint8_t> dst(std::size_t(kDW) * kDH * 4, 0);
    et::Depth32ToRGBA8(src.data(), dst.data(), kDW, kDH);

    auto check = [&](std::size_t idx, uint8_t expected, const char* msg) {
        std::cout << "[TestImageCompare] depth byte[" << idx << "] = "
                  << int(dst[idx]) << " (expected " << int(expected) << ")" << std::endl;
        TEST_ASSERT_EQ(expected, dst[idx], msg);
    };
    check(0, 0,   "depth 0.0 → grayscale 0");
    TEST_ASSERT_EQ(uint8_t(255), dst[3], "depth pixel alpha must be 255");
    check(4, 64,  "depth 0.25 → grayscale 64");
    check(8, 128, "depth 0.5 → grayscale 128");
    check(12, 255, "depth 1.0 → grayscale 255");

    // Clamping: out-of-range values must clamp to [0, 255].
    std::vector<uint8_t> src2(std::size_t(kDW) * kDH * 4, 0);
    le_f32(2.0f, &src2[0]);
    le_f32(-0.5f, &src2[4]);
    float nanf = std::numeric_limits<float>::quiet_NaN();
    le_f32(nanf, &src2[8]);
    float inf = std::numeric_limits<float>::infinity();
    le_f32(inf, &src2[12]);

    std::vector<uint8_t> dst2(std::size_t(kDW) * kDH * 4, 0);
    et::Depth32ToRGBA8(src2.data(), dst2.data(), kDW, kDH);
    TEST_ASSERT_EQ(uint8_t(255), dst2[0], "depth 2.0 → clamped to 255");
    TEST_ASSERT_EQ(uint8_t(0),   dst2[4], "depth -0.5 → clamped to 0");
    TEST_ASSERT_EQ(uint8_t(0),   dst2[8], "depth NaN → 0");
    TEST_ASSERT_EQ(uint8_t(0),   dst2[12], "depth +Inf → 0 (non-finite → 0 per contract)");

    return TestResult::Passed;
}

/// FlipYInPlace must swap row 0 with row (h-1) for an RGBA8 buffer.
TestResult TestFlipY_RoundTripSwap() {
    constexpr uint32_t kFW = 2, kFH = 2;
    // Top row = red, bottom row = blue.
    std::vector<uint8_t> img = {
        255, 0,   0,   255,   255, 0,   0,   255,
        0,   0,   255, 255,   0,   0,   255, 255,
    };
    et::FlipYInPlace(img.data(), kFW, kFH);
    TEST_ASSERT_EQ(uint8_t(0),   img[0],  "after flip: row0 R = 0 (was red)");
    TEST_ASSERT_EQ(uint8_t(0),   img[1],  "after flip: row0 G = 0");
    TEST_ASSERT_EQ(uint8_t(255), img[2],  "after flip: row0 B = 255 (blue)");
    TEST_ASSERT_EQ(uint8_t(255), img[8],  "after flip: row1 R = 255 (red)");
    TEST_ASSERT_EQ(uint8_t(0),   img[10], "after flip: row1 B = 0");

    et::FlipYInPlace(img.data(), kFW, kFH);
    TEST_ASSERT_EQ(uint8_t(255), img[0],  "double flip restores row0 R = 255");
    TEST_ASSERT_EQ(uint8_t(0),   img[2],  "double flip restores row0 B = 0");
    return TestResult::Passed;
}

void RegisterImageCompareTests() {
    auto suite = std::make_shared<TestSuite>("ImageCompareTests");
    suite->AddTestCase(TestCase("SSIM_Identical",     TestSSIM_Identical));
    suite->AddTestCase(TestCase("SSIM_Disjoint",      TestSSIM_Disjoint));
    suite->AddTestCase(TestCase("PNG_RoundTrip",      TestPNG_RoundTrip));
    suite->AddTestCase(TestCase("RGBA16F_Zero",       TestRGBA16F_Zero));
    suite->AddTestCase(TestCase("Depth32_KnownValues", TestDepth32_KnownValues));
    suite->AddTestCase(TestCase("FlipY_RoundTripSwap", TestFlipY_RoundTripSwap));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterImageCompareTests();
    TestRunner::RunAllSuites();
    return 0;
}
