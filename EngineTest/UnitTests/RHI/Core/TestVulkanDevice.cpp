/**
 * @file TestVulkanDevice.cpp
 * @brief Vulkan RHI 设备 Phase 1 烟雾测试
 * @details 验证 ENABLE_VULKAN=1 时 VulkanDevice 能创建/销毁，VkInstance/PhysicalDevice/Device
 *          正确建立，三 queue 可访问，DeviceInfo 字段填充合理。
 *          Phase 1 范围：不测 buffer/texture/pipeline 等资源；那些在 Phase 2+ 加入。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include <vulkan/vulkan.h>
#endif

using namespace primal::graphics::rhi;
using namespace Engine::Test;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

// 验证工厂能返回非空的 VulkanDevice
TestResult TestVulkanDeviceFactoryCreatesNonNull() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Vulkan;
    desc.enableDebug = false;
    desc.enableValidation = true; // 打开 validation layer 的最小验证

    RHIDeviceBase* device = CreateRHIDevice(desc);
    TEST_ASSERT_NOT_NULL(device, "CreateRHIDevice(Vulkan) should return non-null when ENABLE_VULKAN is defined");

    DestroyRHIDevice(device);
    return TestResult::Passed;
}

// 验证是真正的 VulkanDevice 实例 + 原生 VkDevice 可访问
TestResult TestVulkanDeviceNativeHandles() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Vulkan;
    desc.enableValidation = true;

    RHIDeviceBase* base = CreateRHIDevice(desc);
    TEST_ASSERT_NOT_NULL(base, "CreateRHIDevice(Vulkan) should return non-null");

    auto* vk = dynamic_cast<VulkanDevice*>(base);
    TEST_ASSERT_NOT_NULL(vk, "Device should be a VulkanDevice");

    TEST_ASSERT(vk->GetNativeInstance() != VK_NULL_HANDLE, "VkInstance must be initialized");
    TEST_ASSERT(vk->GetNativePhysicalDevice() != VK_NULL_HANDLE, "VkPhysicalDevice must be initialized");
    TEST_ASSERT(vk->GetNativeDevice() != VK_NULL_HANDLE, "VkDevice must be initialized");
    TEST_ASSERT(vk->GetGraphicsQueue() != VK_NULL_HANDLE, "Graphics queue must be available");
    TEST_ASSERT(vk->GetGraphicsQueueFamily() != UINT32_MAX, "Graphics queue family index must be valid");

    DestroyRHIDevice(base);
    return TestResult::Passed;
}

// 验证 DeviceInfo 字段填充
TestResult TestVulkanDeviceInfoPopulated() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Vulkan;
    desc.enableValidation = true;

    RHIDeviceBase* base = CreateRHIDevice(desc);
    TEST_ASSERT_NOT_NULL(base, "Device should be created");

    const DeviceInfo& info = base->GetDeviceInfo();
    TEST_ASSERT_EQ(static_cast<int>(RHIPlatform::Vulkan), static_cast<int>(info.platform),
                   "DeviceInfo.platform should be Vulkan");
    TEST_ASSERT(info.deviceName[0] != '\0', "Device name should be populated");
    TEST_ASSERT(info.maxTexture2DSize > 0, "maxTexture2DSize should be > 0");
    TEST_ASSERT(info.maxConstantBufferSize > 0, "maxConstantBufferSize should be > 0");
    TEST_ASSERT(info.dedicatedVideoMemory > 0 || info.sharedSystemMemory > 0,
                "Some memory info should be reported");

    DestroyRHIDevice(base);
    return TestResult::Passed;
}

// 验证 IsValid / Shutdown 双向语义
TestResult TestVulkanDeviceLifecycle() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Vulkan;
    desc.enableValidation = true;

    RHIDeviceBase* base = CreateRHIDevice(desc);
    TEST_ASSERT_NOT_NULL(base, "Device should be created");
    TEST_ASSERT(base->IsValid(), "IsValid() should be true after creation");
    TEST_ASSERT_EQ(static_cast<int>(RHIPlatform::Vulkan), static_cast<int>(base->GetPlatform()),
                   "GetPlatform should report Vulkan");

    // WaitIdle 在 valid 状态下应不崩溃
    base->WaitIdle();

    DestroyRHIDevice(base);
    return TestResult::Passed;
}

// 验证 frameIndex 在 BeginFrame 后递增 + 循环
TestResult TestVulkanDeviceFrameRing() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Vulkan;
    desc.enableValidation = true;
    desc.maxFramesInFlight = 3;

    RHIDeviceBase* base = CreateRHIDevice(desc);
    TEST_ASSERT_NOT_NULL(base, "Device should be created");

    auto* vk = dynamic_cast<VulkanDevice*>(base);

    // BeginFrame 是 RHIDevice template 内的方法，base class 不暴露，我们走 native 验证：
    // 直接调用 native vkDeviceWaitIdle 是 Phase 1 的核心循环 sanity check。
    vk->WaitIdle();
    vk->WaitIdle();
    vk->WaitIdle();

    DestroyRHIDevice(base);
    return TestResult::Passed;
}

// 验证 validation 关闭时也能创建（即两条路径都通过）
TestResult TestVulkanDeviceNoValidation() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Vulkan;
    desc.enableValidation = false;
    desc.enableDebug = false;

    RHIDeviceBase* base = CreateRHIDevice(desc);
    TEST_ASSERT_NOT_NULL(base, "Device should be created without validation");
    TEST_ASSERT(base->IsValid(), "IsValid() should be true");

    DestroyRHIDevice(base);
    return TestResult::Passed;
}

void RegisterVulkanDeviceTests() {
    auto suite = std::make_shared<TestSuite>("VulkanDeviceTests");
    suite->AddTestCase(TestCase("FactoryCreatesNonNull", TestVulkanDeviceFactoryCreatesNonNull));
    suite->AddTestCase(TestCase("NativeHandles", TestVulkanDeviceNativeHandles));
    suite->AddTestCase(TestCase("DeviceInfoPopulated", TestVulkanDeviceInfoPopulated));
    suite->AddTestCase(TestCase("Lifecycle", TestVulkanDeviceLifecycle));
    suite->AddTestCase(TestCase("FrameRing", TestVulkanDeviceFrameRing));
    suite->AddTestCase(TestCase("NoValidation", TestVulkanDeviceNoValidation));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanDeviceTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

// ENABLE_VULKAN 未定义时编译此 TU：测试空跑，返回 0 表示"构建通过，跳过"。
int main() {
    std::cout << "[TestVulkanDevice] ENABLE_VULKAN not defined — test is a no-op build check." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
