/**
 * @file DawnRenderPass.cpp
 * @brief Dawn/WebGPU render pass implementation (descriptor cache)
 * @details WebGPU creates render passes inline during command recording.
 *          This class simply stores the RenderPassDesc for later use
 *          when DawnCommandBuffer::BeginRenderPass constructs the
 *          WGPURenderPassDescriptor on the fly.
 * @author GameEngine VulkanCPP Team
 * @date 2026-05-14
 * @version 0.1.0
 */

#include "DawnRenderPass.h"
#include "DawnDevice.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

namespace primal::graphics::rhi {

DawnRenderPass::DawnRenderPass(DawnDevice& device)
    : device_(device) {}

DawnRenderPass::~DawnRenderPass() {
    Destroy();
}

bool DawnRenderPass::Initialize(const RenderPassDesc& desc) {
    // Simply cache the descriptor. The actual WGPURenderPassDescriptor
    // is built on the fly in DawnCommandBuffer::BeginRenderPass.
    desc_ = desc;
    return true;
}

void DawnRenderPass::Destroy() {
    // No native WebGPU objects to release; the desc_ vector members
    // will clean up automatically via their destructors.
}

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
