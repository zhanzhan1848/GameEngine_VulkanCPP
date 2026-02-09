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
        
        // Convert to Near/Far distances for CreateOrthographicMatrix
        // CreateOrthographicMatrix maps -Near to 0 and -Far to 1
        // We want maxZ (closest) to map to 0, and minZ (furthest) to map to 1
        // Extend the Z range to include potential casters that are outside the camera frustum
        // but between the light and the frustum.
        float nearDist = -maxZ - 1000.0f; 
        float farDist = -minZ + 1000.0f;

        auto lightProj = math::CreateOrthographicMatrix(minX, maxX, minY, maxY, nearDist, farDist);
        
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

void CreateSpotShadowView(
    const math::v3& lightPos,
    const math::v3& lightDir,
    float outerCone,
    float range,
    uint32_t shadowMapSize,
    RenderView& outView
) {
    // 1. Calculate FOV
    // outerCone is cos(theta), so theta = acos(outerCone)
    // FOV = 2 * theta
    float cosTheta = std::clamp(outerCone, -1.0f, 1.0f);
    float theta = std::acos(cosTheta);
    float fov = 2.0f * theta;

    // 2. Create Projection Matrix
    // Aspect ratio 1.0 for shadow map
    // Near plane 0.1f (tweakable)
    float nearPlane = 0.1f;
    math::m4x4 proj = math::CreatePerspectiveMatrix(fov, 1.0f, nearPlane, range);

    // 3. Create View Matrix
    math::v3 up = {0.0f, 1.0f, 0.0f};
    if (std::abs(math::Dot(up, lightDir)) > 0.99f) {
        up = {0.0f, 0.0f, 1.0f};
    }
    math::m4x4 view = math::CreateLookAtMatrix(lightPos, lightPos + lightDir, up);

    // 4. Set to RenderView
    outView.SetType(ViewType::ShadowMap);
    outView.SetViewMatrix(view);
    outView.SetProjectionMatrix(proj);
    outView.UpdateFrustum();
    
    rhi::Rect rect{ { 0, 0 }, { shadowMapSize, shadowMapSize } };
    rhi::ViewportDesc viewport{
        {0.0f, 0.0f},
        {static_cast<float>(shadowMapSize), static_cast<float>(shadowMapSize)},
        0.0f, 1.0f
    };
    outView.SetViewport(viewport);
    outView.SetScissor(rect);
}

void CreatePointShadowViews(
    const math::v3& lightPos,
    float range,
    uint32_t shadowMapSize,
    utl::vector<RenderView>& outViews
) {
    outViews.clear();
    outViews.resize(6);

    // Projection is same for all faces: 90 degree FOV, aspect 1.0
    float fov = math::constants::HALF_PI; // 90 degrees
    float nearPlane = 0.1f;
    math::m4x4 proj = math::CreatePerspectiveMatrix(fov, 1.0f, nearPlane, range);

    // Define the 6 directions and up vectors for CubeMap faces
    // Order: +X, -X, +Y, -Y, +Z, -Z
    // Standard CubeMap conventions (RenderMan/OpenGL/Metal usually compatible regarding direction)
    struct FaceConfig {
        math::v3 targetOffset;
        math::v3 up;
    };

    FaceConfig faces[6] = {
        {{ 1.0f,  0.0f,  0.0f}, {0.0f, -1.0f,  0.0f}}, // +X (Right)
        {{-1.0f,  0.0f,  0.0f}, {0.0f, -1.0f,  0.0f}}, // -X (Left)
        {{ 0.0f,  1.0f,  0.0f}, {0.0f,  0.0f,  1.0f}}, // +Y (Top)
        {{ 0.0f, -1.0f,  0.0f}, {0.0f,  0.0f, -1.0f}}, // -Y (Bottom)
        {{ 0.0f,  0.0f,  1.0f}, {0.0f, -1.0f,  0.0f}}, // +Z (Front)
        {{ 0.0f,  0.0f, -1.0f}, {0.0f, -1.0f,  0.0f}}  // -Z (Back)
    };

    rhi::Rect rect{ { 0, 0 }, { shadowMapSize, shadowMapSize } };
    rhi::ViewportDesc viewport{
        {0.0f, 0.0f},
        {static_cast<float>(shadowMapSize), static_cast<float>(shadowMapSize)},
        0.0f, 1.0f
    };

    // 6 Faces
    for (uint32_t i = 0; i < 6; ++i) {
        math::v3 target = lightPos + faces[i].targetOffset;
        math::m4x4 view = math::CreateLookAtMatrix(lightPos, target, faces[i].up);

        outViews[i].SetType(ViewType::CubeMap);
        outViews[i].SetViewMatrix(view);
        outViews[i].SetProjectionMatrix(proj);
        outViews[i].UpdateFrustum();
        
        outViews[i].SetViewport(viewport);
        outViews[i].SetScissor(rect);
    }
}

} // namespace primal::graphics::utils
