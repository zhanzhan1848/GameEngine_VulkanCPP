/**
 * @file NeuralNRBackend.h
 * @brief NR transformer（DLSS-dlssnr 提取模型）MLX shim 的 C ABI 封装
 *
 * 与 NeuralFrameGenBackend 同范式：dlopen libNeuralMLXShim.dylib，绑
 * neural_nr_create_ex/infer/destroy（Tools/NeuralMLXShim NeuralRender.swift）。
 * 数据契约：planes7 CHW [log1p(gi)×3, normal×3, depth/16] f32 → LDR rgb CHW。
 * ABI 纪律：不进 EngineDLL 导出面。
 */
#pragma once

#include <string>

#include "Graphics/Neural/NeuralWeights.h"

namespace primal::graphics::neural {

class NeuralNRBackend {
public:
    /// 加载 shim 并创建 NR 模型。weights_path = .dlssmodel 包目录。
    bool Initialize(const std::string& package_path = {},
                    const std::string& dylib_path = {}, std::string* err = nullptr);
    void Shutdown();

    bool IsAvailable() const { return model_ != nullptr; }

    /// 前向。planes CHW 7ch f32 → out CHW 3ch f32 LDR [0,1]。失败返回 false。
    bool Infer(const f32* planes7, u32 width, u32 height, u32 frame_index,
               f32* out_rgb, std::string* err = nullptr);

    const std::string& LoadedPackagePath() const { return package_path_; }
    const std::string& LoadedDylibPath() const { return dylib_path_; }

    static std::string FindDylib();
    static std::string FindPackage();

private:
    void* dylib_ = nullptr;
    void* model_ = nullptr;
    std::string package_path_;
    std::string dylib_path_;

    using FnCreateEx = void* (*)(const char*, std::int32_t, char*, std::int32_t);
    using FnDestroy = void (*)(void*);
    using FnInfer = std::int32_t (*)(void*, const f32*, std::int32_t, std::int32_t,
                                     std::int32_t, f32*, char*, std::int32_t);
    FnCreateEx fn_create_ex_ = nullptr;
    FnDestroy fn_destroy_ = nullptr;
    FnInfer fn_infer_ = nullptr;
};

}  // namespace primal::graphics::neural
