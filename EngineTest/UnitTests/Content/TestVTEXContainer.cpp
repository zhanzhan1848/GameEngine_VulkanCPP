/**
 * @file TestVTEXContainer.cpp
 * @brief VT M1 — VTBaker 烘焙 + VTEX 容器 roundtrip / 校验 / 质量测试
 * @details 覆盖 tasks 1.7 / 1.10:
 *   1. BakeAndParse:烘焙 → vtex_asset 解析 → header/页索引/段访问校验
 *   2. BC1DecodeQuality:页 BC1 数据软解码,与源图 PSNR ≥ 22dB
 *   3. ZlibRoundtrip:zlib tile 解压尺寸一致
 *   4. NonSquare:256x128 非正方形页计数
 *   5. ValidationRejects:坏 magic / 截断数据拒绝
 *   6. SponzaSubset:三张代表纹理(diffuse/normal/roughness)实烘焙
 */

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "../TestFramework.h"
#include "VTBaker.h"
#include "Engine/Content/VTEXAsset.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <zlib.h>

using namespace primal::content;
using namespace primal::content::vt;
using namespace Engine::Test;

namespace {

std::vector<uint8_t> make_image(uint32_t w, uint32_t h, uint32_t pattern) {
    std::vector<uint8_t> img(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &img[(size_t(y) * w + x) * 4];
            if (pattern == 0) {  // 平滑渐变(压缩友好)
                p[0] = uint8_t(x * 255 / (w - 1));
                p[1] = uint8_t(y * 255 / (h - 1));
                p[2] = uint8_t((x + y) * 255 / (w + h - 2));
                p[3] = 255;
            } else {              // 细节图案
                p[0] = uint8_t((x * 7 + y * 13) & 0xFF);
                p[1] = uint8_t((x * 5 ^ y * 3) & 0xFF);
                p[2] = uint8_t((x * 3 + y * 11) & 0xFF);
                p[3] = 255;
            }
        }
    return img;
}

// 测试侧 BC1 软解码(仅验证用)
void decode_bc1_block(const uint8_t in[8], uint8_t out[16 * 3]) {
    uint16_t c0 = uint16_t(in[0] | (in[1] << 8));
    uint16_t c1 = uint16_t(in[2] | (in[3] << 8));
    uint8_t p0[3], p1[3];
    auto unpack = [](uint16_t p, uint8_t o[3]) {
        o[0] = uint8_t(((p >> 11) & 0x1F) * 255 / 31);
        o[1] = uint8_t(((p >> 5) & 0x3F) * 255 / 63);
        o[2] = uint8_t((p & 0x1F) * 255 / 31);
    };
    unpack(c0, p0); unpack(c1, p1);
    uint8_t pal[4][3];
    for (int c = 0; c < 3; ++c) {
        pal[0][c] = p0[c];
        pal[1][c] = p1[c];
        pal[2][c] = uint8_t((uint32_t(p0[c]) * 2 + p1[c]) / 3);
        pal[3][c] = uint8_t((uint32_t(p0[c]) + p1[c] * 2) / 3);
    }
    uint32_t indices = uint32_t(in[4]) | (uint32_t(in[5]) << 8) |
                       (uint32_t(in[6]) << 16) | (uint32_t(in[7]) << 24);
    for (int i = 0; i < 16; ++i)
        std::memcpy(out + i * 3, pal[(indices >> (2 * i)) & 3], 3);
}

} // anonymous namespace

TestResult TestVTEX_BakeAndParse() {
    const uint32_t W = 256, H = 256;
    std::vector<uint8_t> src = make_image(W, H, 0);
    bake_config cfg;   // color,BC1+ASTC,tile 128,border 4
    std::vector<uint8_t> vt_file;
    bake_stats stats;
    TEST_ASSERT(bake(src.data(), W, H, cfg, vt_file, &stats), "bake 256x256");

    // 256→128 两级 mip;页:mip0 2x2=4,mip1 1x1=1
    TEST_ASSERT_EQ(2u, stats.mip_count, "mip count");
    TEST_ASSERT_EQ(5u, stats.page_count, "page count");

    vtex_asset asset;
    TEST_ASSERT(asset.initialize(vt_file.data(), vt_file.size()), "parse container");
    TEST_ASSERT_EQ(2u, asset.header().mip_count, "header mip count");
    TEST_ASSERT_EQ(5u, asset.header().page_count, "header page count");
    TEST_ASSERT_EQ(128u, asset.header().tile_size, "tile size");
    TEST_ASSERT_EQ(4u, asset.header().border, "border");
    TEST_ASSERT_EQ(bcn_format::BC1_UNorm, asset.header().bcn_format, "bcn format");
    TEST_ASSERT_EQ(astc_format::ASTC_4x4_UNorm, asset.header().astc_format, "astc format");
    TEST_ASSERT((asset.header().avg_color & 0xFF000000u) == 0xFF000000u, "avg opaque");

    // 页索引 mip 升序 + 段数据可访问
    uint32_t last_mip = 0;
    for (uint32_t i = 0; i < asset.page_count(); ++i) {
        const vtex_page_index* p = asset.page(i);
        TEST_ASSERT(p != nullptr, "page entry");
        TEST_ASSERT(p->mip >= last_mip, "mip ascending");
        last_mip = p->mip;
        uint64_t sz = 0;
        TEST_ASSERT(asset.tile_data(*p, false, sz) != nullptr && sz > 0, "bcn tile data");
        // BC1 tile:34x34 block × 8B
        TEST_ASSERT_EQ(uint64_t(34 * 34 * 8), sz, "bcn tile size");
        TEST_ASSERT(asset.tile_data(*p, true, sz) != nullptr && sz > 0, "astc tile data");
        TEST_ASSERT_EQ(uint64_t(34 * 34 * 16), sz, "astc tile size");
    }
    return TestResult::Passed;
}

TestResult TestVTEX_BC1DecodeQuality() {
    const uint32_t W = 256, H = 256;
    std::vector<uint8_t> src = make_image(W, H, 0);   // 渐变
    bake_config cfg;
    cfg.use_astc = false;                             // 只验证 BC 段
    std::vector<uint8_t> vt_file;
    TEST_ASSERT(bake(src.data(), W, H, cfg, vt_file), "bake bc-only");

    vtex_asset asset;
    TEST_ASSERT(asset.initialize(vt_file.data(), vt_file.size()), "parse");

    // 解码 mip0 四页的 interior(128x128)与源图比 PSNR
    double sse = 0.0;
    uint32_t page = 0;
    for (uint32_t py = 0; py < 2; ++py)
        for (uint32_t px = 0; px < 2; ++px, ++page) {
            const vtex_page_index* p = asset.page(page);
            uint64_t sz = 0;
            const uint8_t* data = asset.tile_data(*p, false, sz);
            TEST_ASSERT(data != nullptr, "tile data");
            // 解码整 tile(136x136,含 border),取 interior
            const uint32_t phys = 136;
            std::vector<uint8_t> decoded(size_t(phys) * phys * 3);
            for (uint32_t by = 0; by < phys / 4; ++by)
                for (uint32_t bx = 0; bx < phys / 4; ++bx) {
                    uint8_t rgb[16 * 3];
                    decode_bc1_block(data + (size_t(by) * (phys / 4) + bx) * 8, rgb);
                    for (uint32_t y = 0; y < 4; ++y)
                        for (uint32_t x = 0; x < 4; ++x)
                            std::memcpy(&decoded[(size_t(by * 4 + y) * phys + bx * 4 + x) * 3],
                                        rgb + (y * 4 + x) * 3, 3);
                }
            for (uint32_t y = 0; y < 128; ++y)
                for (uint32_t x = 0; x < 128; ++x) {
                    const uint8_t* d = &decoded[(size_t(4 + y) * phys + 4 + x) * 3];
                    const uint8_t* s = &src[(size_t(py * 128 + y) * W + px * 128 + x) * 4];
                    for (int c = 0; c < 3; ++c) {
                        double diff = double(d[c]) - double(s[c]);
                        sse += diff * diff;
                    }
                }
        }
    double mse = sse / (double(W) * H * 3);
    double psnr = 10.0 * std::log10(255.0 * 255.0 / mse);
    std::cout << "[VTEX] BC1 渐变图 PSNR = " << psnr << " dB" << std::endl;
    TEST_ASSERT(psnr >= 22.0, "BC1 PSNR >= 22dB");
    return TestResult::Passed;
}

TestResult TestVTEX_ZlibRoundtrip() {
    const uint32_t W = 256, H = 256;
    std::vector<uint8_t> src = make_image(W, H, 1);
    bake_config cfg;
    cfg.zlib_tiles = true;
    cfg.use_astc = false;
    std::vector<uint8_t> vt_file;
    TEST_ASSERT(bake(src.data(), W, H, cfg, vt_file), "bake zlib");
    vtex_asset asset;
    TEST_ASSERT(asset.initialize(vt_file.data(), vt_file.size()), "parse");
    TEST_ASSERT(asset.uses_zlib(), "zlib flag");

    // 解压第 0 页 BC 数据 → 尺寸 = 未压缩 BC1 tile 尺寸
    const vtex_page_index* p = asset.page(0);
    uint64_t compSize = 0;
    const uint8_t* comp = asset.tile_data(*p, false, compSize);
    TEST_ASSERT(comp != nullptr && compSize > 0, "compressed tile");
    std::vector<uint8_t> plain(34 * 34 * 8);
    uLongf plainLen = uLongf(plain.size());
    TEST_ASSERT(uncompress(plain.data(), &plainLen, comp, uLong(compSize)) == Z_OK,
                "uncompress");
    TEST_ASSERT_EQ(uLongf(34 * 34 * 8), plainLen, "decompressed size");
    return TestResult::Passed;
}

TestResult TestVTEX_NonSquare() {
    // 256x128:mip0 2x1 页,mip1 1x1 页(短边到 tile)
    std::vector<uint8_t> src = make_image(256, 128, 0);
    bake_config cfg;
    cfg.use_astc = false;
    std::vector<uint8_t> vt_file;
    bake_stats stats;
    TEST_ASSERT(bake(src.data(), 256, 128, cfg, vt_file, &stats), "bake non-square");
    TEST_ASSERT_EQ(2u, stats.mip_count, "mips");
    TEST_ASSERT_EQ(3u, stats.page_count, "pages(2+1)");
    vtex_asset asset;
    TEST_ASSERT(asset.initialize(vt_file.data(), vt_file.size()), "parse");
    return TestResult::Passed;
}

TestResult TestVTEX_ValidationRejects() {
    std::vector<uint8_t> src = make_image(256, 256, 0);
    bake_config cfg;
    std::vector<uint8_t> vt_file;
    TEST_ASSERT(bake(src.data(), 256, 256, cfg, vt_file), "bake");

    vtex_asset asset;
    TEST_ASSERT(!asset.initialize(vt_file.data(), 16), "truncated header rejected");
    std::vector<uint8_t> bad = vt_file;
    bad[0] = 'X';
    TEST_ASSERT(!asset.initialize(bad.data(), bad.size()), "bad magic rejected");
    std::vector<uint8_t> trunc(vt_file.begin(), vt_file.begin() + vt_file.size() / 2);
    TEST_ASSERT(!asset.initialize(trunc.data(), trunc.size()), "truncated body rejected");
    return TestResult::Passed;
}

TestResult TestVTEX_SponzaSubset() {
    // assets 相对本测试二进制:build/Tests/UnitTests → 仓库根
    const char* rootRel = "../../../EngineTest/assets/models/Sponza/";
    const char* files[3] = {
        "Sponza_Arch_diffuse.png", "Sponza_Arch_normal.png",
        "Sponza_Arch_roughness.png"};
    for (int i = 0; i < 3; ++i) {
        std::string path = std::string(rootRel) + files[i];
        int w = 0, h = 0, comp = 0;
        stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
        if (!pixels) {
            std::cout << "[VTEX] 跳过 " << files[i] << "(找不到资产)" << std::endl;
            continue;
        }
        bake_config cfg;
        cfg.kind = (i == 1) ? channel_kind::normal :
                   (i == 2) ? channel_kind::scalar : channel_kind::color;
        cfg.use_astc = false;   // 子集保持快速;ASTC 已在其余用例覆盖
        std::vector<uint8_t> vt_file;
        bake_stats stats;
        TEST_ASSERT(bake(pixels, uint32_t(w), uint32_t(h), cfg, vt_file, &stats),
                    "bake sponza tile");
        stbi_image_free(pixels);
        vtex_asset asset;
        TEST_ASSERT(asset.initialize(vt_file.data(), vt_file.size()), "parse sponza tile");
        std::cout << "[VTEX] " << files[i] << ": mips=" << stats.mip_count
                  << " pages=" << stats.page_count << " bcn=" << stats.bcn_bytes
                  << "B total=" << stats.total_bytes << "B" << std::endl;
    }
    return TestResult::Passed;
}

TestResult TestVTEX_BakeLarge8K() {
    // 1.10:8K 烘焙可行性(BC-only 保时;ASTC 路径已在其余用例覆盖)
    const uint32_t W = 8192, H = 8192;
    std::vector<uint8_t> src(size_t(W) * H * 4);
    for (uint32_t x = 0; x < W; ++x) {   // 填一行,其余行按行复制(内容不影响管线校验)
        uint8_t* p = &src[size_t(x) * 4];
        p[0] = uint8_t(x & 0xFF); p[1] = uint8_t((x >> 8) & 0xFF);
        p[2] = uint8_t((x >> 3) & 0xFF); p[3] = 255;
    }
    for (uint32_t y = 1; y < H; ++y)
        std::memcpy(&src[(size_t(y) * W) * 4], &src[0], size_t(W) * 4);
    bake_config cfg;
    cfg.use_astc = false;
    std::vector<uint8_t> vt_file;
    bake_stats stats;
    TEST_ASSERT(bake(src.data(), W, H, cfg, vt_file, &stats), "bake 8K");
    // 8192→128 共 7 级 mip;页数 64²+32²+...+1
    TEST_ASSERT_EQ(7u, stats.mip_count, "8K mips");
    uint32_t expectPages = 0;
    for (uint32_t side = 8192; side >= 128; side /= 2) expectPages += (side / 128) * (side / 128);
    TEST_ASSERT_EQ(expectPages, stats.page_count, "8K pages");
    std::cout << "[VTEX] 8K bake: total=" << stats.total_bytes << "B (源 "
              << size_t(W) * H * 4 << "B)" << std::endl;
    vtex_asset asset;
    TEST_ASSERT(asset.initialize(vt_file.data(), vt_file.size()), "parse 8K");
    return TestResult::Passed;
}

void RegisterVTEXContainerTests() {
    auto suite = std::make_shared<TestSuite>("VTEXContainerTests");
    suite->AddTestCase(TestCase("BakeAndParse", TestVTEX_BakeAndParse));
    suite->AddTestCase(TestCase("BC1DecodeQuality", TestVTEX_BC1DecodeQuality));
    suite->AddTestCase(TestCase("ZlibRoundtrip", TestVTEX_ZlibRoundtrip));
    suite->AddTestCase(TestCase("NonSquare", TestVTEX_NonSquare));
    suite->AddTestCase(TestCase("ValidationRejects", TestVTEX_ValidationRejects));
    suite->AddTestCase(TestCase("SponzaSubset", TestVTEX_SponzaSubset));
    suite->AddTestCase(TestCase("BakeLarge8K", TestVTEX_BakeLarge8K));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVTEXContainerTests();
    TestRunner::RunAllSuites();
    return 0;
}
