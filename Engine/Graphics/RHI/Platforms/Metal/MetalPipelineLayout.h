#pragma once
#include "MetalCommon.h"
#include "Engine/Graphics/RHI/Core/RHIPipelineLayout.h"

namespace primal::graphics::rhi {

class MetalDevice;

class MetalPipelineLayout : public RHIPipelineLayout {
public:
    MetalPipelineLayout(MetalDevice& device, const PipelineLayoutDesc& desc);
    ~MetalPipelineLayout() override;
    
    bool Initialize() override { return true; }
    void Destroy() override;
    bool updateDataImpl(const void* data, uint64_t size, uint64_t offset) override { return false; }
    void* mapImpl(uint64_t offset, uint64_t size) override { return nullptr; }
    void unmapImpl() override {}
};

}
