/**
 * @file TestD3D12Format.cpp
 * @brief D3D12 格式映射单测（DataFormat <-> DXGI 双向全覆盖）
 * @details D3D12 M0（F0-6 验收）。逐值断言 ToDxgiFormat / FromDxgiFormat /
 *          GetTypelessDxgiFormat / ToSrgbDxgiFormat；无等价物格式必须显式
 *          Unknown。映射值与 D3D12Abi.h（Microsoft 公开 ABI）锁死。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/D3D12/D3D12Format.h"

using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::d3d12;
using namespace Engine::Test;

// ToDxgiFormat：全部 DataFormat 枚举值逐一断言（含 Unknown 与不支持家族）
TestResult TestToDxgiFormatAllValues() {
    // 8 位
    TEST_ASSERT(ToDxgiFormat(DataFormat::R8_UNorm) == kDXGIFormat_R8_UNorm, "R8_UNorm -> 61");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R8_SNorm) == kDXGIFormat_R8_SNorm, "R8_SNorm -> 63");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R8_UInt) == kDXGIFormat_R8_UInt, "R8_UInt -> 62");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R8_SInt) == kDXGIFormat_R8_SInt, "R8_SInt -> 64");
    // 16 位
    TEST_ASSERT(ToDxgiFormat(DataFormat::R16_UNorm) == kDXGIFormat_R16_UNorm, "R16_UNorm -> 56");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R16_SNorm) == kDXGIFormat_R16_SNorm, "R16_SNorm -> 58");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R16_UInt) == kDXGIFormat_R16_UInt, "R16_UInt -> 57");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R16_SInt) == kDXGIFormat_R16_SInt, "R16_SInt -> 59");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R16_Float) == kDXGIFormat_R16_Float, "R16_Float -> 54");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG8_UNorm) == kDXGIFormat_R8G8_UNorm, "RG8_UNorm -> 49");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG8_SNorm) == kDXGIFormat_R8G8_SNorm, "RG8_SNorm -> 51");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG8_UInt) == kDXGIFormat_R8G8_UInt, "RG8_UInt -> 50");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG8_SInt) == kDXGIFormat_R8G8_SInt, "RG8_SInt -> 52");
    // 24 位：DXGI 无 24-bit 布局 → Unknown（显式拒绝）
    TEST_ASSERT(ToDxgiFormat(DataFormat::R8G8B8_UNorm) == kDXGIFormat_Unknown, "R8G8B8_UNorm unsupported");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R8G8B8_SNorm) == kDXGIFormat_Unknown, "R8G8B8_SNorm unsupported");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R8G8B8_UInt) == kDXGIFormat_Unknown, "R8G8B8_UInt unsupported");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R8G8B8_SInt) == kDXGIFormat_Unknown, "R8G8B8_SInt unsupported");
    // 32 位 R32（UNorm/SNorm 按 Vulkan 先例 fallback FLOAT）
    TEST_ASSERT(ToDxgiFormat(DataFormat::R32_UNorm) == kDXGIFormat_R32_Float, "R32_UNorm fallback FLOAT");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R32_SNorm) == kDXGIFormat_R32_Float, "R32_SNorm fallback FLOAT");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R32_UInt) == kDXGIFormat_R32_UInt, "R32_UInt -> 42");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R32_SInt) == kDXGIFormat_R32_SInt, "R32_SInt -> 43");
    TEST_ASSERT(ToDxgiFormat(DataFormat::R32_Float) == kDXGIFormat_R32_Float, "R32_Float -> 41");
    // RG16
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG16_UNorm) == kDXGIFormat_R16G16_UNorm, "RG16_UNorm -> 35");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG16_SNorm) == kDXGIFormat_R16G16_SNorm, "RG16_SNorm -> 37");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG16_UInt) == kDXGIFormat_R16G16_UInt, "RG16_UInt -> 36");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG16_SInt) == kDXGIFormat_R16G16_SInt, "RG16_SInt -> 38");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG16_Float) == kDXGIFormat_R16G16_Float, "RG16_Float -> 34");
    // RG8B8A8（历史命名 = RGBA8 字节序）
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG8B8A8_UNorm) == kDXGIFormat_R8G8B8A8_UNorm, "RG8B8A8_UNorm -> 28");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG8B8A8_SNorm) == kDXGIFormat_R8G8B8A8_SNorm, "RG8B8A8_SNorm -> 31");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG8B8A8_UInt) == kDXGIFormat_R8G8B8A8_UInt, "RG8B8A8_UInt -> 30");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG8B8A8_SInt) == kDXGIFormat_R8G8B8A8_SInt, "RG8B8A8_SInt -> 32");
    // BGRA8（SNorm/UInt/SInt 无 DXGI 等价）
    TEST_ASSERT(ToDxgiFormat(DataFormat::BGRA8_UNorm) == kDXGIFormat_B8G8R8A8_UNorm, "BGRA8_UNorm -> 87");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BGRA8_SNorm) == kDXGIFormat_Unknown, "BGRA8_SNorm unsupported");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BGRA8_UInt) == kDXGIFormat_Unknown, "BGRA8_UInt unsupported");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BGRA8_SInt) == kDXGIFormat_Unknown, "BGRA8_SInt unsupported");
    // RGBA8 + sRGB
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA8_UNorm) == kDXGIFormat_R8G8B8A8_UNorm, "RGBA8_UNorm -> 28");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA8_SNorm) == kDXGIFormat_R8G8B8A8_SNorm, "RGBA8_SNorm -> 31");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA8_UInt) == kDXGIFormat_R8G8B8A8_UInt, "RGBA8_UInt -> 30");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA8_SInt) == kDXGIFormat_R8G8B8A8_SInt, "RGBA8_SInt -> 32");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA8_sRGB) == kDXGIFormat_R8G8B8A8_UNorm_sRGB, "RGBA8_sRGB -> 29");
    // RG32 / RGB32 / RGBA16 / RGBA32
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG32_UNorm) == kDXGIFormat_R32G32_Float, "RG32_UNorm fallback FLOAT");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG32_UInt) == kDXGIFormat_R32G32_UInt, "RG32_UInt -> 17");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG32_SInt) == kDXGIFormat_R32G32_SInt, "RG32_SInt -> 18");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RG32_Float) == kDXGIFormat_R32G32_Float, "RG32_Float -> 16");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGB32_UNorm) == kDXGIFormat_R32G32B32_Float, "RGB32_UNorm fallback FLOAT");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGB32_UInt) == kDXGIFormat_R32G32B32_UInt, "RGB32_UInt -> 7");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGB32_SInt) == kDXGIFormat_R32G32B32_SInt, "RGB32_SInt -> 8");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGB32_Float) == kDXGIFormat_R32G32B32_Float, "RGB32_Float -> 6");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA16_UNorm) == kDXGIFormat_R16G16B16A16_UNorm, "RGBA16_UNorm -> 11");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA16_SNorm) == kDXGIFormat_R16G16B16A16_SNorm, "RGBA16_SNorm -> 13");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA16_UInt) == kDXGIFormat_R16G16B16A16_UInt, "RGBA16_UInt -> 12");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA16_SInt) == kDXGIFormat_R16G16B16A16_SInt, "RGBA16_SInt -> 14");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA16_Float) == kDXGIFormat_R16G16B16A16_Float, "RGBA16_Float -> 10");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA32_UNorm) == kDXGIFormat_R32G32B32A32_Float, "RGBA32_UNorm fallback FLOAT");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA32_SNorm) == kDXGIFormat_R32G32B32A32_Float, "RGBA32_SNorm fallback FLOAT");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA32_UInt) == kDXGIFormat_R32G32B32A32_UInt, "RGBA32_UInt -> 3");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA32_SInt) == kDXGIFormat_R32G32B32A32_SInt, "RGBA32_SInt -> 4");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA32_Float) == kDXGIFormat_R32G32B32A32_Float, "RGBA32_Float -> 2");
    TEST_ASSERT(ToDxgiFormat(DataFormat::RGBA32_sRGB) == kDXGIFormat_Unknown, "RGBA32_sRGB unsupported");
    // BC 压缩全家
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC1_UNorm) == kDXGIFormat_BC1_UNorm, "BC1_UNorm -> 71");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC1_sRGB) == kDXGIFormat_BC1_UNorm_sRGB, "BC1_sRGB -> 72");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC2_UNorm) == kDXGIFormat_BC2_UNorm, "BC2_UNorm -> 74");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC2_sRGB) == kDXGIFormat_BC2_UNorm_sRGB, "BC2_sRGB -> 75");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC3_UNorm) == kDXGIFormat_BC3_UNorm, "BC3_UNorm -> 77");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC3_sRGB) == kDXGIFormat_BC3_UNorm_sRGB, "BC3_sRGB -> 78");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC4_UNorm) == kDXGIFormat_BC4_UNorm, "BC4_UNorm -> 80");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC4_SNorm) == kDXGIFormat_BC4_SNorm, "BC4_SNorm -> 81");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC5_UNorm) == kDXGIFormat_BC5_UNorm, "BC5_UNorm -> 83");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC5_SNorm) == kDXGIFormat_BC5_SNorm, "BC5_SNorm -> 84");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC6H_UF16) == kDXGIFormat_BC6H_UF16, "BC6H_UF16 -> 95");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC6H_SF16) == kDXGIFormat_BC6H_SF16, "BC6H_SF16 -> 96");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC7_UNorm) == kDXGIFormat_BC7_UNorm, "BC7_UNorm -> 98");
    TEST_ASSERT(ToDxgiFormat(DataFormat::BC7_sRGB) == kDXGIFormat_BC7_UNorm_sRGB, "BC7_sRGB -> 99");
    // 深度模板
    TEST_ASSERT(ToDxgiFormat(DataFormat::D16_UNorm) == kDXGIFormat_D16_UNorm, "D16_UNorm -> 55");
    TEST_ASSERT(ToDxgiFormat(DataFormat::D24_UNorm_S8_UInt) == kDXGIFormat_D24_UNorm_S8_UInt, "D24S8 -> 45");
    TEST_ASSERT(ToDxgiFormat(DataFormat::D32_Float) == kDXGIFormat_D32_Float, "D32_Float -> 40");
    TEST_ASSERT(ToDxgiFormat(DataFormat::D32_Float_S8X24_UInt) == kDXGIFormat_D32_Float_S8X24_UInt, "D32S8X24 -> 20");
    // ASTC：D3D12 原生无 → Unknown（跨端缺口登记）
    TEST_ASSERT(ToDxgiFormat(DataFormat::ASTC_4x4_UNorm) == kDXGIFormat_Unknown, "ASTC_4x4_UNorm unsupported");
    TEST_ASSERT(ToDxgiFormat(DataFormat::ASTC_4x4_sRGB) == kDXGIFormat_Unknown, "ASTC_4x4_sRGB unsupported");
    TEST_ASSERT(ToDxgiFormat(DataFormat::ASTC_6x6_UNorm) == kDXGIFormat_Unknown, "ASTC_6x6_UNorm unsupported");
    TEST_ASSERT(ToDxgiFormat(DataFormat::ASTC_6x6_sRGB) == kDXGIFormat_Unknown, "ASTC_6x6_sRGB unsupported");
    TEST_ASSERT(ToDxgiFormat(DataFormat::ASTC_8x8_UNorm) == kDXGIFormat_Unknown, "ASTC_8x8_UNorm unsupported");
    TEST_ASSERT(ToDxgiFormat(DataFormat::ASTC_8x8_sRGB) == kDXGIFormat_Unknown, "ASTC_8x8_sRGB unsupported");
    // Unknown -> Unknown
    TEST_ASSERT(ToDxgiFormat(DataFormat::Unknown) == kDXGIFormat_Unknown, "Unknown -> 0");
    return TestResult::Passed;
}

TestResult TestFromDxgiFormatRoundTrip() {
    // 已映射值必须反向回规范代表元
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_R8G8B8A8_UNorm) == DataFormat::RGBA8_UNorm, "28 -> RGBA8_UNorm (canonical)");
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_R8G8B8A8_UNorm_sRGB) == DataFormat::RGBA8_sRGB, "29 -> RGBA8_sRGB");
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_B8G8R8A8_UNorm) == DataFormat::BGRA8_UNorm, "87 -> BGRA8_UNorm");
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_R16G16B16A16_Float) == DataFormat::RGBA16_Float, "10 -> RGBA16_Float");
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_R32G32B32A32_Float) == DataFormat::RGBA32_Float, "2 -> RGBA32_Float");
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_D32_Float) == DataFormat::D32_Float, "40 -> D32_Float");
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_D24_UNorm_S8_UInt) == DataFormat::D24_UNorm_S8_UInt, "45 -> D24S8");
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_BC7_UNorm) == DataFormat::BC7_UNorm, "98 -> BC7_UNorm");
    // 往返一致：全部有映射的格式 To->From 后回到自身（或规范代表元）
    for (u16 v = 0; v <= 85; ++v) {
        const DataFormat fmt = static_cast<DataFormat>(v);
        const u32 dxgi = ToDxgiFormat(fmt);
        if (dxgi == kDXGIFormat_Unknown) continue;
        const DataFormat back = FromDxgiFormat(dxgi);
        // 允许 RG8B8A8_* 别名与 R32/RG32/RGB32/RGBA32 的 UNorm/SNorm fallback
        // 回到 Float 变体；其余必须精确往返。
        const bool aliasOk = back == fmt
            || (back == DataFormat::RGBA8_UNorm && fmt == DataFormat::RG8B8A8_UNorm)
            || (back == DataFormat::RGBA8_SNorm && fmt == DataFormat::RG8B8A8_SNorm)
            || (back == DataFormat::RGBA8_UInt && fmt == DataFormat::RG8B8A8_UInt)
            || (back == DataFormat::RGBA8_SInt && fmt == DataFormat::RG8B8A8_SInt)
            || (back == DataFormat::R32_Float && fmt == DataFormat::R32_UNorm)
            || (back == DataFormat::R32_Float && fmt == DataFormat::R32_SNorm)
            || (back == DataFormat::RG32_Float && fmt == DataFormat::RG32_UNorm)
            || (back == DataFormat::RG32_Float && fmt == DataFormat::RG32_SNorm)
            || (back == DataFormat::RGB32_Float && fmt == DataFormat::RGB32_UNorm)
            || (back == DataFormat::RGB32_Float && fmt == DataFormat::RGB32_SNorm)
            || (back == DataFormat::RGBA32_Float && fmt == DataFormat::RGBA32_UNorm)
            || (back == DataFormat::RGBA32_Float && fmt == DataFormat::RGBA32_SNorm)
            || (back == DataFormat::BGRA8_UNorm && fmt == DataFormat::BGRA8_UNorm);
        if (!aliasOk) {
            TEST_ASSERT(false, "round trip mismatch");
            return TestResult::Failed;
        }
    }
    // TYPELESS / 未映射 / RHI 无对应枚举的 sRGB 变体 -> Unknown
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_R8G8B8A8_Typeless) == DataFormat::Unknown, "TYPELESS -> Unknown");
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_R16G16B16A16_Typeless) == DataFormat::Unknown, "TYPELESS16 -> Unknown");
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_B8G8R8A8_UNorm_sRGB) == DataFormat::Unknown, "BGRA sRGB -> Unknown (no RHI enum)");
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_R11G11B10_Float) == DataFormat::Unknown, "R11G11B10 -> Unknown");
    TEST_ASSERT(FromDxgiFormat(103 /*NV12*/) == DataFormat::Unknown, "NV12 -> Unknown");
    TEST_ASSERT(FromDxgiFormat(kDXGIFormat_Unknown) == DataFormat::Unknown, "0 -> Unknown");
    return TestResult::Passed;
}

TestResult TestTypelessAndSrgbHelpers() {
    // TYPELESS 对应
    TEST_ASSERT(GetTypelessDxgiFormat(DataFormat::RGBA8_UNorm) == kDXGIFormat_R8G8B8A8_Typeless, "RGBA8 -> 27 typeless");
    TEST_ASSERT(GetTypelessDxgiFormat(DataFormat::RGBA8_sRGB) == kDXGIFormat_R8G8B8A8_Typeless, "RGBA8_sRGB same typeless parent");
    TEST_ASSERT(GetTypelessDxgiFormat(DataFormat::BGRA8_UNorm) == kDXGIFormat_B8G8R8A8_Typeless, "BGRA8 -> 90 typeless");
    TEST_ASSERT(GetTypelessDxgiFormat(DataFormat::D32_Float) == kDXGIFormat_R32_Typeless, "D32 -> 39 typeless");
    TEST_ASSERT(GetTypelessDxgiFormat(DataFormat::D24_UNorm_S8_UInt) == kDXGIFormat_R24G8_Typeless, "D24S8 -> 44 typeless");
    TEST_ASSERT(GetTypelessDxgiFormat(DataFormat::RGBA16_Float) == kDXGIFormat_R16G16B16A16_Typeless, "RGBA16F -> 9");
    TEST_ASSERT(GetTypelessDxgiFormat(DataFormat::BC7_UNorm) == kDXGIFormat_BC7_Typeless, "BC7 -> 97");
    TEST_ASSERT(GetTypelessDxgiFormat(DataFormat::ASTC_4x4_UNorm) == kDXGIFormat_Unknown, "ASTC no typeless parent");
    TEST_ASSERT(GetTypelessDxgiFormat(DataFormat::R8G8B8_UNorm) == kDXGIFormat_Unknown, "24-bit no typeless parent");
    // sRGB 家族
    TEST_ASSERT(ToSrgbDxgiFormat(DataFormat::RGBA8_UNorm) == kDXGIFormat_R8G8B8A8_UNorm_sRGB, "RGBA8 -> 29 sRGB");
    TEST_ASSERT(ToSrgbDxgiFormat(DataFormat::BGRA8_UNorm) == kDXGIFormat_B8G8R8A8_UNorm_sRGB, "BGRA8 -> 91 sRGB (DXGI 有变体)");
    TEST_ASSERT(ToSrgbDxgiFormat(DataFormat::BC1_UNorm) == kDXGIFormat_BC1_UNorm_sRGB, "BC1 -> 72");
    TEST_ASSERT(ToSrgbDxgiFormat(DataFormat::RGBA16_Float) == kDXGIFormat_Unknown, "RGBA16F no sRGB variant");
    TEST_ASSERT(IsSrgbDxgiFormat(kDXGIFormat_R8G8B8A8_UNorm_sRGB), "29 is sRGB");
    TEST_ASSERT(IsSrgbDxgiFormat(kDXGIFormat_BC7_UNorm_sRGB), "99 is sRGB");
    TEST_ASSERT(!IsSrgbDxgiFormat(kDXGIFormat_R8G8B8A8_UNorm), "28 not sRGB");
    TEST_ASSERT(IsSrgbDataFormat(DataFormat::RGBA8_sRGB), "RHI enum sRGB");
    TEST_ASSERT(!IsSrgbDataFormat(DataFormat::RGBA8_UNorm), "RHI enum not sRGB");
    return TestResult::Passed;
}

void RegisterD3D12FormatTests() {
    auto suite = std::make_shared<TestSuite>("D3D12FormatTests");
    suite->AddTestCase(TestCase("ToDxgiFormatAllValues", TestToDxgiFormatAllValues));
    suite->AddTestCase(TestCase("FromDxgiFormatRoundTrip", TestFromDxgiFormatRoundTrip));
    suite->AddTestCase(TestCase("TypelessAndSrgbHelpers", TestTypelessAndSrgbHelpers));
    TestRunner::RegisterTestSuite(suite);
}

#ifndef UNIT_TEST_LIB
int main() {
    RegisterD3D12FormatTests();
    TestRunner::RunAllSuites();
    return 0;
}
#endif
