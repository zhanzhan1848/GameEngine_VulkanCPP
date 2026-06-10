#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Utilities/MathTypes.h"
#include <vector>

namespace primal::graphics {

class LineBatchRenderer {
public:
    LineBatchRenderer() = default;
    ~LineBatchRenderer();

    bool Initialize(rhi::RHIDeviceBase* device);
    void Shutdown();

    void BeginFrame();
    void AddLines(const math::v3* vertices, u32 vertex_count);
    void Render(rhi::RHICommandBuffer* cmd, const math::m4x4& view_proj);
    u32  line_count() const { return static_cast<u32>(line_vertices_.size()) / 2; }

private:
    void create_pipeline();
    void ensure_buffer(u32 required_vertices);

    rhi::RHIDeviceBase* device_ = nullptr;

    rhi::PipelineHandle        pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle  layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};

    rhi::ResourceHandle        vertex_buffer_{rhi::handles::INVALID_RESOURCE};
    u32                        buffer_capacity_{0};

    std::vector<math::v3>      line_vertices_;
    bool                       initialized_{false};
};

} // namespace primal::graphics
