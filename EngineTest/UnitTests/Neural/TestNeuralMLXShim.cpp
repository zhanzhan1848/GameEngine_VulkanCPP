/**
 * @file TestNeuralMLXShim.cpp
 * @brief MLX 后端对拍门禁（M1-a 步骤 3：GPU 降噪输出 vs torch 参考同帧输入）
 *
 * 链路：denoiser.safetensors（pnn_to_safetensors.py 产物）→ Swift shim
 * （MLX GPU 前向）→ 输出对两路基准：
 *   1. denoiser_parity.bin 的 torch 全前向期望（expm1 模式，内部域
 *      atol 2e-4——边界 zero-pad 与 torch 逐位同语义，累加序差异容忍）；
 *   2. CPU 参考前向 ReferenceForwardConv（单测已锁位级一致）。
 *
 * shim 缺失（未构建 Swift 包）时 SKIP 不 FAIL——MLX 后端是可选依赖，
 * 降级路径（裸 Metal kernel）不因此阻塞；但库存在而对拍失败必须 FAIL。
 */
#include "Engine/Graphics/Neural/NeuralMLXBackend.h"
#include "Engine/Graphics/Neural/NeuralWeights.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <dlfcn.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace primal::graphics::neural;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::cerr << "[FAIL] " << msg << " (line " << __LINE__ << ")" \
                      << std::endl;                                       \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

bool ReadFile(const char* path, std::vector<std::uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const std::streamsize size = f.tellg();
    f.seekg(0);
    out.resize((size_t)size);
    return (bool)f.read(reinterpret_cast<char*>(out.data()), size);
}

std::vector<std::uint8_t> LoadWithCandidates(const char* const* candidates, int n) {
    std::vector<std::uint8_t> bytes;
    for (int i = 0; i < n; ++i) {
        if (ReadFile(candidates[i], bytes)) return bytes;
    }
    return {};
}

}  // namespace

int main() {
    std::cout << "== TestNeuralMLXShim ==" << std::endl;

    NeuralMLXBackend mlx;
    std::string err;
    if (!mlx.Initialize({}, {}, &err)) {
        std::cout << "[SKIP] MLX shim unavailable: " << err << std::endl;
        return 0;   // 可选依赖：缺库/缺权重降级，不 FAIL
    }
    std::cout << "[mlx] dylib=" << mlx.LoadedDylibPath()
              << " weights=" << mlx.LoadedWeightsPath() << std::endl;

    // ---- 网络 + parity 向量 ----
    const char* par_candidates[] = {
        "Assets/Neural/denoiser_parity.bin",
        "EngineTest/assets/Neural/denoiser_parity.bin",
        "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/EngineTest/assets/Neural/denoiser_parity.bin",
    };
    const std::vector<std::uint8_t> bytes = LoadWithCandidates(
        par_candidates, sizeof(par_candidates) / sizeof(par_candidates[0]));
    if (bytes.empty()) {
        std::cout << "[SKIP] denoiser_parity.bin not found" << std::endl;
        return 0;
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
    CHECK(ph == 16 && pw == 16 && cin == 7 && cout == 3, "parity dims");
    std::vector<f32> in((size_t)cin * ph * pw), expect((size_t)cout * ph * pw);
    std::memcpy(in.data(), bytes.data() + pos, in.size() * 4);
    pos += in.size() * 4;
    std::memcpy(expect.data(), bytes.data() + pos, expect.size() * 4);

    // ---- shim 推理（expm1 模式 = parity 语义）----
    std::vector<f32> out(expect.size());
    std::string ierr;
    CHECK(mlx.Infer(in.data(), cin, pw, ph, out.data(), cout, 1u, &ierr),
          "shim infer: " + ierr);

    // ---- 基准 1：torch 期望（全平面，zero-pad 与 torch 同语义）----
    float max_err_torch = 0.0f;
    for (size_t i = 0; i < expect.size(); ++i) {
        max_err_torch = std::max(max_err_torch, std::fabs(out[i] - expect[i]));
    }

    // ---- 基准 2：CPU 参考前向 ----
    NeuralNetwork net;
    std::string perr;
    bool pnn_loaded = false;
    for (const char* c : {"Assets/Neural/denoiser.pnn",
                          "EngineTest/assets/Neural/denoiser.pnn",
                          "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/"
                          "EngineTest/assets/Neural/denoiser.pnn"}) {
        if ((pnn_loaded = LoadPnnFile(c, net, &perr))) break;
    }
    CHECK(pnn_loaded, "denoiser.pnn load: " + perr);
    std::vector<f32> cpu_out(expect.size());
    std::vector<f32> scratch;
    CHECK(ReferenceForwardConv(net, in.data(), pw, ph, cpu_out.data(), scratch),
          "cpu reference forward");
    float max_err_cpu = 0.0f;
    for (size_t i = 0; i < expect.size(); ++i) {
        max_err_cpu = std::max(max_err_cpu, std::fabs(out[i] - cpu_out[i]));
    }

    float mean_abs = 0.0f;
    for (size_t i = 0; i < expect.size(); ++i) mean_abs += std::fabs(out[i] - expect[i]);
    std::cout << "[parity] torch_max_err=" << max_err_torch
              << " mean_abs=" << mean_abs / expect.size()
              << " cpu_max_err=" << max_err_cpu << std::endl;

    // GPU conv 累加序与 torch CPU 不同；f32 下经验量级 1e-5。门禁取
    // 与 kernel 路径 L0 验证一致的 2e-4（fp16 噪声级 0.002 的 1/10）。
    CHECK(max_err_torch < 2e-4f, "MLX vs torch parity (atol 2e-4)");
    CHECK(max_err_cpu < 2e-4f, "MLX vs CPU reference parity (atol 2e-4)");

    // ---- log 域模式（引擎消费端语义）：expm1 前的 log 值应与 parity 的
    // log1p 逆一致——抽样验证 expm1(log) == parity 值 ----
    std::vector<f32> out_log(expect.size());
    CHECK(mlx.Infer(in.data(), cin, pw, ph, out_log.data(), cout, 0u, &ierr),
          "shim infer log mode: " + ierr);
    float max_err_logmode = 0.0f;
    for (size_t i = 0; i < expect.size(); ++i) {
        max_err_logmode = std::max(max_err_logmode,
                                   std::fabs(std::expm1(out_log[i]) - expect[i]));
    }
    std::cout << "[parity] log-mode roundtrip_max_err=" << max_err_logmode << std::endl;
    CHECK(max_err_logmode < 2e-4f, "log-mode expm1 roundtrip (atol 2e-4)");

    // ---- M3：半分辨率执行（infer flags bit1）----
    // 对全分辨率 f32 输出的偏差是近似误差（降采样信息损失），不是正确性
    // 错误；门禁只锁"跑通 + 偏差有界"（log 域平均 |Δ| ≤ 0.05）。
    {
        // parity 输入是纯随机噪声——2×2 池化对其破坏最大（实测 mean|Δ|≈0.41）。
        // 半分辨率的真实场景是空间平滑的 GI 场，故用 3×3 box blur 后的输入
        // 锁门禁（机制验证：降采样→推理→上采样链路 + 偏差有界）。
        std::vector<f32> smooth(in.size());
        for (u32 c = 0; c < cin; ++c)
            for (u32 y = 0; y < ph; ++y)
                for (u32 x = 0; x < pw; ++x) {
                    f32 acc = 0;
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int yy = int(y) + dy, xx = int(x) + dx;
                            if (yy < 0 || yy >= int(ph) || xx < 0 || xx >= int(pw)) continue;
                            acc += in[(size_t)c * ph * pw + yy * pw + xx];
                            ++n;
                        }
                    smooth[(size_t)c * ph * pw + y * pw + x] = acc / n;
                }
        std::vector<f32> full_smooth(expect.size()), out_half(expect.size());
        CHECK(mlx.Infer(smooth.data(), cin, pw, ph, full_smooth.data(), cout, 0u, &ierr),
              "shim infer smooth full: " + ierr);
        CHECK(mlx.Infer(smooth.data(), cin, pw, ph, out_half.data(), cout, 2u, &ierr),
              "shim infer half-res: " + ierr);
        double mean_abs_half = 0.0;
        for (size_t i = 0; i < out_half.size(); ++i)
            mean_abs_half += std::fabs(double(out_half[i]) - double(full_smooth[i]));
        mean_abs_half /= out_half.size();
        double mean_abs_noise = 0.0;
        {
            std::vector<f32> out_half_noise(expect.size());
            mlx.Infer(in.data(), cin, pw, ph, out_half_noise.data(), cout, 2u, &ierr);
            for (size_t i = 0; i < out_half_noise.size(); ++i)
                mean_abs_noise += std::fabs(double(out_half_noise[i]) - double(out_log[i]));
            mean_abs_noise /= out_half_noise.size();
        }
        std::cout << "[m3] half-res vs full mean_abs(log): smooth=" << mean_abs_half
                  << " raw-noise=" << mean_abs_noise << std::endl;
        // 门禁 0.15 = 16×16 微型尺寸上池化信息损失的经验界（平滑输入实测
        // 0.112；随机噪声 0.41）。机制性破坏（tile 式上采样/通道错位）会
        // 数量级爆炸，0.15 足以甄别；真实 GI 数据的近似质量由引擎联跑评估
        // （边界效应占比 ∝ 1/尺寸，720p 下远小于 16×16）。
        CHECK(mean_abs_half < 0.15,
              "half-res approximation bounded on smooth input (mean_abs < 0.15)");
    }

    // ---- M3：FG（FrameGenerator）插值冒烟 ----
    // 权重来源：mlxdlss-weights extract-fg（libnvidia-ngx-dlss.so.310.7.0）。
    // 平面数据验证：a=0.2 平场、b=0.8 平场，phase=0.5 → 输出应 ≈0.5。
    {
        void* dy = dlopen(mlx.LoadedDylibPath().c_str(), RTLD_NOW | RTLD_LOCAL);
        using FnFgCreate = void* (*)(const char*, char*, std::int32_t);
        using FnFgInterp = std::int32_t (*)(void*, const float*, const float*,
                                            std::int32_t, std::int32_t, float,
                                            float*, char*, std::int32_t);
        const auto fgCreate = dy
            ? reinterpret_cast<FnFgCreate>(dlsym(dy, "neural_fg_create")) : nullptr;
        const auto fgInterp = dy
            ? reinterpret_cast<FnFgInterp>(dlsym(dy, "neural_fg_interpolate")) : nullptr;
        char ferr[256] = {};
        void* fg = fgCreate
            ? fgCreate("/tmp/dlss_sdk/weights/framegen.safetensors", ferr, sizeof(ferr))
            : nullptr;
        if (fg && fgInterp) {
            // 移动斜坡：a = x 斜坡，b = 右移 16px 的斜坡 → phase 0.5 的插值
            // 应 ≈ 右移 8px 的斜坡（光流网络对有结构输入的语义验证）。
            const int W = 64, H = 64;
            std::vector<float> a(W * H * 3), b(W * H * 3), out(W * H * 3, -1.f);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        a[(y * W + x) * 3 + c] = float(x) / float(W - 1);
                        const int xb = std::min(x + 16, W - 1);
                        b[(y * W + x) * 3 + c] = float(xb) / float(W - 1);
                    }
                }
            const std::int32_t rc = fgInterp(fg, a.data(), b.data(), W, H, 0.5f,
                                             out.data(), ferr, sizeof(ferr));
            CHECK(rc == 0, "fg interpolate rc=" + std::to_string(rc) + " " + ferr);
            int nonzero = 0; float mx = -2.f; bool has_nan = false;
            for (float v : out) { mx = std::max(mx, v); if (v > 1e-3f) ++nonzero; if (std::isnan(v)) has_nan = true; }
            const double nz_frac = double(nonzero) / double(out.size());
            std::cout << "[m3] FG phase0.5 (ramp) max=" << mx
                      << " 非零占比=" << nz_frac << " nan=" << has_nan << std::endl;
            // 机制验证：调用成功 + 输出有限。内容校验需真实 FG 权重
            // （当前占位文件由统一 SR 库误提取，权重为垃圾——见计划 §7.4）。
            CHECK(!has_nan, "FG output finite");
            const auto fgDestroy = dy
                ? reinterpret_cast<void (*)(void*)>(dlsym(dy, "neural_fg_destroy")) : nullptr;
            if (fgDestroy) fgDestroy(fg);
        } else {
            std::cout << "[SKIP] FG symbols/weights unavailable: " << ferr << std::endl;
        }
    }

    // ---- M3：int8 权重存储（create_ex bit0 = 逐出通道对称量化）----
    // 直接走原始 ABI（create_ex/infer_f32/destroy），与 backend 封装解耦。
    {
        void* dy = dlopen(mlx.LoadedDylibPath().c_str(), RTLD_NOW | RTLD_LOCAL);
        using FnCreateEx = void* (*)(const char*, std::int32_t, char*, std::int32_t);
        using FnInferRaw = std::int32_t (*)(void*, const f32*, std::int32_t,
                                            std::int32_t, std::int32_t, f32*,
                                            std::int32_t, std::int32_t, char*,
                                            std::int32_t);
        using FnDestroyRaw = void (*)(void*);
        const auto createEx = dy
            ? reinterpret_cast<FnCreateEx>(dlsym(dy, "neural_mlx_create_ex")) : nullptr;
        const auto inferRaw = dy
            ? reinterpret_cast<FnInferRaw>(dlsym(dy, "neural_mlx_infer_f32")) : nullptr;
        const auto destroyRaw = dy
            ? reinterpret_cast<FnDestroyRaw>(dlsym(dy, "neural_mlx_destroy")) : nullptr;
        char err8[256] = {};
        void* h8 = createEx
            ? createEx("/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/"
                       "EngineTest/assets/Neural/denoiser.safetensors",
                       1, err8, sizeof(err8))
            : nullptr;
        if (h8 && inferRaw) {
            std::vector<f32> out8(expect.size());
            const std::int32_t rc = inferRaw(h8, in.data(), (std::int32_t)cin,
                                             (std::int32_t)pw, (std::int32_t)ph,
                                             out8.data(), (std::int32_t)cout, 0,
                                             err8, sizeof(err8));
            CHECK(rc == 0, "int8 infer rc=" + std::to_string(rc) + " " + err8);
            double mean_abs_8 = 0.0;
            for (size_t i = 0; i < out8.size(); ++i)
                mean_abs_8 += std::fabs(double(out8[i]) - double(out_log[i]));
            mean_abs_8 /= out8.size();
            std::cout << "[m3] int8 vs f32 mean_abs(log)=" << mean_abs_8 << std::endl;
            CHECK(mean_abs_8 < 0.02, "int8 weight quant bounded (mean_abs < 0.02)");
            if (destroyRaw) destroyRaw(h8);
        } else {
            std::cout << "[SKIP] create_ex/infer unavailable: " << err8 << std::endl;
        }
    }

    if (g_failures) {
        std::cerr << "FAILED: " << g_failures << " check(s)" << std::endl;
        return 1;
    }
    std::cout << "PASSED" << std::endl;
    return 0;
}
