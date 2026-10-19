/**
 * @file TestLightmapBaking.cpp
 * @brief Lightmap M1 契约单测：MaxRects 装箱 + LMAP 容器往返（无 GPU 依赖）
 *
 * 覆盖：
 *   1. 装箱：无重叠、全在界内、确定性（同输入两次同输出）、利用率下限、
 *      装不下的正确标记 placed=false；
 *   2. LMAP：往返逐字段一致、坏 magic/截断拒绝、非 RGBA16F 拒写；
 *   3. 契约尺寸锁（static_assert 在头内，这里运行期复核一次）。
 */
#include "Engine/Graphics/Baking/LightmapAtlasPacker.h"
#include "Engine/Graphics/Baking/LightmapBaker.h"
#include "Engine/Graphics/Baking/LightmapContainer.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace primal::graphics::baking;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cerr << "[FAIL] " << msg << " (line " << __LINE__ << ")"   \
                      << std::endl;                                         \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

std::vector<PackRequest> SyntheticRequests(std::uint32_t seed, std::uint32_t count) {
    // xorshift 确定性伪随机——同 seed 同序列。
    std::uint32_t s = seed ? seed : 1;
    std::vector<PackRequest> reqs;
    for (std::uint32_t i = 0; i < count; ++i) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const std::uint32_t w = 16 + (s % 128);
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        const std::uint32_t h = 16 + (s % 128);
        reqs.push_back({i, w, h});
    }
    return reqs;
}

void TestPackerBasics() {
    MaxRectsPacker packer(1024);
    auto reqs = SyntheticRequests(7, 64);
    auto results = packer.Pack(reqs);

    CHECK(results.size() == reqs.size(), "result count");
    std::uint32_t placed = 0;
    std::uint64_t placedArea = 0;
    std::vector<PackResult> placedRects;
    for (const auto& r : results) {
        if (!r.placed) continue;
        ++placed;
        CHECK(r.x + r.w <= 1024 && r.y + r.h <= 1024, "in-bounds");
        placedArea += (std::uint64_t)r.w * r.h;
        placedRects.push_back(r);
    }
    CHECK(placed >= 60, "most synthetic rects should place");
    // 利用率内部一致性：Utilization() 必须 == 已放置面积 / bin²。绝对下限
    // 受合成负载总面积约束（~0.41 上限），只查放置充分性。
    const double expectUtil = (double)placedArea / (1024.0 * 1024.0);
    CHECK(std::abs(packer.Utilization() - (float)expectUtil) < 1e-3,
          "utilization internal consistency");
    CHECK(packer.Utilization() > 0.3f, "utilization sanity floor");

    for (size_t i = 0; i < placedRects.size(); ++i)
        for (size_t j = i + 1; j < placedRects.size(); ++j) {
            const auto& a = placedRects[i];
            const auto& b = placedRects[j];
            const bool overlap = a.x < b.x + b.w && b.x < a.x + a.w &&
                                 a.y < b.y + b.h && b.y < a.y + a.h;
            CHECK(!overlap, "no overlap between placed rects");
        }
}

void TestPackerDeterministic() {
    const auto reqs = SyntheticRequests(42, 48);
    MaxRectsPacker p1(512), p2(512);
    const auto r1 = p1.Pack(reqs);
    const auto r2 = p2.Pack(reqs);
    CHECK(r1.size() == r2.size(), "deterministic size");
    bool identical = true;
    for (size_t i = 0; i < r1.size(); ++i)
        identical = identical && std::memcmp(&r1[i], &r2[i], sizeof(PackResult)) == 0;
    CHECK(identical, "deterministic layout");

    // 输入顺序无关性（id 对应结果一致）：逆序喂入，按 id 比对。
    std::vector<PackRequest> reversed(reqs.rbegin(), reqs.rend());
    MaxRectsPacker p3(512);
    const auto r3 = p3.Pack(reversed);
    for (const auto& a : r1) {
        const PackResult* match = nullptr;
        for (const auto& b : r3)
            if (b.id == a.id) { match = &b; break; }
        CHECK(match != nullptr, "id present in reversed pack");
        if (match)
            CHECK(match->placed == a.placed && match->x == a.x && match->y == a.y,
                  "order-independent placement for same id");
    }
}

void TestPackerOversize() {
    MaxRectsPacker packer(256);
    const std::vector<PackRequest> reqs = {{0, 512, 512}, {1, 0, 0}, {2, 100, 100}};
    const auto results = packer.Pack(reqs);
    // 结果按装箱顺序（面积降序）返回——一律按 id 查。
    bool oversizePlaced = true, zeroPlaced = true, smallPlaced = false;
    for (const auto& r : results) {
        if (r.id == 0) oversizePlaced = r.placed;
        if (r.id == 1) zeroPlaced = r.placed;
        if (r.id == 2) smallPlaced = r.placed;
    }
    CHECK(!oversizePlaced, "oversize rejected");
    CHECK(!zeroPlaced, "zero-size rejected");
    CHECK(smallPlaced, "small rect still places");
}

void TestContainerRoundtrip() {
    LightmapContainer c;
    c.atlas_count = 1;
    LightmapPageData page;
    page.width = 8;
    page.height = 8;
    page.texels_rgba16f.resize(8 * 8 * 4);
    for (size_t i = 0; i < page.texels_rgba16f.size(); ++i)
        page.texels_rgba16f[i] = static_cast<std::uint16_t>(i * 7 % 65536);
    c.pages.push_back(page);

    LightmapMeshEntry e;
    e.SetName("sponza/pillar_03");
    e.atlas_index = 0;
    e.scale_x = 0.25f; e.scale_y = 0.25f;
    e.bias_x = 0.5f; e.bias_y = 0.125f;
    e.rect_x = 512; e.rect_y = 256; e.rect_w = 128; e.rect_h = 64;
    c.entries.push_back(e);

    const std::string path = "/tmp/lmap_roundtrip_test.lmap";
    CHECK(SaveLightmapContainer(path, c), "save");

    LightmapContainer loaded;
    CHECK(LoadLightmapContainer(path, loaded), "load");
    CHECK(loaded.atlas_count == 1, "atlas count");
    CHECK(loaded.pages.size() == 1 && loaded.pages[0].width == 8 &&
              loaded.pages[0].height == 8, "page dims");
    CHECK(loaded.pages[0].texels_rgba16f == page.texels_rgba16f, "texel bytes identical");
    CHECK(loaded.entries.size() == 1, "entry count");
    const auto& le = loaded.entries[0];
    CHECK(le.GetName() == "sponza/pillar_03", "name roundtrip");
    CHECK(le.scale_x == 0.25f && le.bias_y == 0.125f, "scale/bias roundtrip");
    CHECK(le.rect_w == 128 && le.rect_h == 64, "rect roundtrip");
    std::remove(path.c_str());
}

void TestContainerRejects() {
    LightmapContainer bad;
    bad.atlas_count = 1;
    LightmapPageData p;
    p.width = 4; p.height = 4;
    p.texels_rgba16f.resize(4 * 4 * 4, 0x3C00);
    bad.pages.push_back(p);

    const std::string path = "/tmp/lmap_reject_test.lmap";
    CHECK(SaveLightmapContainer(path, bad), "save valid first");

    // 坏 magic。
    FILE* f = std::fopen(path.c_str(), "r+b");
    CHECK(f != nullptr, "open for corruption");
    if (f) {
        const std::uint32_t evil = 0xDEADBEEF;
        std::fseek(f, 0, SEEK_SET);
        std::fwrite(&evil, 4, 1, f);
        std::fclose(f);
    }
    LightmapContainer out;
    CHECK(!LoadLightmapContainer(path, out), "bad magic rejected");

    // 截断。
    f = std::fopen(path.c_str(), "rb");
    std::vector<std::uint8_t> bytes;
    if (f) {
        char buf[4096]; size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
    }
    f = std::fopen(path.c_str(), "wb");
    if (f) { std::fwrite(bytes.data(), 1, bytes.size() / 2, f); std::fclose(f); }
    CHECK(!LoadLightmapContainer(path, out), "truncated rejected");
    std::remove(path.c_str());

    // 尺寸不符拒绝写。
    LightmapContainer mismatch = bad;
    mismatch.pages[0].texels_rgba16f.resize(10);  // 与 4×4×4 不符
    CHECK(!SaveLightmapContainer("/tmp/lmap_never.lmap", mismatch), "mismatched texels rejected on save");
}

void TestContractSizes() {
    CHECK(sizeof(LightmapBakeSettings) == 280, "settings ABI size 280");
    CHECK(sizeof(LightmapMeshEntry) == 104, "mesh entry size 104");
    const auto def = DefaultLightmapBakeSettings();
    CHECK(def.atlas_size == 2048 && def.samples_per_texel == 256 && def.bounces == 3 &&
              def.texel_density == 16.0f && def.denoise == 1 && def.dilation_px == 8,
          "default settings values");
}

} // namespace

int main() {
    TestPackerBasics();
    TestPackerDeterministic();
    TestPackerOversize();
    TestContainerRoundtrip();
    TestContainerRejects();
    TestContractSizes();
    if (g_failures == 0) {
        std::cout << "[TestLightmapBaking] ALL PASSED" << std::endl;
        return 0;
    }
    std::cout << "[TestLightmapBaking] FAILED: " << g_failures << " check(s)" << std::endl;
    return 1;
}
