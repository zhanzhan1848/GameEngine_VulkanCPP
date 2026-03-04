#include "MetalPipelineLayout.h"
#include "MetalDevice.h"

namespace primal::graphics::rhi {

MetalPipelineLayout::MetalPipelineLayout(MetalDevice& device, const PipelineLayoutDesc& desc)
    : RHIPipelineLayout(device, desc) {
}

MetalPipelineLayout::~MetalPipelineLayout() {
    Destroy();
}

void MetalPipelineLayout::Destroy() {
    // Software layout
}

}
