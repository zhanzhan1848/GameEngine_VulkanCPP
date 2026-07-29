/**
 * @file TestRHIDeviceFactory.cpp
 * @brief RHI 设备被动分发器端到端验证
 * @details 验证 CreateRHIDevice/DestroyRHIDevice 能正常工作。
 *          覆盖：Metal 平台创建+销毁、未实现平台返回 nullptr、未知平台返回 nullptr。
 * @author GameEngine VulkanCPP Team
 * @date 2026-06-16
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"

using namespace primal::graphics::rhi;
using namespace Engine::Test;

// 验证 Metal 平台能创建非空 device，且原生 MTL::Device* 可访问
TestResult TestCreateMetalDevice() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = false;

    RHIDeviceBase* device = CreateRHIDevice(desc);
    TEST_ASSERT(device != nullptr, "CreateRHIDevice(Metal) should return non-null");

#if defined(__APPLE__)
    // 验证是真正的 MetalDevice 实例
    auto* metalDevice = dynamic_cast<MetalDevice*>(device);
    TEST_ASSERT(metalDevice != nullptr, "Device should be a MetalDevice");
    TEST_ASSERT(metalDevice->GetNativeDevice() != nullptr, "Native MTL::Device should be initialized");
#endif

    DestroyRHIDevice(device);
    return TestResult::Passed;
}

// 验证未实现的后端返回 nullptr
TestResult TestUnimplementedBackendsReturnNull() {
    DeviceDesc desc;
    desc.enableDebug = false;

    desc.platform = RHIPlatform::Vulkan;
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
    RHIDeviceBase* vk = CreateRHIDevice(desc);
    TEST_ASSERT(vk != nullptr, "Vulkan backend should return non-null when ENABLE_VULKAN is defined");
    DestroyRHIDevice(vk);
#else
    TEST_ASSERT(CreateRHIDevice(desc) == nullptr, "Vulkan backend should return nullptr when ENABLE_VULKAN is undefined");
#endif

    desc.platform = RHIPlatform::D3D12;
    TEST_ASSERT(CreateRHIDevice(desc) == nullptr, "D3D12 backend should return nullptr (not implemented)");

    desc.platform = RHIPlatform::Dawn;
    TEST_ASSERT(CreateRHIDevice(desc) == nullptr, "Dawn backend should return nullptr (not implemented)");

    return TestResult::Passed;
}

// 验证 Unknown 平台返回 nullptr
TestResult TestUnknownPlatformReturnsNull() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Unknown;
    TEST_ASSERT(CreateRHIDevice(desc) == nullptr, "Unknown platform should return nullptr");
    return TestResult::Passed;
}

// 验证 DestroyRHIDevice(nullptr) 安全（不崩溃）
TestResult TestDestroyNullIsSafe() {
    DestroyRHIDevice(nullptr);
    return TestResult::Passed;
}

// 注册测试套件
void RegisterRHIDeviceFactoryTests() {
    auto suite = std::make_shared<TestSuite>("RHIDeviceFactoryTests");
    suite->AddTestCase(TestCase("CreateMetalDevice", TestCreateMetalDevice));
    suite->AddTestCase(TestCase("UnimplementedBackendsReturnNull", TestUnimplementedBackendsReturnNull));
    suite->AddTestCase(TestCase("UnknownPlatformReturnsNull", TestUnknownPlatformReturnsNull));
    suite->AddTestCase(TestCase("DestroyNullIsSafe", TestDestroyNullIsSafe));

    TestRunner::RegisterTestSuite(suite);
}

#ifndef UNIT_TEST_LIB
int main() {
    RegisterRHIDeviceFactoryTests();
    TestRunner::RunAllSuites();
    return 0;
}
#endif
