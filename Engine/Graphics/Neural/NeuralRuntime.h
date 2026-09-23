/**
 * @file NeuralRuntime.h
 * @brief 神经网络权重 → GPU 资源的最小组合层（RHI 之上，平台无关）
 *
 * M0 神经渲染基础设施 Layer B 的 GPU 半边。职责单一：把 NeuralNetwork 的
 * table + weights 拼成一个 f32 平铺 SSBO 并创建推理所需的资源视图。
 * 不含 pass 编排（模块层职责），不改 RHIDeviceBase（ABI 纪律）。
 *
 * 布局约定（与 MLPInferDebug.metal / MLPInfer.comp 的 params 缓冲一致）：
 *   params buffer = [ table (table_float_count 个 f32) ][ weights 原样平铺 ]。
 *   各层 W/B 在 CB 里的偏移 = table_float_count + 层内偏移（float 索引）。
 */
#pragma once

#include "Graphics/Neural/NeuralWeights.h"
#include "Graphics/RHI/Core/RHIDevice.h"

namespace primal::graphics::neural {

/// 与 shader 侧数组容量对齐（NeuralDebugCB.layers[8]、feat[64]）。
constexpr u32 kMaxLayers = 8;
constexpr u32 kMaxLayerDim = 64;

class NeuralRuntime {
public:
    bool Initialize(rhi::RHIDeviceBase* device, const NeuralNetwork& net);
    void Shutdown();

    bool IsValid() const { return params_buffer_ != rhi::handles::INVALID_RESOURCE; }
    rhi::ResourceHandle GetParamsBuffer() const { return params_buffer_; }
    u32 GetTableFloatCount() const { return table_float_count_; }
    u32 GetTotalFloatCount() const { return total_float_count_; }

private:
    rhi::RHIDeviceBase* device_ = nullptr;
    rhi::ResourceHandle params_buffer_{rhi::handles::INVALID_RESOURCE};
    u32 table_float_count_ = 0;
    u32 total_float_count_ = 0;
};

}  // namespace primal::graphics::neural
