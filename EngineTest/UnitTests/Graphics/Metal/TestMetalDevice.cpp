#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"

using namespace primal::graphics::rhi;
using namespace Engine::Test;

TestResult TestCreateDevice() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = true;

    MetalDevice device(desc);
    bool result = device.Initialize();
    
    TEST_ASSERT(result, "Device initialization failed");
    TEST_ASSERT(device.IsValid(), "Device should be valid after initialization");
    
    if (result) {
        const DeviceInfo& info = device.GetInfo();
        std::cout << "Device Name: " << info.deviceName << std::endl;
        
        // 注意：这里比较整数值，因为 RHIPlatform 是 enum class
        TEST_ASSERT(info.platform == RHIPlatform::Metal, "Platform should be Metal");
        
        device.Shutdown();
        TEST_ASSERT(!device.IsValid(), "Device should be invalid after shutdown");
    }
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("MetalDeviceTests");
    suite->AddTestCase(TestCase("CreateDevice", TestCreateDevice));
    
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    
    return 0;
}
