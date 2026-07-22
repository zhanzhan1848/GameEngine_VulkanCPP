#pragma once

#include "Graphics/Nanite/ISDFDataProvider.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Utilities/Math.h"

namespace primal::graphics::nanite {

class AnalyticSDFProvider : public ISDFDataProvider {
public:
    struct SphereParams {
        math::v3 center{0.0f, 0.0f, 0.0f};
        f32      radius{20.0f};
    };

    AnalyticSDFProvider() = default;
    ~AnalyticSDFProvider() override;

    bool Initialize(rhi::RHIDeviceBase* device) override;
    bool IsReady() const override { return pipeline_ != rhi::handles::INVALID_PIPELINE; }

    void DispatchCascade(rhi::RHICommandBuffer* cmd,
                          u32 frame_index,
                          const SDFCascade& cascade) override;

    void SetSphere(const SphereParams& p) { sphere_ = p; }

private:
    rhi::RHIDeviceBase* device_{nullptr};

    rhi::ShaderHandle            shader_{rhi::handles::INVALID_SHADER};
    rhi::PipelineLayoutHandle    layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::PipelineHandle          pipeline_{rhi::handles::INVALID_PIPELINE};

    rhi::DescriptorSetHandle ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::ResourceHandle params_cb_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE
    };

    SphereParams sphere_{};
};

} // namespace primal::graphics::nanite
