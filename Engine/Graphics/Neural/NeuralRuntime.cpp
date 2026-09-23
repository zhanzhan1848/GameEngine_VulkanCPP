/**
 * @file NeuralRuntime.cpp
 * @brief NeuralRuntime 实现：权重整块上传为 host-visible StorageBuffer
 */
#include "NeuralRuntime.h"

#include <cstring>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::neural {

using namespace rhi;

bool NeuralRuntime::Initialize(RHIDeviceBase* device, const NeuralNetwork& net) {
    Shutdown();
    device_ = device;
    if (!device || net.table.empty() || net.weights.empty()) return false;
    if (net.layers.size() > kMaxLayers) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralRuntime] layer count "
                  << net.layers.size() << " > kMaxLayers " << kMaxLayers << std::endl;
        return false;
    }
    for (const LayerDesc& d : net.layers) {
        if (d.in_dim > kMaxLayerDim || d.out_dim > kMaxLayerDim) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralRuntime] layer dim "
                      << d.in_dim << "x" << d.out_dim << " > kMaxLayerDim "
                      << kMaxLayerDim << std::endl;
            return false;
        }
    }

    // params buffer = [table][weights] 单块 f32 平铺（shader 单 SSBO 视图）。
    table_float_count_ = (u32)net.table.size();
    total_float_count_ = table_float_count_ + (u32)net.weights.size();

    BufferDesc desc{};
    desc.size = (u64)total_float_count_ * 4ull;
    desc.type = BufferType::Structured;
    desc.memoryUsage = GPUMemoryUsage::Dynamic;   // 调试路径：host-visible 直写
    desc.usage = GPUMemoryUsage::Dynamic;
    desc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
    desc.name = "Neural_Params";
    params_buffer_ = device->CreateBuffer(desc);
    if (params_buffer_ == handles::INVALID_RESOURCE) return false;

    if (auto* dst = static_cast<f32*>(device->MapBuffer(params_buffer_))) {
        std::memcpy(dst, net.table.data(), (size_t)table_float_count_ * 4);
        std::memcpy(dst + table_float_count_, net.weights.data(),
                    (size_t)net.weights.size() * 4);
        device->UnmapBuffer(params_buffer_);
    } else {
        PRIMAL_LOG_STREAM(Error, "Render") << "[NeuralRuntime] map failed" << std::endl;
        Shutdown();
        return false;
    }
    PRIMAL_LOG_STREAM(Info, "Render") << "[NeuralRuntime] params buffer ready: "
              << total_float_count_ << " f32 (" << (total_float_count_ * 4ull) << " B)"
              << std::endl;
    return true;
}

void NeuralRuntime::Shutdown() {
    if (device_ && params_buffer_ != handles::INVALID_RESOURCE) {
        device_->DestroyBuffer(params_buffer_);
    }
    params_buffer_ = handles::INVALID_RESOURCE;
    table_float_count_ = 0;
    total_float_count_ = 0;
}

}  // namespace primal::graphics::neural
