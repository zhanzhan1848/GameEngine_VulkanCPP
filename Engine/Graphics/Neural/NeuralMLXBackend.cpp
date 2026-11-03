/**
 * @file NeuralMLXBackend.cpp
 * @brief MLX 后端封装实现：dlopen + ABI 版本握手 + 生命周期
 *
 * 默认探测序列刻意覆盖三种部署形态：仓库源树（swift build 产物）、
 * 可执行文件旁（安装/CI 布局）、显式 env（排障与 A/B）。找不到库不是
 * 错误——返回 false，调用方走 kernel fallback（计划 §6：裸 kernel 不删）。
 *
 * R3（Windows 首编）：MLX/CoreML shim 是 macOS 专属依赖，<dlfcn.h> 在 MSVC
 * 下不存在。实现整体 APPLE 门控；非 Apple 平台编译降级 stub（Initialize 恒
 * false → NeuralDenoiseModule 走 kernel fallback，与 shim 缺库时的运行时
 * 降级同语义，见 NeuralDenoiseModule.cpp "降级 kernel" 分支）。头文件保持
 * 全平台可编（NeuralDenoiseModule.h 按值持有本类成员）。CMake glob 不变。
 */
#include "NeuralMLXBackend.h"

#if defined(__APPLE__)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

namespace primal::graphics::neural {

namespace {

constexpr std::int32_t kExpectedApiVersion = 1;

/// 仓库根回退（与 NeuralDenoiseModule::LoadWithFallback 同惯例）。
std::string CandidateWithRoot(const std::string& rel) {
    std::string pfx;
    for (int i = 0; i < 6; ++i) {
        pfx += "../";
        if (FILE* f = fopen((pfx + rel).c_str(), "rb")) {
            fclose(f);
            return pfx + rel;
        }
    }
    return rel;   // 最后回 cwd 相对
}

}  // namespace

std::string NeuralMLXBackend::FindDylib() {
    if (const char* e = std::getenv("PRIMAL_NEURAL_MLX_LIB")) {
        if (*e) return e;
    }
    const char* rels[] = {
        "Tools/NeuralMLXShim/.build/release/libNeuralMLXShim.dylib",
        "libNeuralMLXShim.dylib",
    };
    for (const char* rel : rels) {
        std::string p = CandidateWithRoot(rel);
        if (FILE* f = fopen(p.c_str(), "rb")) {
            fclose(f);
            return p;
        }
    }
    return {};
}

std::string NeuralMLXBackend::FindWeights() {
    if (const char* e = std::getenv("PRIMAL_NEURAL_MLX_WEIGHTS")) {
        if (*e) return e;
    }
    const char* rels[] = {
        "EngineTest/assets/Neural/denoiser.safetensors",
    };
    for (const char* rel : rels) {
        std::string p = CandidateWithRoot(rel);
        if (FILE* f = fopen(p.c_str(), "rb")) {
            fclose(f);
            return p;
        }
    }
    return {};
}

bool NeuralMLXBackend::Initialize(const std::string& weights_path,
                                  const std::string& dylib_path, std::string* err) {
    Shutdown();
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        Shutdown();
        return false;
    };

    dylib_path_ = dylib_path.empty() ? FindDylib() : dylib_path;
    if (dylib_path_.empty()) {
        return fail("mlx shim dylib not found (build Tools/NeuralMLXShim: "
                    "swift build -c release)");
    }
    dylib_ = dlopen(dylib_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!dylib_) {
        const char* e = dlerror();
        return fail(std::string("dlopen failed: ") + (e ? e : "?") + " (" + dylib_path_ + ")");
    }

    auto sym = [&](const char* name) { return dlsym(dylib_, name); };
    auto api_version = reinterpret_cast<FnApiVersion>(sym("neural_mlx_api_version"));
    fn_create_ = reinterpret_cast<FnCreate>(sym("neural_mlx_create"));
    fn_destroy_ = reinterpret_cast<FnDestroy>(sym("neural_mlx_destroy"));
    fn_infer_ = reinterpret_cast<FnInfer>(sym("neural_mlx_infer_f32"));
    fn_capabilities_ = reinterpret_cast<FnCapabilities>(sym("neural_mlx_capabilities"));
    if (!api_version || !fn_create_ || !fn_destroy_ || !fn_infer_) {
        return fail("shim missing required symbols");
    }
    if (api_version() != kExpectedApiVersion) {
        return fail("shim api version mismatch: " + std::to_string(api_version()) +
                    " (expected " + std::to_string(kExpectedApiVersion) + ")");
    }

    weights_path_ = weights_path.empty() ? FindWeights() : weights_path;
    if (weights_path_.empty()) {
        return fail("safetensors not found (run ContentTools/neural/"
                    "pnn_to_safetensors.py)");
    }
    char errbuf[256] = {};
    model_ = fn_create_(weights_path_.c_str(), errbuf, sizeof(errbuf));
    if (!model_) {
        return fail(std::string("shim create failed: ") + errbuf);
    }

    // 零尺寸探测前向拿不到维度——safetensors 元数据在 shim 侧；引擎侧以
    // 调用参数为准，这里仅记录路径。维度校验推迟到 Infer。
    if (err) err->clear();
    return true;
}

void NeuralMLXBackend::Shutdown() {
    if (model_ && fn_destroy_) fn_destroy_(model_);
    model_ = nullptr;
    if (dylib_) dlclose(dylib_);
    dylib_ = nullptr;
    fn_create_ = nullptr;
    fn_infer_ = nullptr;
    fn_destroy_ = nullptr;
    fn_capabilities_ = nullptr;
}

bool NeuralMLXBackend::Infer(const f32* planes_in, u32 in_c, u32 width, u32 height,
                             f32* planes_out, u32 out_c, u32 flags, std::string* err) {
    if (!model_ || !fn_infer_) {
        if (err) *err = "mlx backend not initialized";
        return false;
    }
    char errbuf[256] = {};
    const std::int32_t rc = fn_infer_(model_, planes_in,
                                      std::int32_t(in_c), std::int32_t(width),
                                      std::int32_t(height), planes_out,
                                      std::int32_t(out_c), std::int32_t(flags),
                                      errbuf, sizeof(errbuf));
    if (rc != 0) {
        if (err) *err = std::string("shim infer rc=") + std::to_string(rc) +
                        (errbuf[0] ? (": " + std::string(errbuf)) : "");
        return false;
    }
    if (err) err->clear();
    return true;
}

#else  // !defined(__APPLE__)

// 非 Apple 降级 stub：IsAvailable() 恒 false；NeuralDenoiseModule 按
// Initialize 失败降级回裸 Metal/Vulkan kernel 路径。
std::string NeuralMLXBackend::FindDylib() { return {}; }
std::string NeuralMLXBackend::FindWeights() { return {}; }

bool NeuralMLXBackend::Initialize(const std::string&, const std::string&,
                                  std::string* err) {
    if (err) *err = "NeuralMLXBackend: MLX shim is macOS-only (libNeuralMLXShim.dylib)";
    return false;
}

void NeuralMLXBackend::Shutdown() {}

bool NeuralMLXBackend::Infer(const f32*, u32, u32, u32, f32*, u32, u32,
                             std::string* err) {
    if (err) *err = "NeuralMLXBackend: not available on this platform";
    return false;
}

#endif  // defined(__APPLE__)

}  // namespace primal::graphics::neural
