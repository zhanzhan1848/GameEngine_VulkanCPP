#pragma once
#include "MetalCommon.h"
#include "Engine/Graphics/RHI/Core/RHIDescriptorSet.h"

namespace primal::graphics::rhi {

class MetalDevice;
class MetalDescriptorSetLayout;

struct MetalDescriptorBinding {
    u32 binding;
    DescriptorType type;
    u32 count;
    ShaderStage stageFlags;
    
    utl::vector<ResourceHandle> resources;
    utl::vector<SamplerHandle> samplers;
    utl::vector<u64> bufferOffsets;
};

class MetalDescriptorSet : public RHIDescriptorSet {
public:
    MetalDescriptorSet(MetalDevice& device, const DescriptorSetDesc& desc);
    ~MetalDescriptorSet() override;
    
    bool Initialize() override;
    void Destroy() override;
    bool updateDataImpl(const void* data, u64 size, u64 offset) override;
    
    void* mapImpl(u64 offset, u64 size) override;
    void unmapImpl() override;
    
    void Update(const WriteDescriptorSet* writes, u32 count);
    
    const utl::vector<MetalDescriptorBinding>& GetBindings() const { return bindings_; }

private:
    utl::vector<MetalDescriptorBinding> bindings_;
};

}
