/**
 * @file NeuralMLXBackend.h
 * @brief MLX 推理后端的 C ABI 封装（dlopen，可选依赖，不可用即降级）
 *
 * 路线修正（Docs/2026-09-15-neural-mlx-backend-plan.md §1.2 路径 A）：
 * Swift shim（Tools/NeuralMLXShim）包装 MLX 推理图并暴露 C ABI；引擎侧
 * 运行时 dlopen，缺库/加载失败/推理失败时调用方降级回裸 Metal kernel
 * （NeuralConv 路径保留为 fallback，见计划 §6）。
 *
 * 数据契约（与 shim 侧注释逐字对应）：
 *   - 权重：safetensors（pnn_to_safetensors.py 从 .pnn v2 转出）
 *   - 输入/输出：平面 CHW [c][y][x] f32 连续
 *   - infer flags bit0 = expm1 输出（parity 模式）；0 = log 域输出
 *     （引擎消费端执行 expm1，与 kernel 路径现行约定一致）
 *
 * ABI 纪律：本头文件不进 EngineDLL 导出面——MLX 后端是渲染进程内部
 * 实现细节，非编辑器/CLI 契约。
 */
#pragma once

#include <string>

#include "Graphics/Neural/NeuralWeights.h"

namespace primal::graphics::neural {

class NeuralMLXBackend {
public:
    /// 加载 shim 动态库并创建模型。dylib_path 为空按默认候选探测；
    /// weights_path 为空按默认候选探测。失败返回 false（原因写入 err）。
    bool Initialize(const std::string& weights_path = {},
                    const std::string& dylib_path = {}, std::string* err = nullptr);
    void Shutdown();

    bool IsAvailable() const { return model_ != nullptr; }

    /// 前向。planes CHW f32。flags bit0 = expm1。失败返回 false 并写 err。
    bool Infer(const f32* planes_in, u32 in_c, u32 width, u32 height,
               f32* planes_out, u32 out_c, u32 flags, std::string* err = nullptr);

    u32 InputDim() const { return input_dim_; }
    u32 OutputDim() const { return output_dim_; }
    const std::string& LoadedWeightsPath() const { return weights_path_; }
    const std::string& LoadedDylibPath() const { return dylib_path_; }

    /// 默认探测序列中的 shim 库路径（Initialize 之前可查 env 覆盖）。
    static std::string FindDylib();
    /// 默认探测序列中的权重路径。
    static std::string FindWeights();

private:
    void* dylib_ = nullptr;
    void* model_ = nullptr;
    u32 input_dim_ = 0;
    u32 output_dim_ = 0;
    std::string weights_path_;
    std::string dylib_path_;

    // shim 函数指针（C ABI）
    using FnApiVersion = std::int32_t (*)();
    using FnCreate = void* (*)(const char*, char*, std::int32_t);
    using FnDestroy = void (*)(void*);
    using FnInfer = std::int32_t (*)(void*, const f32*, std::int32_t, std::int32_t,
                                     std::int32_t, f32*, std::int32_t,
                                     std::int32_t, char*, std::int32_t);
    using FnCapabilities = std::int32_t (*)();
    FnCreate fn_create_ = nullptr;
    FnDestroy fn_destroy_ = nullptr;
    FnInfer fn_infer_ = nullptr;
    FnCapabilities fn_capabilities_ = nullptr;
};

}  // namespace primal::graphics::neural
