/**
 * @file TestNeuralWeights.cpp
 * @brief .pnn 解析 + CPU 参考前向的单测（无 GPU 依赖）
 *
 * 覆盖三块：
 *   1. 合成网络（1 级 hash grid + 2 层 MLP）解析与手算前向对拍；
 *   2. 失败路径（坏 magic / 截断 / 坏版本 / 层链断裂）必须拒绝；
 *   3. Python 训练器导出的 parity 向量（EngineTest/assets/Neural/
 *      test_parity.bin）：编码级 atol 1e-6，端到端 atol 2e-4（累加顺序差异）。
 *      资产缺失时 SKIP（不 FAIL），保证无资产环境可跑。
 */
#include "Engine/Graphics/Neural/NeuralWeights.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace primal::graphics::neural;

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

template <typename T>
void Append(std::vector<std::uint8_t>& blob, const T& v) {
    const size_t off = blob.size();
    blob.resize(off + sizeof(T));
    std::memcpy(blob.data() + off, &v, sizeof(T));
}

// 合成网络：levels=1, F=2, entries=4 → in_dim=4；4→3 relu；3→2 sigmoid。
std::vector<std::uint8_t> BuildSyntheticPnn() {
    std::vector<std::uint8_t> blob;
    Append(blob, kPnnMagic);
    Append(blob, kPnnVersion);
    Append(blob, std::uint32_t{2});       // layer_count
    Append(blob, std::uint32_t{8});       // table_element_count (1*4*2)
    Append(blob, std::uint32_t{4});       // input_dim
    Append(blob, std::uint32_t{2});       // output_dim
    Append(blob, std::uint32_t{1});       // param_count
    Append(blob, std::uint32_t{0});       // reserved
    Append(blob, std::uint32_t{2});       // level_res[0] = 2
    // layer 0: 4→3 relu
    Append(blob, std::uint32_t{4});
    Append(blob, std::uint32_t{3});
    blob.push_back(1);  // relu
    blob.push_back(0);  // f32
    Append(blob, std::uint16_t{0});
    // layer 1: 3→2 sigmoid
    Append(blob, std::uint32_t{3});
    Append(blob, std::uint32_t{2});
    blob.push_back(2);  // sigmoid
    blob.push_back(0);
    Append(blob, std::uint16_t{0});
    // table: 8 f32（0.0,0.1,...,0.7）
    for (int i = 0; i < 8; ++i) Append(blob, 0.1f * i);
    // W0 (3x4 row-major) + b0
    for (int o = 0; o < 3; ++o)
        for (int i = 0; i < 4; ++i) Append(blob, 0.25f * (o + 1) - 0.1f * i);
    Append(blob, 0.5f); Append(blob, -0.25f); Append(blob, 0.125f);
    // W1 (2x3) + b1
    Append(blob, 0.75f); Append(blob, -0.5f); Append(blob, 0.25f);
    Append(blob, 0.5f); Append(blob, 0.125f); Append(blob, -0.375f);
    Append(blob, 0.1f); Append(blob, -0.2f);
    return blob;
}

bool NearlyEqual(float a, float b, float atol) {
    return std::fabs(a - b) <= atol;
}

void TestSynthetic() {
    const std::vector<std::uint8_t> blob = BuildSyntheticPnn();
    NeuralNetwork net;
    std::string err;
    CHECK(ParsePnn(blob.data(), blob.size(), net, &err), "synthetic parse failed: " + err);
    if (g_failures) return;
    CHECK(net.input_dim == 4 && net.output_dim == 2, "synthetic dims");
    CHECK(net.layers.size() == 2, "synthetic layer count");
    CHECK(net.level_res.size() == 1 && net.level_res[0] == 2, "synthetic level_res");
    CHECK(net.table.size() == 8, "synthetic table size");

    // 手算：uv = (0.25, 0.75)，res=2 → qx=0.5, qy=1.5
    // level0: flx=0, fly=1, i0=0, j0=1, fx=0.5, fy=0.5, i1=1, j1=1（clamp 1）
    // corners hash(l=0): h = 0 ^ i*P_I ^ j*P_J; h *= MIX; h ^= h>>15; mask 3
    auto hash3 = [](std::uint32_t i, std::uint32_t j) {
        std::uint32_t h = i * 0x85EBCA6Bu;
        h ^= j * 0xC2B2AE35u;
        h *= 0x27D4EB2Fu;
        h ^= h >> 15u;
        return h & 3u;
    };
    const std::uint32_t idx[2][2] = {
        {hash3(0, 1), hash3(0, 1)},  // (i0,j0) (i0,j1)
        {hash3(1, 1), hash3(1, 1)},  // (i1,j0) (i1,j1)
    };
    (void)idx;

    std::vector<float> feat(4);
    CHECK(HashEncode(net, 0.25f, 0.75f, feat.data()), "synthetic hash encode");
    // 手算插值（w = 0.5/0.5）：c0 = (f00+f10+f01+f11)*0.25 per comp
    const auto at = [&](std::uint32_t i, std::uint32_t j, int c) {
        return net.table[(hash3(i, j) * 2 + c)];
    };
    const float c0 = 0.25f * (at(0, 1, 0) + at(1, 1, 0) + at(0, 1, 0) + at(1, 1, 0));
    const float c1 = 0.25f * (at(0, 1, 1) + at(1, 1, 1) + at(0, 1, 1) + at(1, 1, 1));
    CHECK(NearlyEqual(feat[0], c0, 1e-6f), "synthetic feat[0]");
    CHECK(NearlyEqual(feat[1], c1, 1e-6f), "synthetic feat[1]");
    CHECK(NearlyEqual(feat[2], 0.25f, 1e-7f) && NearlyEqual(feat[3], 0.75f, 1e-7f),
          "synthetic raw uv append");

    // 端到端手算
    std::vector<float> scratch, out(2);
    CHECK(ReferenceForward(net, feat.data() + 2, out.data(), scratch), "synthetic forward");
    // hidden: h[o] = relu(b0[o] + Σ feat[i]*W0[o][i])
    float h[3];
    const float* w0 = net.weights.data();
    for (int o = 0; o < 3; ++o) {
        float acc = net.weights[12 + o];   // b0 after 12 W0 floats
        for (int i = 0; i < 4; ++i) acc += feat[i] * w0[o * 4 + i];
        h[o] = acc > 0 ? acc : 0;
    }
    const float* w1 = net.weights.data() + 15;   // W1 after W0(12)+b0(3)
    for (int o = 0; o < 2; ++o) {
        float acc = net.weights[15 + 6 + o];     // b1 after W1(6)
        for (int i = 0; i < 3; ++i) acc += h[i] * w1[o * 3 + i];
        const float expect = 1.0f / (1.0f + std::exp(-acc));
        CHECK(NearlyEqual(out[o], expect, 1e-6f), "synthetic mlp out");
    }
}

void TestRejects() {
    NeuralNetwork net;
    std::string err;
    const std::vector<std::uint8_t> good = BuildSyntheticPnn();

    auto bad = good;
    bad[0] = 0xFF;
    CHECK(!ParsePnn(bad.data(), bad.size(), net, &err), "bad magic must fail");

    CHECK(!ParsePnn(good.data(), 16, net, &err), "truncated header must fail");

    auto badver = good;
    badver[4] = 9;
    CHECK(!ParsePnn(badver.data(), badver.size(), net, &err), "bad version must fail");

    // 层链断裂：layer1.in_dim 改成 5。
    auto broken = good;
    const size_t l1_in_off = 32 + 4 + 8;   // header+res+layer0 desc = 44
    broken[l1_in_off] = 5;
    CHECK(!ParsePnn(broken.data(), broken.size(), net, &err), "broken chain must fail");

    // 截断 payload。
    CHECK(!ParsePnn(good.data(), good.size() - 8, net, &err), "truncated payload must fail");
}

void TestParityVectors() {
    const char* candidates[] = {
        "Assets/Neural/test_parity.bin",
        "EngineTest/assets/Neural/test_parity.bin",
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Neural/test_parity.bin",
    };
    std::vector<std::uint8_t> bytes;
    for (const char* c : candidates) {
        std::ifstream f(c, std::ios::binary | std::ios::ate);
        if (!f.is_open()) continue;
        const std::streamsize size = f.tellg();
        f.seekg(0);
        bytes.resize((size_t)size);
        if (f.read(reinterpret_cast<char*>(bytes.data()), size)) break;
        bytes.clear();
    }
    if (bytes.empty()) {
        std::cout << "[SKIP] test_parity.bin not found — trained-model parity not checked"
                  << std::endl;
        return;
    }
    constexpr std::uint32_t kParityMagic = 0x56504E50;  // "PNPV"
    size_t pos = 0;
    auto rd32 = [&]() {
        std::uint32_t v;
        std::memcpy(&v, bytes.data() + pos, 4);
        pos += 4;
        return v;
    };
    auto rdf32 = [&]() {
        float v;
        std::memcpy(&v, bytes.data() + pos, 4);
        pos += 4;
        return v;
    };
    CHECK(rd32() == kParityMagic, "parity magic");
    CHECK(rd32() == 1, "parity version");
    const std::uint32_t n = rd32();
    const std::uint32_t in_dim = rd32();
    const std::uint32_t out_dim = rd32();
    CHECK(n > 0 && in_dim > 0 && out_dim > 0, "parity dims");

    NeuralNetwork net;
    std::string err;
    bool loaded = false;
    for (const char* c : {"Assets/Neural/test_image.pnn",
                          "EngineTest/assets/Neural/test_image.pnn",
                          "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Neural/"
                          "test_image.pnn"}) {
        if ((loaded = LoadPnnFile(c, net, &err))) break;
    }
    if (!loaded) {
        std::cout << "[SKIP] test_image.pnn not found — trained-model parity not checked"
                  << std::endl;
        return;
    }
    CHECK(net.input_dim == in_dim && net.output_dim == out_dim, "parity dims match net");

    std::vector<float> uv(n * 2), enc(n * in_dim), expect(n * out_dim);
    for (std::uint32_t i = 0; i < n * 2; ++i) uv[i] = rdf32();
    for (std::uint32_t i = 0; i < n * in_dim; ++i) enc[i] = rdf32();
    for (std::uint32_t i = 0; i < n * out_dim; ++i) expect[i] = rdf32();

    std::uint32_t enc_bad = 0, fwd_bad = 0;
    std::vector<float> feat(in_dim), scratch, out(out_dim);
    float enc_max_err = 0.0f, fwd_max_err = 0.0f;
    for (std::uint32_t p = 0; p < n; ++p) {
        HashEncode(net, uv[p * 2], uv[p * 2 + 1], feat.data());
        for (std::uint32_t i = 0; i < in_dim; ++i) {
            const float e = std::fabs(feat[i] - enc[p * in_dim + i]);
            enc_max_err = std::max(enc_max_err, e);
            if (e > 1e-6f) ++enc_bad;
        }
        const float uv2[2] = {uv[p * 2], uv[p * 2 + 1]};
        ReferenceForward(net, uv2, out.data(), scratch);
        for (std::uint32_t i = 0; i < out_dim; ++i) {
            const float e = std::fabs(out[i] - expect[p * out_dim + i]);
            fwd_max_err = std::max(e, fwd_max_err);
            if (e > 2e-4f) ++fwd_bad;
        }
    }
    std::cout << "[parity] n=" << n << " enc_max_err=" << enc_max_err
              << " fwd_max_err=" << fwd_max_err << std::endl;
    CHECK(enc_bad == 0, "hash-encoding parity (atol 1e-6)");
    CHECK(fwd_bad == 0, "end-to-end forward parity (atol 2e-4)");
}

void TestConvParityVectors() {
    // denoiser.pnn（v2 conv）+ denoiser_parity.bin：torch 全前向期望，
    // CPU 参考前向只比较内部像素（边界 clamp ≠ torch zero-pad）。
    const char* pnn_candidates[] = {
        "Assets/Neural/denoiser.pnn",
        "EngineTest/assets/Neural/denoiser.pnn",
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Neural/denoiser.pnn",
    };
    NeuralNetwork net;
    std::string err;
    bool loaded = false;
    for (const char* c : pnn_candidates) {
        if ((loaded = LoadPnnFile(c, net, &err))) break;
    }
    if (!loaded) {
        std::cout << "[SKIP] denoiser.pnn not found — conv parity not checked" << std::endl;
        return;
    }
    CHECK(net.version == 2 && net.layers.size() == 4, "denoiser net shape");
    CHECK(net.input_dim == 7 && net.output_dim == 3, "denoiser dims");

    const char* par_candidates[] = {
        "Assets/Neural/denoiser_parity.bin",
        "EngineTest/assets/Neural/denoiser_parity.bin",
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Neural/denoiser_parity.bin",
    };
    std::vector<std::uint8_t> bytes;
    for (const char* c : par_candidates) {
        std::ifstream f(c, std::ios::binary | std::ios::ate);
        if (!f.is_open()) continue;
        const std::streamsize size = f.tellg();
        f.seekg(0);
        bytes.resize((size_t)size);
        if (f.read(reinterpret_cast<char*>(bytes.data()), size)) break;
        bytes.clear();
    }
    if (bytes.empty()) {
        std::cout << "[SKIP] denoiser_parity.bin not found" << std::endl;
        return;
    }
    size_t pos = 0;
    auto rd32 = [&]() {
        std::uint32_t v;
        std::memcpy(&v, bytes.data() + pos, 4);
        pos += 4;
        return v;
    };
    CHECK(rd32() == 0x32504E50, "conv parity magic");   // "PNP2"
    const std::uint32_t ph = rd32(), pw = rd32(), cin = rd32(), cout = rd32();
    CHECK(ph == 16 && pw == 16 && cin == net.input_dim && cout == net.output_dim,
          "conv parity dims");
    std::vector<float> in(ph * pw * cin), expect(ph * pw * cout);
    std::memcpy(in.data(), bytes.data() + pos, in.size() * 4);
    pos += in.size() * 4;
    std::memcpy(expect.data(), bytes.data() + pos, expect.size() * 4);

    std::vector<float> scratch, out((size_t)cout * ph * pw);
    CHECK(ReferenceForwardConv(net, in.data(), pw, ph, out.data(), scratch),
          "conv forward runs");
    float max_err = 0.0f;
    for (std::uint32_t y = 1; y < ph - 1; ++y) {
        for (std::uint32_t x = 1; x < pw - 1; ++x) {
            for (std::uint32_t c = 0; c < cout; ++c) {
                const float got = out[(size_t)c * ph * pw + y * pw + x];
                const float exp = expect[(size_t)c * ph * pw + y * pw + x];
                max_err = std::max(max_err, std::fabs(got - exp));
            }
        }
    }
    std::cout << "[conv parity] interior max_err=" << max_err << std::endl;
    CHECK(max_err < 2e-4f, "conv parity (atol 2e-4, interior)");
}

}  // namespace

int main() {
    std::cout << "== TestNeuralWeights ==" << std::endl;
    TestSynthetic();
    TestRejects();
    TestParityVectors();
    TestConvParityVectors();
    if (g_failures) {
        std::cerr << "FAILED: " << g_failures << " check(s)" << std::endl;
        return 1;
    }
    std::cout << "PASSED" << std::endl;
    return 0;
}
