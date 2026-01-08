#pragma once
#include "MetalCommon.h"
#include "Engine/Graphics/RHI/Core/RHIDescriptorSetLayout.h"

namespace primal::graphics::rhi {

class MetalDevice;

class MetalDescriptorSetLayout : public RHIDescriptorSetLayout {
public:
    MetalDescriptorSetLayout(MetalDevice& device, const DescriptorSetLayoutDesc& desc);
    ~MetalDescriptorSetLayout() override;
    
    bool Initialize() override { return true; }
    void Destroy() override;
    bool updateDataImpl(const void* data, uint64_t size, uint64_t offset) override { return false; }
    void* mapImpl(uint64_t offset, uint64_t size) override { return nullptr; }
    void unmapImpl() override {}
    
    const DescriptorSetLayoutBinding* GetBinding(uint32_t binding) const;
};

}
