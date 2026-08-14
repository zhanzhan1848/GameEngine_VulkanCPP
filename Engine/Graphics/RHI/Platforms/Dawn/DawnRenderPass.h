/**
 * @file DawnRenderPass.h
 * @brief Dawn/WebGPU render pass (descriptor cache)
 * @details In WebGPU, render passes are created inline during command recording.
 *          This class serves as a descriptor cache only.
 * @author GameEngine VulkanCPP Team
 * @date 2026-05-14
 * @version 0.1.0
 */

#pragma once

#include "DawnCommon.h"

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "../../Core/RHITypes.h"

namespace primal::graphics::rhi {

class DawnDevice;

class DawnRenderPass {
    friend class DawnDevice;
public:
    ~DawnRenderPass();
    const RenderPassDesc& GetDesc() const { return desc_; }

public:
    DawnRenderPass(DawnDevice& device);

private:
    bool Initialize(const RenderPassDesc& desc);
    void Destroy();

    DawnDevice& device_;
    RenderPassDesc desc_;
};

} // namespace primal::graphics::rhi

#endif // ENABLE_WEBGPU
