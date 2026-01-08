#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalSwapChain.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/PlatformTypes.h"

using namespace primal::graphics::rhi;
using namespace Engine::Test;

TestResult TestCreateSwapChain_InvalidWindow() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        return TestResult::Failed;
    }

    SwapChainDesc swapChainDesc;
    swapChainDesc.window = nullptr; // Invalid window
    swapChainDesc.width = 800;
    swapChainDesc.height = 600;
    swapChainDesc.format = DataFormat::BGRA8_UNorm;
    swapChainDesc.bufferCount = 3;

    RHISwapChain* swapChain = device.CreateSwapChain(swapChainDesc);
    
    // Expect nullptr because window is invalid
    TEST_ASSERT(swapChain == nullptr, "CreateSwapChain should fail with null window");

    device.Shutdown();
    return TestResult::Passed;
}

TestResult TestCreateSwapChain_ValidWindow() {
    // 1. Initialize Window
    primal::platform::window_init_info init_info;
    init_info.caption = "Test Window";
    init_info.width = 800;
    init_info.height = 600;
    
    primal::platform::window win = primal::platform::create_window(&init_info);
    if (!win.is_valid()) return TestResult::Failed;

    // 2. Initialize Device
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        primal::platform::remove_window(win.get_id());
        return TestResult::Failed;
    }

    // 3. Create SwapChain
    SwapChainDesc swapChainDesc;
    swapChainDesc.window = win.handle();
    swapChainDesc.width = 800;
    swapChainDesc.height = 600;
    swapChainDesc.format = DataFormat::BGRA8_UNorm;
    swapChainDesc.bufferCount = 3;

    RHISwapChain* swapChain = device.CreateSwapChain(swapChainDesc);
    TEST_ASSERT(swapChain != nullptr, "CreateSwapChain should succeed with valid window");

    if (swapChain) {
        const SwapChainDesc& actualDesc = swapChain->GetDesc();
        TEST_ASSERT(actualDesc.width == 800, "Width mismatch");
        TEST_ASSERT(actualDesc.height == 600, "Height mismatch");
        
        swapChain->Present(false); 
        device.DestroySwapChain(swapChain);
    }
    
    device.Shutdown();
    primal::platform::remove_window(win.get_id());
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("MetalSwapChainTests");
    suite->AddTestCase(TestCase("CreateSwapChain_InvalidWindow", TestCreateSwapChain_InvalidWindow));
    suite->AddTestCase(TestCase("CreateSwapChain_ValidWindow", TestCreateSwapChain_ValidWindow));
    
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    
    return 0;
}
