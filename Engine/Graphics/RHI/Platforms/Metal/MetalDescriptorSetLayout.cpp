#include "MetalDevice.h"
#include "MetalDescriptorSetLayout.h"

namespace primal::graphics::rhi {

MetalDescriptorSetLayout::MetalDescriptorSetLayout(MetalDevice& device, const DescriptorSetLayoutDesc& desc)
    : RHIDescriptorSetLayout(device, desc) {
}

MetalDescriptorSetLayout::~MetalDescriptorSetLayout() {
    Destroy();
}

void MetalDescriptorSetLayout::Destroy() {
    // Software layout, no native resources to release
}

const DescriptorSetLayoutBinding* MetalDescriptorSetLayout::GetBinding(u32 binding) const {
    for (const auto& b : bindings_) {
        if (b.binding == binding) return &b;
    }
    return nullptr;
}

}
