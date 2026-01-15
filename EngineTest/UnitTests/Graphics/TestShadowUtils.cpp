#include "../TestFramework.h"
#include "Graphics/RHI/Utils/ShadowUtils.h"
#include "Graphics/RenderView.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "CommonHeaders.h"
#include <iostream>

using namespace primal::graphics;
using namespace primal::graphics::utils;
using namespace primal::graphics::rhi;
using namespace Engine::Test;

TestResult TestCalculateCascadeSplits() {
    CascadeConfig config;
    config.cascadeCount = 3;
    config.nearClip = 0.1f;
    config.farClip = 100.0f;
    config.splitLambda = 0.5f;

    primal::utl::vector<float> splits;
    CalculateCascadeSplits(config, splits);

    if(splits.size() != 4) {
        std::cout << "Expected 4 splits, got " << splits.size() << std::endl;
        return TestResult::Failed;
    }
    if(std::abs(splits[0] - 0.1f) >= 0.001f) {
        std::cout << "Split[0] expected 0.1, got " << splits[0] << std::endl;
        return TestResult::Failed;
    }
    if(std::abs(splits[3] - 100.0f) >= 0.001f) {
        std::cout << "Split[3] expected 100.0, got " << splits[3] << std::endl;
        return TestResult::Failed;
    }
    
    std::cout << "Splits: ";
    for(float s : splits) std::cout << s << " ";
    std::cout << std::endl;
    
    for(size_t i = 0; i < splits.size() - 1; ++i) {
        if(splits[i] >= splits[i+1]) {
            std::cout << "Splits not increasing at index " << i << std::endl;
            return TestResult::Failed;
        }
    }
    return TestResult::Passed;
}

TestResult TestCreateCascadeViews() {
    RenderView mainView;
    // Setup main view projection
    // FOV 60 deg, Aspect 1.77, Near 0.1, Far 100
    auto proj = math::CreatePerspectiveMatrix(
        60.0f * 3.14159f / 180.0f, 
        16.0f/9.0f, 
        0.1f, 
        100.0f);
    mainView.SetProjectionMatrix(proj);
    
    // View at origin looking -Z
    auto view = math::MatrixIdentity();
    mainView.SetViewMatrix(view);
    
    CascadeConfig config;
    config.cascadeCount = 3;
    config.nearClip = 0.1f;
    config.farClip = 100.0f;
    config.splitLambda = 0.5f;
    config.shadowMapSize = 1024;
    
    math::v3 lightDir = math::Normalize(math::v3{1.0f, -1.0f, 1.0f});
    
    primal::utl::vector<RenderView> shadowViews;
    CreateCascadeViews(mainView, lightDir, config, shadowViews);
    
    if(shadowViews.size() != 3) {
        std::cout << "Expected 3 shadow views, got " << shadowViews.size() << std::endl;
        return TestResult::Failed;
    }
    
    for(size_t i = 0; i < shadowViews.size(); ++i) {
        if(shadowViews[i].GetType() != ViewType::ShadowMap) {
            std::cout << "View " << i << " type incorrect" << std::endl;
            return TestResult::Failed;
        }
        if(shadowViews[i].GetShadowInfo().cascadeIndex != static_cast<uint32_t>(i)) {
            std::cout << "View " << i << " cascade index incorrect" << std::endl;
            return TestResult::Failed;
        }
        
        // Verify Viewport
        const auto& vp = shadowViews[i].GetViewport();
        if(vp.size.x != 1024.0f) {
            std::cout << "View " << i << " viewport width incorrect" << std::endl;
            return TestResult::Failed;
        }
        if(vp.size.y != 1024.0f) {
            std::cout << "View " << i << " viewport height incorrect" << std::endl;
            return TestResult::Failed;
        }
        
        // Verify Projection is Ortho
        // m[3][3] should be 1 for Ortho usually
        const auto& p = shadowViews[i].GetProjectionMatrix();
        if(std::abs(p.columns[3][3] - 1.0f) >= 0.001f) {
            std::cout << "View " << i << " projection not ortho (m33=" << p.columns[3][3] << ")" << std::endl;
            return TestResult::Failed;
        }
    }
    return TestResult::Passed;
}

int main() {
    TestSuite suite("ShadowUtils Tests");
    suite.AddTestCase(TestCase("CalculateCascadeSplits", TestCalculateCascadeSplits));
    suite.AddTestCase(TestCase("CreateCascadeViews", TestCreateCascadeViews));
    
    TestStats stats = suite.RunAllTests();
    return stats.failedTests == 0 ? 0 : 1;
}
