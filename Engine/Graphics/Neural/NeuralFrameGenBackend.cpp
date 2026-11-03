/**
 * @file NeuralFrameGenBackend.cpp
 * @brief FG shim 封装实现（dlopen + 生命周期；探测序列同 NeuralMLXBackend）
 *
 * R3（Windows 首编）：MLX/CoreML shim 是 macOS 专属依赖，<dlfcn.h> 在 MSVC
 * 下不存在。实现整体 APPLE 门控；非 Apple 平台编译降级 stub（Initialize 恒
 * false → 模块降级，与 shim 缺库时的运行时降级同语义）。头文件保持全平台
 * 可编（NeuralFrameGenModule.h 按值持有本类成员）。CMake glob 不变。
 */
#include "NeuralFrameGenBackend.h"

#if defined(__APPLE__)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#endif

// R4（Windows 四测修复）：namespace 必须包裹真实现与 stub 两个分支——R3 版本
// 把开括号放在 __APPLE__ 分支内、闭括号在 #endif 之后，非 Apple 编译时 stub
// 定义裸露全局作用域（MSVC C2653 "NeuralFrameGenBackend 不是类或命名空间名
// 称" + 级联 C2059/C2143）。头文件 include 不能进 namespace，故拆两段 #if。
namespace primal::graphics::neural {

#if defined(__APPLE__)

namespace {

std::string CandidateWithRoot(const std::string& rel) {
    std::string pfx;
    for (int i = 0; i < 6; ++i) {
        pfx += "../";
        if (FILE* f = fopen((pfx + rel).c_str(), "rb")) {
            fclose(f);
            return pfx + rel;
        }
    }
    return rel;
}

}  // namespace

std::string NeuralFrameGenBackend::FindDylib() {
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

std::string NeuralFrameGenBackend::FindWeights() {
    if (const char* e = std::getenv("PRIMAL_FRAMEGEN_WEIGHTS")) {
        if (*e) return e;
    }
    const char* rels[] = {
        "weights/fg/framegen.safetensors",
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

bool NeuralFrameGenBackend::Initialize(const std::string& weights_path,
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
    // 库与降噪后端共享：dlopen 引用计数合并，dlclose 由双方各自调用安全。

    auto sym = [&](const char* name) { return dlsym(dylib_, name); };
    fn_create_ = reinterpret_cast<FnCreate>(sym("neural_fg_create"));
    fn_destroy_ = reinterpret_cast<FnDestroy>(sym("neural_fg_destroy"));
    fn_interpolate_ = reinterpret_cast<FnInterpolate>(sym("neural_fg_interpolate"));
    if (!fn_create_ || !fn_destroy_ || !fn_interpolate_) {
        return fail("shim missing neural_fg_* symbols");
    }

    weights_path_ = weights_path.empty() ? FindWeights() : weights_path;
    if (weights_path_.empty()) {
        return fail("framegen safetensors not found (mlxdlss-weights extract-fg)");
    }
    char errbuf[256] = {};
    model_ = fn_create_(weights_path_.c_str(), errbuf, sizeof(errbuf));
    if (!model_) {
        return fail(std::string("shim fg create failed: ") + errbuf);
    }
    if (err) err->clear();
    return true;
}

void NeuralFrameGenBackend::Shutdown() {
    if (model_ && fn_destroy_) fn_destroy_(model_);
    model_ = nullptr;
    if (dylib_) dlclose(dylib_);
    dylib_ = nullptr;
    fn_create_ = nullptr;
    fn_interpolate_ = nullptr;
    fn_destroy_ = nullptr;
}

bool NeuralFrameGenBackend::Interpolate(const f32* frame_a, const f32* frame_b,
                                        u32 width, u32 height, f32 phase,
                                        f32* out_frame, std::string* err) {
    if (!model_ || !fn_interpolate_) {
        if (err) *err = "fg backend not initialized";
        return false;
    }
    char errbuf[256] = {};
    const std::int32_t rc = fn_interpolate_(model_, frame_a, frame_b,
                                            std::int32_t(width), std::int32_t(height),
                                            phase, out_frame, errbuf, sizeof(errbuf));
    if (rc != 0) {
        if (err) *err = std::string("shim fg interpolate rc=") + std::to_string(rc) +
                        (errbuf[0] ? (": " + std::string(errbuf)) : "");
        return false;
    }
    if (err) err->clear();
    return true;
}

#else  // !defined(__APPLE__)

// 非 Apple 降级 stub：IsAvailable() 恒 false；NeuralFrameGenModule 按
// Initialize 失败降级（不注册 FG pass）。
std::string NeuralFrameGenBackend::FindDylib() { return {}; }
std::string NeuralFrameGenBackend::FindWeights() { return {}; }

bool NeuralFrameGenBackend::Initialize(const std::string&, const std::string&,
                                       std::string* err) {
    if (err) *err = "NeuralFrameGenBackend: MLX shim is macOS-only (libNeuralMLXShim.dylib)";
    return false;
}

void NeuralFrameGenBackend::Shutdown() {}

bool NeuralFrameGenBackend::Interpolate(const f32*, const f32*, u32, u32, f32,
                                        f32*, std::string* err) {
    if (err) *err = "NeuralFrameGenBackend: not available on this platform";
    return false;
}

#endif  // defined(__APPLE__)

}  // namespace primal::graphics::neural
