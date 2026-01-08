#pragma once

#include "MetalCommon.h"
#include "../../Core/RHITypes.h"
#include "../../Core/RHIResource.h"
#include <map>
#include <vector>

namespace primal::graphics::rhi {

class MetalDevice;

/**
 * @brief Metal 描述符集布局实现
 */
class MetalDescriptorSetLayout : public RHIResource {
    friend class MetalDevice;
public:
    MetalDescriptorSetLayout(MetalDevice& device, const DescriptorSetLayoutDesc& desc);
    ~MetalDescriptorSetLayout() override;

    bool Initialize() override { return true; }
    void Destroy() override {}

    const DescriptorSetLayoutDesc& GetDesc() const { return desc_; }
    const std::vector<DescriptorSetLayoutBinding>& GetBindings() const { return bindings_; }

protected:
    void* mapImpl(uint64_t /*offset*/, uint64_t /*size*/) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void* /*data*/, uint64_t /*size*/, uint64_t /*offset*/) override { return false; }

private:
    MetalDevice& device_;
    DescriptorSetLayoutDesc desc_;
    std::vector<DescriptorSetLayoutBinding> bindings_;
};

/**
 * @brief Metal 描述符绑定信息
 */
struct MetalDescriptorBinding {
    DescriptorType type;
    ResourceHandle resource; // Buffer or Texture view
    SamplerHandle sampler;
    uint64_t offset;
    uint64_t range;
    ResourceState imageLayout;
    
    MetalDescriptorBinding() : type(DescriptorType::Unknown), 
                              resource(handles::INVALID_RESOURCE), 
                              sampler(handles::INVALID_SAMPLER),
                              offset(0), range(0),
                              imageLayout(ResourceState::Unknown) {}
};

/**
 * @brief Metal 描述符集实现
 */
class MetalDescriptorSet : public RHIResource {
    friend class MetalDevice;
public:
    MetalDescriptorSet(MetalDevice& device, const DescriptorSetDesc& desc);
    ~MetalDescriptorSet() override;

    bool Initialize() override { return true; }
    void Destroy() override {}

    void Update(uint32_t writeCount, const WriteDescriptorSet* writes);
    
    const std::map<uint32_t, MetalDescriptorBinding>& GetBindings() const { return bindings_; }

protected:
    void* mapImpl(uint64_t /*offset*/, uint64_t /*size*/) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void* /*data*/, uint64_t /*size*/, uint64_t /*offset*/) override { return false; }

private:
    MetalDevice& device_;
    DescriptorSetDesc desc_;
    std::map<uint32_t, MetalDescriptorBinding> bindings_; // binding -> info
};

} // namespace primal::graphics::rhi
