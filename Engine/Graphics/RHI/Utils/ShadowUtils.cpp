#include "ShadowUtils.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace primal::graphics::utils {

using namespace rhi;

void CalculateCascadeSplits(const CascadeConfig& config, utl::vector<float>& outSplits) {
    outSplits.resize(config.cascadeCount + 1);
    
    float nearClip = config.nearClip;
    float farClip = config.farClip;
    float lambda = config.splitLambda;
    
    outSplits[0] = nearClip;
    outSplits[config.cascadeCount] = farClip;
    
    for (uint32_t i = 1; i < config.cascadeCount; ++i) {
        float p = static_cast<float>(i) / static_cast<float>(config.cascadeCount);
        float log = nearClip * std::pow(farClip / nearClip, p);
        float uniform = nearClip + (farClip - nearClip) * p;
        outSplits[i] = lambda * log + (1.0f - lambda) * uniform;
    }
}

void CreateCascadeViews(
    const RenderView& mainView,
    const math::v3& lightDir,
    const CascadeConfig& config,
    utl::vector<RenderView>& outViews) 
{
    outViews.clear();
    outViews.reserve(config.cascadeCount);
    
    utl::vector<float> splits;
    CalculateCascadeSplits(config, splits);
    
    const auto& mainProj = mainView.GetProjectionMatrix();
    // Assuming Perspective Projection
    // m[1][1] = 1 / tan(fov/2)
    // aspect = m[1][1] / m[0][0] (if m[0][0] = 1/(aspect*tan(fov/2)))
    float m11 = mainProj.columns[1][1];
    float m00 = mainProj.columns[0][0];
    
    // Avoid division by zero
    if (std::abs(m11) < 0.0001f || std::abs(m00) < 0.0001f) {
        return; 
    }
    
    float tanHalfFovY = 1.0f / m11;
    float aspect = m11 / m00;
    
    const auto& mainViewMat = mainView.GetViewMatrix();
    const auto mainInvView = math::Inverse(mainViewMat);
    
    for (uint32_t i = 0; i < config.cascadeCount; ++i) {
        float splitNear = splits[i];
        float splitFar = splits[i+1];
        
        utl::vector<math::v3> corners;
        corners.reserve(8);
        
        // Calculate frustum slice corners in View Space (looking down -Z)
        float hNear = tanHalfFovY * splitNear;
        float wNear = hNear * aspect;
        float hFar = tanHalfFovY * splitFar;
        float wFar = hFar * aspect;
        
        // Z is negative in View Space
        float zNear = -splitNear;
        float zFar = -splitFar;
        
        corners.push_back(math::v3{-wNear, -hNear, zNear});
        corners.push_back(math::v3{ wNear, -hNear, zNear});
        corners.push_back(math::v3{ wNear,  hNear, zNear});
        corners.push_back(math::v3{-wNear,  hNear, zNear});
        corners.push_back(math::v3{-wFar, -hFar, zFar});
        corners.push_back(math::v3{ wFar, -hFar, zFar});
        corners.push_back(math::v3{ wFar,  hFar, zFar});
        corners.push_back(math::v3{-wFar,  hFar, zFar});
        
        // Transform to World Space
        math::v3 center{0,0,0};
        for (auto& p : corners) {
            math::v4 ptView{p.x, p.y, p.z, 1.0f};
            math::v4 ptWorld = mainInvView * ptView;
            p = math::v3{ptWorld.x, ptWorld.y, ptWorld.z};
            center = center + p;
        }
        center = center / 8.0f;
        
        // Light View Matrix
        math::v3 lightUp{0.0f, 1.0f, 0.0f};
        if (std::abs(math::Dot(lightUp, lightDir)) > 0.99f) {
            lightUp = {0.0f, 0.0f, 1.0f};
        }
        
        // Move light position back to ensure everything is in front of near plane
        math::v3 lightPos = center - lightDir * (splitFar * 2.0f); 
        
        auto lightView = math::CreateLookAtMatrix(lightPos, center, lightUp);
        
        // Light Projection Matrix
        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float minY = std::numeric_limits<float>::max();
        float maxY = std::numeric_limits<float>::lowest();
        float minZ = std::numeric_limits<float>::max();
        float maxZ = std::numeric_limits<float>::lowest();
        
        for (const auto& p : corners) {
            math::v4 pWorld{p.x, p.y, p.z, 1.0f};
            math::v4 pLight = lightView * pWorld;
            minX = std::min(minX, pLight.x);
            maxX = std::max(maxX, pLight.x);
            minY = std::min(minY, pLight.y);
            maxY = std::max(maxY, pLight.y);
            minZ = std::min(minZ, pLight.z);
            maxZ = std::max(maxZ, pLight.z);
        }
        
        // Texel Snapping
        float worldUnitsPerTexel = (maxX - minX) / config.shadowMapSize;
        if (worldUnitsPerTexel > 0.0001f) {
            minX = std::floor(minX / worldUnitsPerTexel) * worldUnitsPerTexel;
            maxX = std::floor(maxX / worldUnitsPerTexel) * worldUnitsPerTexel;
            minY = std::floor(minY / worldUnitsPerTexel) * worldUnitsPerTexel;
            maxY = std::floor(maxY / worldUnitsPerTexel) * worldUnitsPerTexel;
        }
        
        // Extend Z range to include potential casters between light and frustum
        minZ -= 100.0f; 
        maxZ += 100.0f;
        
        auto lightProj = math::CreateOrthographicMatrix(minX, maxX, minY, maxY, minZ, maxZ);
        
        RenderView shadowView;
        shadowView.SetType(ViewType::ShadowMap);
        shadowView.SetViewMatrix(lightView);
        shadowView.SetProjectionMatrix(lightProj);
        
        rhi::ViewportDesc viewport;
        viewport.size = {static_cast<float>(config.shadowMapSize), static_cast<float>(config.shadowMapSize)};
        viewport.topLeft = {0.0f, 0.0f};
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        shadowView.SetViewport(viewport);
        
        rhi::Rect scissor;
        scissor.extent = {config.shadowMapSize, config.shadowMapSize};
        scissor.offset = {0, 0};
        shadowView.SetScissor(scissor);
        
        ShadowViewInfo info;
        info.cascadeIndex = i;
        info.splitDistance = splits[i+1];
        shadowView.SetShadowInfo(info);
        
        outViews.push_back(shadowView);
    }
}

} // namespace primal::graphics::utils
