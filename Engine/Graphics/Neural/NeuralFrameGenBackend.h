/**
 * @file NeuralFrameGenBackend.h
 * @brief 帧生成（FG）MLX shim 的 C ABI 封装（dlopen，可选依赖）
 *
 * 与 NeuralMLXBackend 同范式：dlopen libNeuralMLXShim.dylib，绑
 * neural_fg_create/interpolate/destroy（Tools/NeuralMLXShim FrameGen.swift）。
 * ABI 纪律：不进 EngineDLL 导出面——渲染进程内部实现细节。
 *
 * 数据契约（与 shim 注释逐字对应）：
 *   - 权重：weights/fg/framegen.safetensors（92 张量 fp16 dense conv）
 *   - 帧：NHWC [h][w][3] f32，[0,1]（LDR）；outFrame 同布局
 */
#pragma once

#include <string>

#include "Graphics/Neural/NeuralWeights.h"   // f32/u32 基础类型（同 NeuralMLXBackend）

namespace primal::graphics::neural {

class NeuralFrameGenBackend {
public:
    /// 加载 shim 动态库并创建 FrameGenerator。路径为空按默认候选探测。
    bool Initialize(const std::string& weights_path = {},
                    const std::string& dylib_path = {}, std::string* err = nullptr);
    void Shutdown();

    bool IsAvailable() const { return model_ != nullptr; }
    const std::string& LoadedWeightsPath() const { return weights_path_; }

    /// 插值两帧。frameA/B 与 out 均为 NHWC [h][w][3] f32 [0,1]。失败返回 false。
    bool Interpolate(const f32* frame_a, const f32* frame_b, u32 width, u32 height,
                     f32 phase, f32* out_frame, std::string* err = nullptr);

    static std::string FindDylib();
    static std::string FindWeights();

private:
    void* dylib_ = nullptr;
    void* model_ = nullptr;
    std::string weights_path_;
    std::string dylib_path_;

    using FnCreate = void* (*)(const char*, char*, std::int32_t);
    using FnDestroy = void (*)(void*);
    using FnInterpolate = std::int32_t (*)(void*, const f32*, const f32*,
                                           std::int32_t, std::int32_t, f32,
                                           f32*, char*, std::int32_t);
    FnCreate fn_create_ = nullptr;
    FnDestroy fn_destroy_ = nullptr;
    FnInterpolate fn_interpolate_ = nullptr;
};

}  // namespace primal::graphics::neural
