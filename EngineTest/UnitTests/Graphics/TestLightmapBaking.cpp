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

// ---------- M2：烘焙器 ----------

std::uint16_t HalfFromBits(std::uint16_t h) { return h; }
float HalfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const std::uint32_t exp = (h >> 10) & 0x1Fu;
    const std::uint32_t mant = h & 0x3FFu;
    std::uint32_t out;
    if (exp == 0) out = sign;
    else if (exp == 31) out = sign | 0x7F800000u | (mant << 13);
    else out = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &out, 4);
    return f;
}

// 轴对齐水平四边形（2 三角形）。normalY = +1 朝上 / -1 朝下（绕序随之）。
// UV 恒等映射（u=x 归一, v=z 归一）。
BakeMesh MakeQuad(const std::string& name, float cx, float cy, float cz, float half,
                  float normalY, bool withUv, const float (&albedo)[3],
                  const float (&emissive)[3]) {
    // 测试寿命内持有；一次性预留大容量——后续 push 不得 realloc，
    // 否则早先 BakeMesh 里的裸指针悬空。
    static std::vector<float> posPool, uvPool;
    static std::vector<std::uint32_t> idxPool;
    static bool reserved = false;
    if (!reserved) {
        posPool.reserve(8192); uvPool.reserve(8192); idxPool.reserve(8192);
        reserved = true;
    }
    BakeMesh m;
    m.name = name;
    const std::uint32_t base = (std::uint32_t)(posPool.size() / 3);      // 顶点窗起点
    const std::uint32_t idxBase = (std::uint32_t)(idxPool.size() / 6);   // quad 索引窗起点
    const float xs[4] = {cx - half, cx - half, cx + half, cx + half};
    const float zs[4] = {cz - half, cz + half, cz + half, cz - half};
    const float us[4] = {0, 0, 1, 1};
    const float vs[4] = {0, 1, 1, 0};
    for (int i = 0; i < 4; ++i) {
        posPool.push_back(xs[i]); posPool.push_back(cy); posPool.push_back(zs[i]);
        uvPool.push_back(us[i]); uvPool.push_back(vs[i]);
    }
    // 朝上：0,1,2 / 0,2,3；朝下反转绕序。
    // 索引用网格内相对值（0..3）——BakeMesh 契约：indices 相对自身顶点窗。
    if (normalY > 0) {
        const std::uint32_t t[6] = {0, 1, 2, 0, 2, 3};
        for (std::uint32_t i : t) idxPool.push_back(i);
    } else {
        const std::uint32_t t[6] = {0, 2, 1, 0, 3, 2};
        for (std::uint32_t i : t) idxPool.push_back(i);
    }
    m.positions = posPool.data() + base * 3;
    m.lightmap_uvs = withUv ? uvPool.data() + base * 2 : nullptr;
    m.indices = idxPool.data() + idxBase * 6;
    m.vertex_count = 4;
    m.index_count = 6;
    m.albedo[0] = albedo[0]; m.albedo[1] = albedo[1]; m.albedo[2] = albedo[2];
    m.emissive[0] = emissive[0]; m.emissive[1] = emissive[1]; m.emissive[2] = emissive[2];
    return m;
}

// 解析解场景：地板 1×1（albedo 0.5）+ 天花板 4×4@y=1（emissive 1.0，朝下）。
// 地板中心 bounce-1 出射 = ρ/π × E；E = πL·F，F = 4×(1/2π)[W/√(1+W²)·atan(T/√(1+W²))
// + T/√(1+T²)·atan(W/√(1+T²))]（点源正对矩形中心，四象限各延展 w=t=2、
// h=1 → W=T=2 → F=0.83112 → E=2.6108 → 期望 ρE/π = 0.41558；
// 与径向数值积分 ∫2πr/(1+r²)² 下限 2.513 + 角落增量交叉验证）。
void TestBakerAnalytic() {
    const float floorAlbedo[3] = {0.5f, 0.5f, 0.5f};
    const float ceilingAlbedo[3] = {0.0f, 0.0f, 0.0f};
    const float emissive1[3] = {1.0f, 1.0f, 1.0f};
    const float emissive0[3] = {0.0f, 0.0f, 0.0f};

    BakeScene scene;
    scene.meshes.push_back(MakeQuad("floor", 0, 0, 0, 0.5f, +1, true, floorAlbedo, emissive0));
    scene.meshes.push_back(MakeQuad("ceiling", 0, 1, 0, 2.0f, -1, true, ceilingAlbedo, emissive1));
    scene.sky.intensity = 0.0f;

    LightmapBakeSettings s = DefaultLightmapBakeSettings();
    s.atlas_size = 128;
    s.samples_per_texel = 1024;
    s.bounces = 1;
    s.texel_density = 16.0f;
    s.denoise = 0;
    s.dilation_px = 0;
    std::snprintf(s.output_dir, sizeof(s.output_dir), "%s", "/tmp/lmap_bake_test");

    const auto r = BakeLightmaps(scene, s);
    CHECK(r.ok, "bake ok" + (r.ok ? "" : " err=" + r.error));
    if (!r.ok) return;
    CHECK(r.entries.size() == 2, "two baked entries");

    LightmapContainer c;
    CHECK(LoadLightmapContainer(r.output_path, c), "reload baked container");

    // 找 floor 条目，取其 rect 中心纹素。
    const LightmapMeshEntry* floorEntry = nullptr;
    for (const auto& e : c.entries)
        if (e.GetName() == "floor") floorEntry = &e;
    CHECK(floorEntry != nullptr, "floor entry exists");
    if (!floorEntry) return;
    const auto& page = c.pages[0];
    const std::uint32_t cx = floorEntry->rect_x + floorEntry->rect_w / 2;
    const std::uint32_t cy = floorEntry->rect_y + floorEntry->rect_h / 2;
    const float got = HalfToFloat(page.texels_rgba16f[(cy * page.width + cx) * 4 + 1]);
    CHECK(std::abs(got - 0.41558f) < 0.02f,
          "analytic bounce-1 center value (got " + std::to_string(got) + ", want 0.41558)");

    // 天花板自身间接 = 0（albedo 0，向下射线只能打到地板的 0 阶=0）。
    const LightmapMeshEntry* ceilEntry = nullptr;
    for (const auto& e : c.entries)
        if (e.GetName() == "ceiling") ceilEntry = &e;
    if (ceilEntry) {
        const std::uint32_t kx = ceilEntry->rect_x + ceilEntry->rect_w / 2;
        const std::uint32_t ky = ceilEntry->rect_y + ceilEntry->rect_h / 2;
        const float ceilGot = HalfToFloat(page.texels_rgba16f[(ky * page.width + kx) * 4 + 1]);
        CHECK(ceilGot < 0.01f, "ceiling indirect ~0 (got " + std::to_string(ceilGot) + ")");
    }
}

// 确定性：同场景两次烘焙逐位一致。
void TestBakerDeterministic() {
    const float albedo[3] = {0.7f, 0.5f, 0.3f};
    const float emissive0[3] = {0, 0, 0};
    BakeScene scene;
    scene.meshes.push_back(MakeQuad("floor", 0, 0, 0, 0.5f, +1, true, albedo, emissive0));
    scene.meshes.push_back(MakeQuad("ceiling", 0, 1, 0, 2.0f, -1, true, albedo, {0.6f, 0.6f, 0.6f}));
    scene.sky.intensity = 0.5f;

    LightmapBakeSettings s = DefaultLightmapBakeSettings();
    s.atlas_size = 128;
    s.samples_per_texel = 64;
    s.bounces = 2;
    s.texel_density = 12.0f;
    s.denoise = 0;
    s.dilation_px = 0;
    std::snprintf(s.output_dir, sizeof(s.output_dir), "%s", "/tmp/lmap_det_a");
    const auto a = BakeLightmaps(scene, s);
    std::snprintf(s.output_dir, sizeof(s.output_dir), "%s", "/tmp/lmap_det_b");
    const auto b = BakeLightmaps(scene, s);
    CHECK(a.ok && b.ok, "both bakes ok");
    if (!(a.ok && b.ok)) return;
    LightmapContainer ca, cb;
    CHECK(LoadLightmapContainer(a.output_path, ca), "load a");
    CHECK(LoadLightmapContainer(b.output_path, cb), "load b");
    CHECK(ca.pages[0].texels_rgba16f == cb.pages[0].texels_rgba16f, "deterministic texels");
}

// 扩张：rect 外圈（margin 区）在 dilation 后非零。
void TestBakerDilation() {
    const float albedo[3] = {0.8f, 0.8f, 0.8f};
    const float emissive0[3] = {0, 0, 0};
    const float emissive1[3] = {1, 1, 1};
    BakeScene scene;
    scene.meshes.push_back(MakeQuad("floor", 0, 0, 0, 0.5f, +1, true, albedo, emissive0));
    scene.meshes.push_back(MakeQuad("ceiling", 0, 1, 0, 2.0f, -1, true, {0, 0, 0}, emissive1));
    scene.sky.intensity = 0.0f;

    LightmapBakeSettings s = DefaultLightmapBakeSettings();
    s.atlas_size = 128;
    s.samples_per_texel = 64;
    s.bounces = 1;
    s.texel_density = 16.0f;
    s.denoise = 0;
    s.dilation_px = 3;
    std::snprintf(s.output_dir, sizeof(s.output_dir), "%s", "/tmp/lmap_dilate");
    const auto r = BakeLightmaps(scene, s);
    CHECK(r.ok, "dilate bake ok");
    if (!r.ok) return;
    LightmapContainer c;
    CHECK(LoadLightmapContainer(r.output_path, c), "load");
    const LightmapMeshEntry* floorEntry = nullptr;
    for (const auto& e : c.entries)
        if (e.GetName() == "floor") floorEntry = &e;
    if (!floorEntry) { CHECK(false, "floor entry"); return; }
    // rect 内中心必须非零；rect 外 2px（margin 区）必须已被扩张填充。
    const auto& page = c.pages[0];
    const std::uint32_t cx = floorEntry->rect_x + floorEntry->rect_w / 2;
    const std::uint32_t cy = floorEntry->rect_y + floorEntry->rect_h / 2;
    CHECK(HalfToFloat(page.texels_rgba16f[(cy * page.width + cx) * 4 + 1]) > 0.1f,
          "floor interior nonzero");
    if (floorEntry->rect_x >= 2 && floorEntry->rect_y >= 2) {
        const std::uint32_t ox = floorEntry->rect_x - 2;
        const std::uint32_t oy = floorEntry->rect_y - 2;
        CHECK(HalfToFloat(page.texels_rgba16f[(oy * page.width + ox) * 4 + 1]) > 0.01f,
              "dilation ring nonzero at rect corner -2px");
    }
}

// trace-only：无 UV mesh 不占 atlas、可被命中遮挡。
void TestBakerTraceOnly() {
    const float albedo[3] = {0.5f, 0.5f, 0.5f};
    const float emissive0[3] = {0, 0, 0};
    BakeScene scene;
    scene.meshes.push_back(MakeQuad("floor", 0, 0, 0, 0.5f, +1, true, albedo, emissive0));
    // 遮挡板：带 UV 但设 trace-only（置空 UV 模拟未展开网格）。
    scene.meshes.push_back(MakeQuad("blocker", 0, 0.5f, 0, 2.0f, -1, false, {0, 0, 0}, emissive0));
    // 方向光自上而下——被 blocker 完全遮挡 → 地板直射=0、反弹=0。
    BakeLight light;
    light.type = BakeLight::Type::Directional;
    light.intensity = 5.0f;
    light.direction[0] = 0; light.direction[1] = -1; light.direction[2] = 0;
    scene.lights.push_back(light);
    scene.sky.intensity = 0.0f;

    LightmapBakeSettings s = DefaultLightmapBakeSettings();
    s.atlas_size = 128;
    s.samples_per_texel = 64;
    s.bounces = 1;
    s.texel_density = 16.0f;
    s.denoise = 0;
    s.dilation_px = 0;
    std::snprintf(s.output_dir, sizeof(s.output_dir), "%s", "/tmp/lmap_traceonly");
    const auto r = BakeLightmaps(scene, s);
    CHECK(r.ok, "trace-only bake ok" + (r.ok ? "" : " err=" + r.error));
    if (!r.ok) return;
    CHECK(r.entries.size() == 1, "only floor gets an entry");
    CHECK(r.entries[0].GetName() == "floor", "entry is floor");

    LightmapContainer c;
    CHECK(LoadLightmapContainer(r.output_path, c), "load");
    const auto& e = c.entries[0];
    const auto& page = c.pages[0];
    const std::uint32_t cx = e.rect_x + e.rect_w / 2;
    const std::uint32_t cy = e.rect_y + e.rect_h / 2;
    const float got = HalfToFloat(page.texels_rgba16f[(cy * page.width + cx) * 4 + 1]);
    CHECK(got < 0.01f, "occluded floor indirect ~0 (got " + std::to_string(got) + ")");
}

// v1.1：trace-only 遮挡面一阶直射反弹。同解析测试几何（F=0.83112），
// 板朝下、方向光自下而上照亮板底 → 地板收到板反弹的间接光。
// 期望 ≈ 0.5/π×(0.5/π×5)×0.83112 ≈ 0.105。
void TestBakerBlockerBounce() {
    const float albedo[3] = {0.5f, 0.5f, 0.5f};
    const float emissive0[3] = {0, 0, 0};
    BakeScene scene;
    scene.meshes.push_back(MakeQuad("floor", 0, 0, 0, 0.5f, +1, true, albedo, emissive0));
    // 板朝下（trace-only，无 UV 不占 atlas）；光从下方来 → 板底被照亮。
    scene.meshes.push_back(MakeQuad("blocker", 0, 1, 0, 2.0f, -1, false, albedo, emissive0));
    BakeLight light;
    light.type = BakeLight::Type::Directional;
    light.intensity = 5.0f;
    light.direction[0] = 0; light.direction[1] = 1; light.direction[2] = 0;
    scene.lights.push_back(light);
    scene.sky.intensity = 0.0f;

    LightmapBakeSettings s = DefaultLightmapBakeSettings();
    s.atlas_size = 128;
    s.samples_per_texel = 256;
    s.bounces = 1;
    s.texel_density = 16.0f;
    s.denoise = 0;
    s.dilation_px = 0;
    std::snprintf(s.output_dir, sizeof(s.output_dir), "%s", "/tmp/lmap_blockerbounce");
    const auto r = BakeLightmaps(scene, s);
    CHECK(r.ok, "blocker bounce bake ok" + (r.ok ? "" : " err=" + r.error));
    if (!r.ok) return;
    CHECK(r.entries.size() == 1, "only floor gets an entry");

    LightmapContainer c;
    CHECK(LoadLightmapContainer(r.output_path, c), "load");
    const auto& e = c.entries[0];
    const auto& page = c.pages[0];
    const std::uint32_t cx = e.rect_x + e.rect_w / 2;
    const std::uint32_t cy = e.rect_y + e.rect_h / 2;
    const float got = HalfToFloat(page.texels_rgba16f[(cy * page.width + cx) * 4 + 1]);
    CHECK(got > 0.05f, "bounced floor indirect present (got " + std::to_string(got) + ")");
    CHECK(got < 0.25f, "bounced floor indirect bounded (got " + std::to_string(got) + ")");
}

} // namespace

int main() {
    TestPackerBasics();
    TestPackerDeterministic();
    TestPackerOversize();
    TestContainerRoundtrip();
    TestContainerRejects();
    TestContractSizes();
    TestBakerAnalytic();
    TestBakerDeterministic();
    TestBakerDilation();
    TestBakerTraceOnly();
    TestBakerBlockerBounce();
    if (g_failures == 0) {
        std::cout << "[TestLightmapBaking] ALL PASSED" << std::endl;
        return 0;
    }
    std::cout << "[TestLightmapBaking] FAILED: " << g_failures << " check(s)" << std::endl;
    return 1;
}
