#pragma once
#include "MetalCommon.h"
#include "Engine/Graphics/RHI/Core/RHIDescriptorSet.h"

namespace primal::graphics::rhi {

class MetalDevice;
class MetalDescriptorSetLayout;

struct MetalDescriptorBinding {
    uint32_t binding;
    DescriptorType type;
    uint32_t count;
    ShaderStage stageFlags;
    
    utl::vector<ResourceHandle> resources;
    utl::vector<SamplerHandle> samplers;
    utl::vector<uint64_t> bufferOffsets;
};

class MetalDescriptorSet : public RHIDescriptorSet {
public:
    MetalDescriptorSet(MetalDevice& device, const DescriptorSetDesc& desc);
    ~MetalDescriptorSet() override;
    
    bool Initialize() override;
    void Destroy() override;
    bool updateDataImpl(const void* data, uint64_t size, uint64_t offset) override;
    
    void* mapImpl(uint64_t offset, uint64_t size) override;
    void unmapImpl() override;
    
    void Update(const WriteDescriptorSet* writes, uint32_t count);
    
    const utl::vector<MetalDescriptorBinding>& GetBindings() const { return bindings_; }

private:
    utl::vector<MetalDescriptorBinding> bindings_;
};

}
