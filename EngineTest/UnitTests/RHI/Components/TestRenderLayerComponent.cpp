#include "../../TestFramework.h"
#include "Engine/Graphics/RHI/Components/RenderLayerComponent.h"

using namespace primal::graphics::rhi;

Engine::Test::TestResult TestRenderLayerComponent_Construction() {
    RenderLayerComponent component;
    
    // Test default values
    TEST_ASSERT(component.layerMask == 1, "Default layerMask should be 1 (Layer 0)");
    TEST_ASSERT(component.priority == 0, "Default priority should be 0");
    TEST_ASSERT(component.useCustomViewport == false, "Default useCustomViewport should be false");
    TEST_ASSERT(component.useCustomScissor == false, "Default useCustomScissor should be false");
    
    return Engine::Test::TestResult::Passed;
}

Engine::Test::TestResult TestRenderLayerComponent_LayerManipulation() {
    RenderLayerComponent component;
    
    // Test SetLayer
    component.SetLayer(2); // Set to Layer 2 (bit 2 = 4)
    TEST_ASSERT(component.layerMask == (1 << 2), "SetLayer failed");
    TEST_ASSERT(component.HasLayer(2), "HasLayer(2) should be true");
    TEST_ASSERT(!component.HasLayer(0), "HasLayer(0) should be false");
    
    // Test EnableLayer
    component.EnableLayer(5);
    TEST_ASSERT(component.HasLayer(2), "Layer 2 should still be enabled");
    TEST_ASSERT(component.HasLayer(5), "Layer 5 should be enabled");
    TEST_ASSERT(component.layerMask == ((1 << 2) | (1 << 5)), "LayerMask incorrect after EnableLayer");
    
    // Test DisableLayer
    component.DisableLayer(2);
    TEST_ASSERT(!component.HasLayer(2), "Layer 2 should be disabled");
    TEST_ASSERT(component.HasLayer(5), "Layer 5 should still be enabled");
    TEST_ASSERT(component.layerMask == (1 << 5), "LayerMask incorrect after DisableLayer");
    
    return Engine::Test::TestResult::Passed;
}

Engine::Test::TestResult TestRenderLayerComponent_Match() {
    RenderLayerComponent component;
    component.SetLayer(1); // 0x02
    
    TEST_ASSERT(component.Match(0x02), "Match exact mask failed");
    TEST_ASSERT(component.Match(0x03), "Match overlapping mask failed"); // 0x02 & 0x03 != 0
    TEST_ASSERT(!component.Match(0x01), "Match non-overlapping mask should fail"); // 0x02 & 0x01 == 0
    
    return Engine::Test::TestResult::Passed;
}

Engine::Test::TestResult TestRenderLayerComponent_ViewportScissor() {
    RenderLayerComponent component;
    
    ViewportDesc vp(10, 20, 800, 600);
    component.viewport = vp;
    component.useCustomViewport = true;
    
    TEST_ASSERT(component.viewport.topLeft.x == 10, "Viewport X mismatch");
    TEST_ASSERT(component.viewport.size.x == 800, "Viewport Width mismatch");
    
    Rect scissor(0, 0, 1920, 1080);
    component.scissor = scissor;
    component.useCustomScissor = true;
    
    TEST_ASSERT(component.scissor.extent.x == 1920, "Scissor Width mismatch");
    
    return Engine::Test::TestResult::Passed;
}

int main() {
    Engine::Test::TestSuite suite("RenderLayerComponent Tests");
    
    suite.AddTestCase({"Construction", TestRenderLayerComponent_Construction});
    suite.AddTestCase({"LayerManipulation", TestRenderLayerComponent_LayerManipulation});
    suite.AddTestCase({"Match", TestRenderLayerComponent_Match});
    suite.AddTestCase({"ViewportScissor", TestRenderLayerComponent_ViewportScissor});
    
    suite.RunAllTests();
    
    return 0;
}
