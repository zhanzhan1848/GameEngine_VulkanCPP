#pragma once

#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Common/CoreLogStream.h"

namespace primaltest {

// The Sponza fixture inspects and occasionally resets GPU-produced counters
// between frames. Discrete D3D12 requires separate UAV and readback resources.
class ProbeDiagnosticReadback {
    using Device = primal::graphics::rhi::RHIDeviceBase;
    using Handle = primal::graphics::rhi::ResourceHandle;
public:
    explicit ProbeDiagnosticReadback(Device* device) : device_(device) {}
    ProbeDiagnosticReadback(const ProbeDiagnosticReadback&) = delete;
    ProbeDiagnosticReadback& operator=(const ProbeDiagnosticReadback&) = delete;

    void* Map(Handle source) {
        using namespace primal::graphics::rhi;
        if (device_->GetPlatform() != RHIPlatform::D3D12) return device_->MapBuffer(source);
        if (staging_ != handles::INVALID_RESOURCE || !device_->GetBufferByteSize(source, size_) || !size_)
            return nullptr;
        BufferDesc desc{};
        desc.size = size_;
        desc.type = BufferType::Raw;
        desc.usage = desc.memoryUsage = GPUMemoryUsage::Readback;
        desc.name = "SPGI diagnostic readback";
        staging_ = device_->CreateBuffer(desc);
        if (staging_ == handles::INVALID_RESOURCE) return nullptr;
        const auto handle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        auto* cmd = GetCommandBuffer(handle);
        bool complete = false;
        if (cmd && cmd->Begin()) {
            cmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::Transfer,
                               AccessFlag::ShaderWrite, AccessFlag::TransferRead);
            cmd->CopyBuffer(source, staging_, 0, 0, size_);
            complete = cmd->End() && cmd->Submit() && cmd->WaitForCompletion();
        }
        if (handle != handles::INVALID_COMMAND_BUFFER) device_->DestroyCommandBuffer(handle);
        mapping_ = complete ? device_->MapBuffer(staging_) : nullptr;
        if (!mapping_) Release();
        return mapping_;
    }

    void Unmap(Handle source, u64 changedBytes = 0) {
        using namespace primal::graphics::rhi;
        if (device_->GetPlatform() != RHIPlatform::D3D12) {
            device_->UnmapBuffer(source);
            return;
        }
        if (changedBytes && (changedBytes > size_ || !device_->UpdateBufferData(source, mapping_, changedBytes)))
            PRIMAL_LOG_STREAM(Error, "Render") << "SPGI diagnostic counter reset failed";
        Release();
    }
    ~ProbeDiagnosticReadback() { Release(); }
private:
    void Release() {
        using namespace primal::graphics::rhi;
        if (mapping_) device_->UnmapBuffer(staging_);
        if (staging_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(staging_);
        mapping_ = nullptr;
        staging_ = handles::INVALID_RESOURCE;
    }
    Device* device_;
    Handle staging_ = primal::graphics::rhi::handles::INVALID_RESOURCE;
    u64 size_ = 0;
    void* mapping_ = nullptr;
};

} // namespace primaltest
