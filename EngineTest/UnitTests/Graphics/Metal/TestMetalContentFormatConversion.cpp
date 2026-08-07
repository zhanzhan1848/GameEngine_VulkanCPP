/**
 * @file TestMetalContentFormatConversion.cpp
 * @brief Regression test for the PNG import crash.
 *
 * Bug: importing a PNG in the editor crashed with
 *   MTLDebugValidateMTLPixelFormat:1622: failed assertion
 *   `pixelFormat (29) is not a valid MTLPixelFormat.'
 *
 * Root cause: Engine/Graphics/Metal/MetalContent.cpp:431 cast the raw
 * `u32` read from the texture blob straight to MTL::PixelFormat. The
 * importer writes DXGI-style format codes (28 = RGBA8 UNORM, 29 = RGBA8
 * sRGB, 71/72 = BC1, 98/99 = BC7), and several of those integers
 * (notably 29) sit in the gaps of Apple's MTL::PixelFormat enum, so
 * `setPixelFormat` asserted on Metal validation layer enabled builds.
 *
 * This test pins the conversion table added to MetalContent.cpp so the
 * legacy texture path stops crashing and so future DXGI additions can't
 * regress.
 */

#include "../../TestFramework.h"
#include "Engine/Graphics/Metal/MetalContent.h"

using namespace Engine::Test;

namespace
{
    // Known-valid MTL::PixelFormat values (from OSAPI/MAC/Metal/MTLPixelFormat.hpp).
    // Using the actual enum values guarantees we catch any silent rename.
    constexpr MTL::PixelFormat kRGBA8Unorm     = MTL::PixelFormatRGBA8Unorm;      // 70
    constexpr MTL::PixelFormat kRGBA8UnormSRGB = MTL::PixelFormatRGBA8Unorm_sRGB; // 71
    constexpr MTL::PixelFormat kBC1RGBA        = MTL::PixelFormatBC1_RGBA;        // 130
    constexpr MTL::PixelFormat kBC1RGBASRGB    = MTL::PixelFormatBC1_RGBA_sRGB;   // 131
    constexpr MTL::PixelFormat kBC7RGBAUnorm   = MTL::PixelFormatBC7_RGBAUnorm;   // 152
    constexpr MTL::PixelFormat kBC7RGBAUnormSRGB = MTL::PixelFormatBC7_RGBAUnorm_sRGB; // 153
    constexpr MTL::PixelFormat kInvalid        = MTL::PixelFormatInvalid;        // 0
}

TestResult TestDXGIRGBA8Unorm() {
    // DXGI_FORMAT_R8G8B8A8_UNORM = 28 → MTL::PixelFormatRGBA8Unorm
    const MTL::PixelFormat result = primal::graphics::metal::content::mtl_pixel_format_from_dxgi(28u);
    TEST_ASSERT_EQ(kRGBA8Unorm, result, "DXGI 28 (RGBA8 UNORM) must map to MTL::PixelFormatRGBA8Unorm");
    TEST_ASSERT(result != kInvalid, "DXGI 28 must NOT resolve to PixelFormatInvalid");
    return TestResult::Passed;
}

TestResult TestDXGIRGBA8SRGB() {
    // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29 → MTL::PixelFormatRGBA8Unorm_sRGB
    // This is the exact value that crashed the editor on PNG import.
    const MTL::PixelFormat result = primal::graphics::metal::content::mtl_pixel_format_from_dxgi(29u);
    TEST_ASSERT_EQ(kRGBA8UnormSRGB, result,
        "DXGI 29 (RGBA8 sRGB) must map to MTL::PixelFormatRGBA8Unorm_sRGB — "
        "this is the value that crashed the editor before the conversion was added");
    TEST_ASSERT(result != kInvalid, "DXGI 29 must NOT resolve to PixelFormatInvalid (this was the crash)");
    return TestResult::Passed;
}

TestResult TestDXGIBc1Unorm() {
    // DXGI_FORMAT_BC1_UNORM = 71 → MTL::PixelFormatBC1_RGBA
    const MTL::PixelFormat result = primal::graphics::metal::content::mtl_pixel_format_from_dxgi(71u);
    TEST_ASSERT_EQ(kBC1RGBA, result, "DXGI 71 (BC1 UNORM) must map to MTL::PixelFormatBC1_RGBA");
    TEST_ASSERT(result != kInvalid, "DXGI 71 must NOT resolve to PixelFormatInvalid");
    return TestResult::Passed;
}

TestResult TestDXGIBc1Srgb() {
    // DXGI_FORMAT_BC1_UNORM_SRGB = 72 → MTL::PixelFormatBC1_RGBA_sRGB
    const MTL::PixelFormat result = primal::graphics::metal::content::mtl_pixel_format_from_dxgi(72u);
    TEST_ASSERT_EQ(kBC1RGBASRGB, result, "DXGI 72 (BC1 sRGB) must map to MTL::PixelFormatBC1_RGBA_sRGB");
    TEST_ASSERT(result != kInvalid, "DXGI 72 must NOT resolve to PixelFormatInvalid");
    return TestResult::Passed;
}

TestResult TestDXGIBc7Unorm() {
    // DXGI_FORMAT_BC7_UNORM = 98 → MTL::PixelFormatBC7_RGBAUnorm
    const MTL::PixelFormat result = primal::graphics::metal::content::mtl_pixel_format_from_dxgi(98u);
    TEST_ASSERT_EQ(kBC7RGBAUnorm, result, "DXGI 98 (BC7 UNORM) must map to MTL::PixelFormatBC7_RGBAUnorm");
    TEST_ASSERT(result != kInvalid, "DXGI 98 must NOT resolve to PixelFormatInvalid");
    return TestResult::Passed;
}

TestResult TestDXGIBc7Srgb() {
    // DXGI_FORMAT_BC7_UNORM_SRGB = 99 → MTL::PixelFormatBC7_RGBAUnorm_sRGB
    const MTL::PixelFormat result = primal::graphics::metal::content::mtl_pixel_format_from_dxgi(99u);
    TEST_ASSERT_EQ(kBC7RGBAUnormSRGB, result, "DXGI 99 (BC7 sRGB) must map to MTL::PixelFormatBC7_RGBAUnorm_sRGB");
    TEST_ASSERT(result != kInvalid, "DXGI 99 must NOT resolve to PixelFormatInvalid");
    return TestResult::Passed;
}

TestResult TestDXGIFallbackIsValid() {
    // Unknown DXGI codes must fall back to a valid Metal format,
    // never to PixelFormatInvalid — that's what crashed the editor.
    const MTL::PixelFormat fallback = primal::graphics::metal::content::mtl_pixel_format_from_dxgi(0xDEADBEEFu);
    TEST_ASSERT(fallback != kInvalid,
        "Unknown DXGI codes must fall back to a valid MTL::PixelFormat, not PixelFormatInvalid "
        "(which would crash Metal validation layer)");
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("MetalContentFormatConversionTests");
    suite->AddTestCase(TestCase("DXGI28_RGBA8_UNorm",   TestDXGIRGBA8Unorm));
    suite->AddTestCase(TestCase("DXGI29_RGBA8_sRGB",    TestDXGIRGBA8SRGB));
    suite->AddTestCase(TestCase("DXGI71_BC1_UNorm",     TestDXGIBc1Unorm));
    suite->AddTestCase(TestCase("DXGI72_BC1_sRGB",      TestDXGIBc1Srgb));
    suite->AddTestCase(TestCase("DXGI98_BC7_UNorm",     TestDXGIBc7Unorm));
    suite->AddTestCase(TestCase("DXGI99_BC7_sRGB",      TestDXGIBc7Srgb));
    suite->AddTestCase(TestCase("Unknown_FallbackSafe", TestDXGIFallbackIsValid));

    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    return 0;
}