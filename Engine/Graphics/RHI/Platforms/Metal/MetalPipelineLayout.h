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
    bool updateDataImpl(const void* /*data*/, u64 /*size*/, u64 /*offset*/) override { return false; }
    void* mapImpl(u64 /*offset*/, u64 /*size*/) override { return nullptr; }
    void unmapImpl() override {}
};

}
