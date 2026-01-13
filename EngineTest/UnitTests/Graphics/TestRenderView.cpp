#include "TestFramework.h"
#include "Graphics/RenderView.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RHI/Core/RHIMath.h"

using namespace primal::graphics;
using namespace primal::graphics::rhi::math;

using TestResult = Engine::Test::TestResult;

TestResult TestRenderViewCreation() {
    RenderView view;
    
    // Check if default matrices are identity
    // Note: Comparing floats directly is risky, but for identity exact 0 and 1 it should be fine
    // Or use a helper function to check matrix equality with epsilon
    
    const m4x4& viewMat = view.GetViewMatrix();
    // Assuming identity is diagonal 1s
    TEST_ASSERT(std::abs(viewMat.columns[0][0] - 1.f) < 0.001f, "Default View Matrix [0][0] should be 1");
    TEST_ASSERT(std::abs(viewMat.columns[3][0] - 0.f) < 0.001f, "Default View Matrix [3][0] should be 0");
    
    return TestResult::Passed;
}

TestResult TestRenderViewMatrices() {
    RenderView view;
    
    // Create a translation matrix for view
    v3 translation{0.f, 0.f, -10.f};
    m4x4 viewMat = CreateTranslationMatrix(translation);
    
    view.SetViewMatrix(viewMat);
    
    // Create a simple projection matrix (e.g. perspective)
    // For now just use identity or another translation to verify setter
    m4x4 projMat = CreateTranslationMatrix(v3{1.f, 2.f, 3.f});
    view.SetProjectionMatrix(projMat);
    
    // Check View Matrix
    const m4x4& currView = view.GetViewMatrix();
    TEST_ASSERT(std::abs(currView.columns[3][2] - (-10.f)) < 0.001f, "View Matrix not updated correctly");
    
    // Check Projection Matrix
    const m4x4& currProj = view.GetProjectionMatrix();
    TEST_ASSERT(std::abs(currProj.columns[3][0] - 1.f) < 0.001f, "Projection Matrix not updated correctly");
    
    // Check VP Matrix
    const m4x4& currVP = view.GetViewProjectionMatrix();
    // VP = P * V
    // Translation(1,2,3) * Translation(0,0,-10) = Translation(1,2,-7)
    TEST_ASSERT(std::abs(currVP.columns[3][2] - (-7.f)) < 0.001f, "ViewProjection Matrix not updated correctly");
    
    return TestResult::Passed;
}

TestResult TestRenderViewCulling() {
    RenderView view;
    RenderScene scene;
    
    // Setup View: Camera at (0,0,10) looking at (0,0,0)
    // View Matrix: Translate(0,0,-10)
    // Proj Matrix: Identity (for simplicity, defines a box -1..1)
    
    // Actually, to make culling predictable without complex perspective setup:
    // Let's keep identity projection (-1 to 1 box)
    // And move camera back by 0, so it sees -1 to 1 in Z.
    
    // Let's use standard identity for view and proj.
    // Frustum should be -1..1 on all axes.
    
    // Add Proxy inside frustum
    RenderProxy proxyInside = RenderProxy::Create(1, 100, 1);
    proxyInside.UpdateTransform(CreateTranslationMatrix(v3{0.f, 0.f, 0.f}));
    scene.AddProxy(proxyInside);
    
    // Add Proxy outside frustum
    RenderProxy proxyOutside = RenderProxy::Create(2, 101, 1);
    proxyOutside.UpdateTransform(CreateTranslationMatrix(v3{10.f, 0.f, 0.f}));
    scene.AddProxy(proxyOutside);
    
    view.UpdateFrustum();
    view.Cull(scene);
    
    const auto& visible = view.GetVisibleProxies();
    TEST_ASSERT(visible.size() == 1, "Should have exactly 1 visible proxy");
    if (!visible.empty()) {
        TEST_ASSERT(visible[0]->entityId == 1, "Visible proxy should be the one inside frustum");
    }
    
    return TestResult::Passed;
}

int main() {
    Engine::Test::TestSuite suite("RenderView Tests");
    
    suite.AddTestCase(Engine::Test::TestCase("TestRenderViewCreation", TestRenderViewCreation));
    suite.AddTestCase(Engine::Test::TestCase("TestRenderViewMatrices", TestRenderViewMatrices));
    suite.AddTestCase(Engine::Test::TestCase("TestRenderViewCulling", TestRenderViewCulling));
    
    suite.RunAllTests();
    
    return 0;
}
