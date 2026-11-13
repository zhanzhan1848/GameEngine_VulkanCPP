/**
 * @file TestVulkanSync.cpp
 * @brief Vulkan RHI Sync Phase 2 单元测试
 * @details 验证 VulkanDevice::CreateSync / DestroySync + VulkanSync::WaitFence 短路语义。
 *          Phase 2 范围:不测 submit-signal-wait(需 CommandBuffer,Phase 3 接入)。
 *          主要测:句柄池健康 + 非信号态 timeout 行为 + 延迟销毁不崩。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanSync.h"
#endif

using namespace primal::graphics::rhi;
using namespace Engine::Test;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {
struct VulkanDeviceFixture {
    RHIDeviceBase* base{ nullptr };
    VulkanDevice* vk{ nullptr };

    explicit VulkanDeviceFixture(bool validation = true) {
        DeviceDesc desc;
        desc.platform = RHIPlatform::Vulkan;
        desc.enableValidation = validation;
        desc.maxFramesInFlight = 3;
        base = CreateRHIDevice(desc);
        if (base) vk = dynamic_cast<VulkanDevice*>(base);
    }
    ~VulkanDeviceFixture() {
        if (base) DestroyRHIDevice(base);
    }
};
} // anonymous namespace

// === 用例 1:Create + Destroy ===
TestResult TestVulkanSyncCreateDestroy() {
    VulkanDeviceFixture fx;
    TEST_ASSERT_NOT_NULL(fx.vk, "VulkanDevice should be created");

    SyncHandle h = fx.base->CreateSync();
    TEST_ASSERT(h != handles::INVALID_SYNC, "CreateSync should return valid handle");
    fx.base->DestroySync(h);
    return TestResult::Passed;
}

// === 用例 2:非信号态 fence,WaitForSync 必须在 timeout 内返回 false ===
// 关键点:VulkanSync::WaitFence 内部 vkWaitForFences 应正确处理 timeout。
//         如果挂死永远 block,本测试会在 ctest 超时被 kill。
// T4.6.5 part 32: CreateSync 默认 signaled=true(避免 RenderSystem 首帧 stall),
//                  本测试需显式 ResetFence 才能验证 timeout 行为。
TestResult TestVulkanSyncTimeoutOnUnsignaled() {
    VulkanDeviceFixture fx;
    TEST_ASSERT_NOT_NULL(fx.vk, "VulkanDevice should be created");

    SyncHandle h = fx.base->CreateSync();
    TEST_ASSERT(h != handles::INVALID_SYNC, "CreateSync should succeed");

    // CreateSync 默认 signaled,显式 reset 后才能测 timeout 路径。
    VulkanSync* sync = fx.vk->GetSync(h);
    TEST_ASSERT_NOT_NULL(sync, "GetSync should return valid pointer");
    sync->ResetFence();

    // 100ms 应足够让 vkWaitForFences 返回 TIMEOUT。返回 false = 未 signal,符合预期。
    bool signaled = fx.base->WaitForSync(h, 100);
    TEST_ASSERT(!signaled, "WaitForSync on unsignaled fence must return false within timeout");

    fx.base->DestroySync(h);
    return TestResult::Passed;
}

// === 用例 3:多次 Create/Destroy 不泄漏 (sync free_list slot 复用) ===
TestResult TestVulkanSyncCreateDestroyCycle() {
    VulkanDeviceFixture fx;
    TEST_ASSERT_NOT_NULL(fx.vk, "VulkanDevice should be created");

    for (int i = 0; i < 32; ++i) {
        SyncHandle h = fx.base->CreateSync();
        TEST_ASSERT(h != handles::INVALID_SYNC, "CreateSync should succeed in cycle");
        fx.base->DestroySync(h);
    }
    return TestResult::Passed;
}

// === 用例 4:并发创建不撞(RHIAllocator 内部 mutex 保护)===
// 注:本测试是顺序 Create-then-Destroy N 个 sync,验证不同 handle 互不冲突。
TestResult TestVulkanSyncDistinctHandles() {
    VulkanDeviceFixture fx;
    TEST_ASSERT_NOT_NULL(fx.vk, "VulkanDevice should be created");

    SyncHandle handles[4] = {
        fx.base->CreateSync(),
        fx.base->CreateSync(),
        fx.base->CreateSync(),
        fx.base->CreateSync(),
    };
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT(handles[i] != handles::INVALID_SYNC, "All syncs must be valid");
        for (int j = i + 1; j < 4; ++j) {
            TEST_ASSERT(handles[i] != handles[j], "All sync handles must be distinct");
        }
    }
    for (int i = 0; i < 4; ++i) fx.base->DestroySync(handles[i]);
    return TestResult::Passed;
}

void RegisterVulkanSyncTests() {
    auto suite = std::make_shared<TestSuite>("VulkanSyncTests");
    suite->AddTestCase(TestCase("CreateDestroy", TestVulkanSyncCreateDestroy));
    suite->AddTestCase(TestCase("TimeoutOnUnsignaled", TestVulkanSyncTimeoutOnUnsignaled));
    suite->AddTestCase(TestCase("CreateDestroyCycle", TestVulkanSyncCreateDestroyCycle));
    suite->AddTestCase(TestCase("DistinctHandles", TestVulkanSyncDistinctHandles));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanSyncTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanSync] ENABLE_VULKAN not defined — test is a no-op build check." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
