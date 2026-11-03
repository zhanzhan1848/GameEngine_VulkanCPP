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
#if defined(__APPLE__)
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#endif

using namespace primal::graphics::rhi;
using namespace Engine::Test;

// 验证 Metal 平台能创建非空 device，且原生 MTL::Device* 可访问。
// 2026-11-03 修正非 APPLE 语义：Metal 后端只在 __APPLE__ 编译，Windows/
// Linux 上 CreateRHIDevice(Metal) 必然返回 nullptr——旧版在此直接断言非空
// → 非 Apple 平台必 FAIL。平台门内运行，非平台 = SKIP 计数（不是失败）。
TestResult TestCreateMetalDevice() {
#if defined(__APPLE__)
    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = false;

    RHIDeviceBase* device = CreateRHIDevice(desc);
    TEST_ASSERT(device != nullptr, "CreateRHIDevice(Metal) should return non-null");

    // 验证是真正的 MetalDevice 实例
    auto* metalDevice = dynamic_cast<MetalDevice*>(device);
    TEST_ASSERT(metalDevice != nullptr, "Device should be a MetalDevice");
    TEST_ASSERT(metalDevice->GetNativeDevice() != nullptr, "Native MTL::Device should be initialized");

    DestroyRHIDevice(device);
    return TestResult::Passed;
#else
    return TestResult::Skipped;  // Metal 后端仅 __APPLE__ 编译（工厂 nullptr 路径由下例覆盖）
#endif
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
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
    // F0-9（D3D12 M0）：Windows + ENABLE_D3D12 时工厂应实例化 D3D12Device
    // 最小闭环（此分支在 macOS 不编译；本文件由 UnitTests CMake 在
    // WIN32+ENABLE_D3D12 时传 ENABLE_D3D12=1 宏）。
    RHIDeviceBase* d3d = CreateRHIDevice(desc);
    TEST_ASSERT(d3d != nullptr, "D3D12 backend should return non-null when ENABLE_D3D12 is defined on Windows");
    DestroyRHIDevice(d3d);
#else
    TEST_ASSERT(CreateRHIDevice(desc) == nullptr, "D3D12 backend should return nullptr (not implemented)");
#endif

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
    // 2026-11-03 修正：失败码传播（对齐 TestD3D12StateTracker M6 修法——
    // RunAllSuites 返回 TestStats，failedTests>0 → exit 1；不再恒 0）。
    Engine::Test::TestStats stats = TestRunner::RunAllSuites();
    return stats.failedTests == 0 ? 0 : 1;
}
#endif
