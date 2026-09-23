/**
 * @file NeuralNRBackend.cpp
 * @brief NR shim 封装实现（dlopen + 生命周期；探测序列同 NeuralMLXBackend）
 */
#include "NeuralNRBackend.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

namespace primal::graphics::neural {

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

std::string NeuralNRBackend::FindDylib() {
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

std::string NeuralNRBackend::FindPackage() {
    if (const char* e = std::getenv("PRIMAL_NEURAL_NR_PACKAGE")) {
        if (*e) return e;
    }
    const char* rels[] = {
        "weights/nr/NeuralRendering.dlssmodel",
    };
    for (const char* rel : rels) {
        std::string p = CandidateWithRoot(rel);
        std::string manifest = p + "/manifest.json";
        if (FILE* f = fopen(manifest.c_str(), "rb")) {
            fclose(f);
            return p;
        }
    }
    return {};
}

bool NeuralNRBackend::Initialize(const std::string& package_path,
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
    fn_create_ex_ = reinterpret_cast<FnCreateEx>(sym("neural_nr_create_ex"));
    fn_destroy_ = reinterpret_cast<FnDestroy>(sym("neural_nr_destroy"));
    fn_infer_ = reinterpret_cast<FnInfer>(sym("neural_nr_infer"));
    if (!fn_create_ex_ || !fn_destroy_ || !fn_infer_) {
        return fail("shim missing neural_nr_* symbols（需重建 shim：swift build -c release）");
    }

    package_path_ = package_path.empty() ? FindPackage() : package_path;
    if (package_path_.empty()) {
        return fail("NR .dlssmodel package not found (weights/nr/NeuralRendering.dlssmodel)");
    }
    // flags bit0 = metalFused（f16+块编译；RepeatCheck 实测较 eager ~5×），
    // env PRIMAL_NEURAL_NR_FUSED=0 可回参考精度。
    const char* fusedEnv = std::getenv("PRIMAL_NEURAL_NR_FUSED");
    const std::int32_t flags = (fusedEnv && fusedEnv[0] == '0') ? 0 : 1;
    char errbuf[256] = {};
    model_ = fn_create_ex_(package_path_.c_str(), flags, errbuf, sizeof(errbuf));
    if (!model_) {
        return fail(std::string("shim nr create failed: ") + errbuf);
    }
    if (err) err->clear();
    return true;
}

void NeuralNRBackend::Shutdown() {
    if (model_ && fn_destroy_) fn_destroy_(model_);
    model_ = nullptr;
    if (dylib_) dlclose(dylib_);
    dylib_ = nullptr;
    fn_create_ex_ = nullptr;
    fn_infer_ = nullptr;
    fn_destroy_ = nullptr;
}

bool NeuralNRBackend::Infer(const f32* planes7, u32 width, u32 height,
                            u32 frame_index, f32* out_rgb, std::string* err) {
    if (!model_ || !fn_infer_) {
        if (err) *err = "nr backend not initialized";
        return false;
    }
    char errbuf[256] = {};
    const std::int32_t rc = fn_infer_(model_, planes7,
                                      std::int32_t(width), std::int32_t(height),
                                      std::int32_t(frame_index), out_rgb,
                                      errbuf, sizeof(errbuf));
    if (rc != 0) {
        if (err) *err = std::string("shim nr infer rc=") + std::to_string(rc) +
                        (errbuf[0] ? (": " + std::string(errbuf)) : "");
        return false;
    }
    if (err) err->clear();
    return true;
}

}  // namespace primal::graphics::neural
