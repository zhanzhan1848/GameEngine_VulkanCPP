#pragma once
#include "Graphics/RenderView.h"
#include "Graphics/RHI/Core/RHIGeometry.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include <vector>

namespace primal::graphics::utils {

struct CascadeConfig {
    uint32_t cascadeCount{3};
    float splitLambda{0.5f}; // 0.0 = uniform, 1.0 = logarithmic
    float nearClip{0.1f};
    float farClip{100.0f};
    uint32_t shadowMapSize{2048};
};

/**
 * @brief Calculate split distances for CSM
 * @param config Cascade configuration
 * @param outSplits Output split distances (normalized 0..1 or view space z)
 */
void CalculateCascadeSplits(const CascadeConfig& config, utl::vector<float>& outSplits);

/**
 * @brief Create shadow views for a directional light
 * @param mainView The main camera view
 * @param lightDir Direction of the light (should be normalized)
 * @param config CSM configuration
 * @param outViews Output vector of RenderViews (one per cascade)
 */
void CreateCascadeViews(
    const RenderView& mainView,
    const math::v3& lightDir,
    const CascadeConfig& config,
    utl::vector<RenderView>& outViews
);

} // namespace primal::graphics::utils
