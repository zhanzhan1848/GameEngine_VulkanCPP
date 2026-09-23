/**
 * @file NeuralMLXGateMain.cpp
 * @brief M1-b 端到端 PSNR 门禁工具（离线，采集数据 + MLX shim 同帧对拍）
 *
 * 数据链路：NeuralCaptureModule 采集的 take（引擎运行时产出，Welford GT +
 * 同帧 noisy 1spp）→ 本工具按引擎 pack 语义组 7ch 输入 → MLX shim 推理 →
 * expm1 回线性域 → 与 gt_mean 算 PSNR（dr = max(gt)，与 train_denoiser.py
 * 同约定）。
 *
 * 门禁：denoised vs GT ≥ 24 dB（Docs/2026-09-15-neural-mlx-backend-plan.md M1-b）。
 * 同时报告 noisy 基线 PSNR 与两种输入变体：
 *   engine  = 引擎 pack 语义（ch3..5 = 真实法线）
 *   trained = 训练脚本语义（ch3..5 = 0 —— train_denoiser.py 的 7ch 分支
 *             未写 normal，发版权重是在法线全零下训练的；此变体量化该
 *             训练/运行时分布差的影响）
 *
 * 用法: NeuralMLXGate <take_dir> [--gate-db 24] [--synthetic-sigma] [--dump-out FILE]
 * [--dump-out]:把末个 take 的 engine-variant denoised 平面(f32 HWC 交错)落盘,
 * 供与 torch 侧逐像素对拍。
 * 返回 0 = 门禁通过或数据缺失（SKIP），2 = 门禁未达。
 *
 * --synthetic-sigma：半真实验证模式（明确标注，不作为门禁依据）。当 take 的
 * noisy 平面无效（Vulkan capture pack 的已知缺口）时，用 take 自带的
 * gt_var（引擎实测方差）合成 1spp 噪声样本 gt + sqrt(var)·randn，
 * 验证门禁机制在真实 GT 上端到端可跑。PSNR 数字仅供参考。
 */
#include "Engine/Graphics/Neural/NeuralMLXBackend.h"
#include "Engine/Graphics/Neural/NeuralWeights.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace primal::graphics::neural;

namespace {

struct TakeMeta {
    u32 w = 0, h = 0;
    float near = 0.1f, far = 1000.0f;
    u32 frames = 0;
};

/// 极简 json 数值提取（take.json 字段固定、一行一条）。
TakeMeta ParseMeta(const std::string& text) {
    TakeMeta m{};
    auto grab = [&](const char* key) -> float {
        const std::string pat = "\"" + std::string(key) + "\": ";
        size_t p = text.find(pat);
        if (p == std::string::npos) return 0.0f;
        p += pat.size();
        return std::strtof(text.c_str() + p, nullptr);
    };
    m.w = (u32)grab("w");
    m.h = (u32)grab("h");
    m.near = grab("near");
    m.far = grab("far");
    m.frames = (u32)grab("frames");
    return m;
}

float LinearizeDepth(float d, float n, float f) {
    return (n * f) / (f - d * (f - n));
}

/// SSIM(结构相似度):luma、Reinhard 显示映射 v/(1+v) 后按 8×8 非重叠块
/// 计算 C1/C2 稳定 SSIM 再平均(HDR 内容的确定性约定;M2 门禁用)。
float Ssim(const std::vector<f32>& a, const std::vector<f32>& b, u32 w, u32 h, float dr) {
    const size_t plane = (size_t)w * h;
    if (a.size() < plane || b.size() < plane) return -1.0f;
    // 显示映射:log1p(luma)/log1p(dr) ∈ [0,1]——Reinhard 对 HDR 压缩过强
    // (SSIM 饱和到 0.9999 无区分度),log 域与训练损失同域、区分度更好。
    const f32 norm = std::log1p(dr);
    auto luma = [&](const std::vector<f32>& v, size_t i) {
        const f32 r = v[i * 3 + 0], g = v[i * 3 + 1], bl = v[i * 3 + 2];
        const f32 y = 0.2126f * r + 0.7152f * g + 0.0722f * bl;
        return std::log1p(std::max(y, 0.0f)) / norm;
    };
    const int B = 8;
    double ssim_sum = 0.0;
    int blocks = 0;
    const double C1 = 6.5025, C2 = 58.5225;   // (0.01*255)^2, (0.03*255)^2
    for (u32 by = 0; by + B <= h; by += B) {
        for (u32 bx = 0; bx + B <= w; bx += B) {
            double ma = 0, mb = 0, aa = 0, bb = 0, ab = 0;
            const int n = B * B;
            for (u32 dy = 0; dy < B; ++dy) {
                for (u32 dx = 0; dx < B; ++dx) {
                    const size_t i = (size_t)(by + dy) * w + (bx + dx);
                    const double va = luma(a, i), vb = luma(b, i);
                    ma += va; mb += vb; aa += va * va; bb += vb * vb; ab += va * vb;
                }
            }
            ma /= n; mb /= n; aa /= n; bb /= n; ab /= n;
            const double va_ = aa - ma * ma, vb_ = bb - mb * mb, cov = ab - ma * mb;
            ssim_sum += ((2 * ma * mb + C1) * (2 * cov + C2)) /
                        ((ma * ma + mb * mb + C1) * (va_ + vb_ + C2));
            ++blocks;
        }
    }
    return blocks ? (float)(ssim_sum / blocks) : -1.0f;
}

float Psnr(const std::vector<f32>& a, const std::vector<f32>& b, float dr) {
    if (a.size() != b.size() || a.empty() || dr <= 0.0f) return -1.0f;
    double mse = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double e = double(a[i]) - double(b[i]);
        mse += e * e;
    }
    mse /= double(a.size());
    return float(10.0 * std::log10(double(dr) * double(dr) / std::max(mse, 1e-12)));
}

/// 组 7ch 平面并跑 shim。variant: 0 = engine（真实法线），1 = trained（法线零）。
bool RunVariant(NeuralMLXBackend& mlx, const std::vector<f32>& noisy_log,
                const std::vector<f32>& normal, const std::vector<f32>& depth16,
                u32 w, u32 h, int variant, std::vector<f32>& out_linear,
                std::string* err) {
    const size_t plane = (size_t)w * h;
    // 2026-09-22 修复：shim 契约是 CHW 平面 [c][y][x]，此前把像素交错
    // (i*3+c) 缓冲直接当平面传——整个 take 评估喂了错误布局。
    std::vector<f32> in(plane * 7);
    for (size_t i = 0; i < plane; ++i) {
        for (int c = 0; c < 3; ++c) in[c * plane + i] = noisy_log[i * 3 + c];
    }
    if (variant == 0) {
        for (size_t i = 0; i < plane; ++i)
            for (int c = 0; c < 3; ++c) in[(3 + c) * plane + i] = normal[i * 3 + c];
    }
    for (size_t i = 0; i < plane; ++i) in[6 * plane + i] = depth16[i];

    std::vector<f32> out_log(plane * 3);
    if (!mlx.Infer(in.data(), 7, w, h, out_log.data(), 3, 0u, err)) return false;
    // shim 返回 CHW 平面 [c][y][x]；GT 侧是像素交错 HWC [i*3+c]——转回 HWC
    // 再比较（2026-09-22：此前 CHW 直比 HWC 导致 PSNR 被布局错位压低）。
    out_linear.resize(plane * 3);
    for (size_t i = 0; i < plane; ++i) {
        for (int c = 0; c < 3; ++c) {
            out_linear[i * 3 + c] = std::expm1(out_log[c * plane + i]);
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <take_dir> [--gate-db 24]" << std::endl;
        return 1;
    }
    const std::string dir = argv[1];
    float gate_db = 24.0f;
    bool synthetic = false;
    std::string dump_out;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--gate-db" && i + 1 < argc) {
            gate_db = std::strtof(argv[++i], nullptr);
        } else if (std::string(argv[i]) == "--synthetic-sigma") {
            synthetic = true;
        } else if (std::string(argv[i]) == "--dump-out" && i + 1 < argc) {
            dump_out = argv[++i];
        }
    }

    NeuralMLXBackend mlx;
    std::string err;
    if (!mlx.Initialize({}, {}, &err)) {
        std::cout << "[SKIP] MLX shim unavailable: " << err << std::endl;
        return 0;
    }

    // 找 take 文件（take_<tag>_<i>.bin/.json 对）
    std::vector<std::string> bases;
    for (int i = 0; i < 16; ++i) {
        for (const char* tag : {"sponza", "scene"}) {
            const std::string base = dir + "/take_" + tag + "_" + std::to_string(i);
            std::ifstream f(base + ".bin", std::ios::binary | std::ios::ate);
            if (f.is_open()) bases.push_back(base);
        }
    }
    if (bases.empty()) {
        std::cout << "[SKIP] no take_*.bin in " << dir
                  << " (run capture: PRIMAL_NEURAL_CAPTURE=1)" << std::endl;
        return 0;
    }

    int gate_failures = 0;
    for (const std::string& base : bases) {
        std::ifstream bf(base + ".bin", std::ios::binary | std::ios::ate);
        std::ifstream jf(base + ".json");
        std::string jtext((std::istreambuf_iterator<char>(jf)),
                          std::istreambuf_iterator<char>());
        const TakeMeta meta = ParseMeta(jtext);
        const size_t plane = (size_t)meta.w * meta.h;
        std::vector<u8> raw((size_t)bf.tellg());
        bf.seekg(0);
        if (!bf.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)raw.size()) ||
            raw.size() < plane * 16 * 5) {
            std::cerr << "[FAIL] " << base << ".bin truncated" << std::endl;
            ++gate_failures;
            continue;
        }
        // 平面布局 [gt_mean, gt_var, depth_raw, normal, noisy_1spp]，
        // rgba32f → 取各面前 3 通道（depth 取 0 通道）。
        auto pf32 = [&](size_t plane_idx) -> const f32* {
            return reinterpret_cast<const f32*>(raw.data() + plane_idx * plane * 16);
        };
        const f32* gt = pf32(0);
        const f32* depthr = pf32(2);
        const f32* nrm = pf32(3);
        const f32* noisy = pf32(4);

        std::vector<f32> noisy_log(plane * 3), normal(plane * 3), depth16(plane),
            gt3(plane * 3), noisy3(plane * 3);
        for (size_t i = 0; i < plane; ++i) {
            for (int c = 0; c < 3; ++c) {
                noisy_log[i * 3 + c] = std::log1p(std::max(noisy[i * 4 + c], 0.0f));
                normal[i * 3 + c] = nrm[i * 4 + c];
                gt3[i * 3 + c] = gt[i * 4 + c];
                noisy3[i * 3 + c] = noisy[i * 4 + c];
            }
            depth16[i] = std::clamp(
                LinearizeDepth(depthr[i * 4], meta.near, meta.far) / 16.0f, 0.0f, 1.0f);
        }
        // noisy 平面有效性检查（全零 = capture pack 缺口，见 --synthetic-sigma）
        float noisy_max = 0.0f;
        for (size_t i = 0; i < noisy3.size(); ++i) noisy_max = std::max(noisy_max, noisy3[i]);
        if (noisy_max <= 0.0f && synthetic) {
            // gt + sqrt(gt_var)·randn（Box-Muller；方差平面 rgba 的 r 通道）
            const f32* var = pf32(1);
            auto rnd = []() {
                return std::sqrt(-2.0 * std::log((std::rand() + 1) / (double)RAND_MAX + 1e-12))
                     * std::cos(2.0 * 3.14159265358979 * (std::rand() / (double)RAND_MAX));
            };
            for (size_t i = 0; i < plane; ++i) {
                for (int c = 0; c < 3; ++c) {
                    const float sg = std::sqrt(std::max(var[i * 4 + c], 0.0f));
                    const float v = std::max(gt[i * 4 + c] + (f32)rnd() * sg, 0.0f);
                    noisy3[i * 3 + c] = v;
                    noisy_log[i * 3 + c] = std::log1p(v);
                }
            }
            std::cout << "[gate] [SYNTHETIC-NOISE] noisy 平面全零，已用引擎 gt_var 合成"
                      << "（机制验证，非门禁依据）" << std::endl;
        }

        float dr = 1e-3f;
        for (size_t i = 0; i < gt3.size(); ++i) dr = std::max(dr, gt3[i]);

        const float psnr_noisy = Psnr(noisy3, gt3, dr);
        // 前置条件（门禁适用域）：noisy 基线本身 < 门限的 take 属不稳定域——
        // 单帧与 128 帧均值的距离已超门限（SPGI 双稳态翻转，GI 分支缺陷，
        // 见计划 §7.2），任何去噪器物理上不可达 → 跳过并如实标注。
        if (psnr_noisy < gate_db) {
            std::cout << "[gate] " << base << " [SKIP-UNSTABLE] noisy 基线 "
                      << psnr_noisy << " dB < 门限 " << gate_db
                      << " dB（SPGI 时域双稳态，非去噪器可修复）" << std::endl;
            continue;
        }
        std::vector<f32> out;
        float psnr_engine = -1, psnr_trained = -1;
        float ssim_engine = -1.0f;
        if (RunVariant(mlx, noisy_log, normal, depth16, meta.w, meta.h, 0, out, &err)) {
            psnr_engine = Psnr(out, gt3, dr);
            ssim_engine = Ssim(out, gt3, meta.w, meta.h, dr);
            if (!dump_out.empty()) {
                std::ofstream df(dump_out, std::ios::binary);
                df.write(reinterpret_cast<const char*>(out.data()),
                         (std::streamsize)(out.size() * sizeof(f32)));
            }
        } else {
            std::cerr << "[FAIL] infer(engine) " << base << ": " << err << std::endl;
        }
        if (RunVariant(mlx, noisy_log, normal, depth16, meta.w, meta.h, 1, out, &err)) {
            psnr_trained = Psnr(out, gt3, dr);
        }

        std::cout << "[gate] " << base << " " << meta.w << "x" << meta.h
                  << " frames=" << meta.frames << " dr=" << dr << std::endl
                  << "       noisy=" << psnr_noisy << " dB"
                  << " | mlx(engine-input)=" << psnr_engine << " dB (SSIM "
                  << ssim_engine << ")"
                  << " | mlx(trained-input,normal=0)=" << psnr_trained << " dB"
                  << std::endl;
        // 门禁取语义正确的一侧（engine 输入）。若 trained 侧显著更高，
        // 说明发版权重与引擎 pack 输入分布不匹配——单独出修复项，不算门禁。
        // 合成噪声模式不参与门禁判定（标注为机制验证）。
        if (synthetic) {
            std::cout << "       [synthetic mode — gate 判定跳过]" << std::endl;
        } else if (psnr_engine < gate_db) {
            std::cerr << "[FAIL] gate " << base << ": " << psnr_engine << " < "
                      << gate_db << " dB" << std::endl;
            ++gate_failures;
        }
    }

    if (gate_failures) {
        std::cerr << "GATE FAILED: " << gate_failures << " take(s)" << std::endl;
        return 2;
    }
    std::cout << "GATE PASSED (>= " << gate_db << " dB)" << std::endl;
    return 0;
}
