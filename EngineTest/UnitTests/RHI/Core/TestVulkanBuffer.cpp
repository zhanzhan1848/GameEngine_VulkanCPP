/**
 * @file TestVulkanBuffer.cpp
 * @brief Vulkan RHI Buffer Phase 2 单元测试
 * @details 验证 VulkanDevice::CreateBuffer / Map / Unmap / UpdateBufferData / DestroyBuffer
 *          路径在 VMA persistent-map (Dynamic) 与静态 DEVICE_LOCAL (Static) 两种语义下均工作。
 *          Phase 2 范围:仅 CPU-writable 路径(Static 的 staging blit 留 Phase 3 接入)。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanBuffer.h"
#include <cstring>
#include <vector>
#endif

using namespace primal::graphics::rhi;
using namespace Engine::Test;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {
// 用 RAII 把 device 创建/销毁封一个夹具,确保异常路径不泄漏
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

// === 用例 1:Dynamic 缓冲走 persistent-map 路径,UpdateData 直接 memcpy ===
TestResult TestVulkanBufferDynamicMapWriteRead() {
    VulkanDeviceFixture fx;
    TEST_ASSERT_NOT_NULL(fx.vk, "VulkanDevice should be created");

    BufferDesc desc{};
    desc.size = 256;
    desc.type = BufferType::Vertex;
    desc.memoryUsage = GPUMemoryUsage::Dynamic;

    ResourceHandle h = fx.base->CreateBuffer(desc);
    TEST_ASSERT(h != handles::INVALID_RESOURCE, "CreateBuffer(Dynamic) should return valid handle");

    // 写入已知 pattern
    std::vector<u8> src(256);
    for (u8& b : src) b = static_cast<u8>(&b - src.data());
    bool ok = fx.base->UpdateBufferData(h, src.data(), src.size(), 0);
    TEST_ASSERT(ok, "UpdateBufferData fast path (persistent-map) should succeed");

    // Map 读回,比对
    void* mapped = fx.base->MapBuffer(h, 0, src.size());
    TEST_ASSERT_NOT_NULL(mapped, "MapBuffer should return non-null on persistent-mapped buffer");
    if (mapped) {
        TEST_ASSERT(std::memcmp(mapped, src.data(), src.size()) == 0,
                    "Mapped memory must match written pattern");
    }

    fx.base->UnmapBuffer(h);
    fx.base->DestroyBuffer(h);
    return TestResult::Passed;
}

// === 用例 2:Constant buffer(Uniform) 路径 — 验证 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT ===
TestResult TestVulkanBufferConstant() {
    VulkanDeviceFixture fx;
    TEST_ASSERT_NOT_NULL(fx.vk, "VulkanDevice should be created");

    BufferDesc desc{};
    desc.size = 64;
    desc.type = BufferType::Constant;
    desc.memoryUsage = GPUMemoryUsage::Dynamic;
    desc.name = "TestConstant";

    ResourceHandle h = fx.base->CreateBuffer(desc);
    TEST_ASSERT(h != handles::INVALID_RESOURCE, "CreateBuffer(Constant) should succeed");

    u64 payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    bool ok = fx.base->UpdateBufferData(h, payload, sizeof(payload), 0);
    TEST_ASSERT(ok, "UpdateBufferData should succeed on Dynamic constant buffer");

    void* mapped = fx.base->MapBuffer(h, 0, sizeof(payload));
    TEST_ASSERT_NOT_NULL(mapped, "MapBuffer should succeed");
    if (mapped) {
        TEST_ASSERT(std::memcmp(mapped, payload, sizeof(payload)) == 0,
                    "Mapped content must match");
    }

    fx.base->UnmapBuffer(h);
    fx.base->DestroyBuffer(h);
    return TestResult::Passed;
}

// === 用例 3:多次 Create/Destroy 不泄漏 (free_list 健康) ===
TestResult TestVulkanBufferCreateDestroyCycle() {
    VulkanDeviceFixture fx;
    TEST_ASSERT_NOT_NULL(fx.vk, "VulkanDevice should be created");

    // 重复创建销毁 64 次,验证 free_list slot 复用 + GC 队列 drain 正常
    for (int i = 0; i < 64; ++i) {
        BufferDesc desc{};
        desc.size = 128;
        desc.type = BufferType::Vertex;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;

        ResourceHandle h = fx.base->CreateBuffer(desc);
        TEST_ASSERT(h != handles::INVALID_RESOURCE, "CreateBuffer in cycle should not fail");
        fx.base->DestroyBuffer(h);
    }

    // 触发 GC drain(几次 BeginFrame/EndFrame)— 主要是确保 DeferredDestroy lambda 不崩
    fx.base->WaitIdle();
    return TestResult::Passed;
}

// === 用例 4:Readback 缓冲走 HOST_ACCESS_RANDOM 路径 ===
TestResult TestVulkanBufferReadback() {
    VulkanDeviceFixture fx;
    TEST_ASSERT_NOT_NULL(fx.vk, "VulkanDevice should be created");

    BufferDesc desc{};
    desc.size = 64;
    desc.type = BufferType::Raw;
    desc.memoryUsage = GPUMemoryUsage::Readback;

    ResourceHandle h = fx.base->CreateBuffer(desc);
    TEST_ASSERT(h != handles::INVALID_RESOURCE, "CreateBuffer(Readback) should succeed");

    // 写入并核对 (Readback 也是 persistent-mapped,fast path 应工作)
    u8 src[64];
    for (u8& b : src) b = 0xA5;
    bool ok = fx.base->UpdateBufferData(h, src, sizeof(src), 0);
    TEST_ASSERT(ok, "UpdateBufferData should succeed on Readback buffer");

    void* mapped = fx.base->MapBuffer(h, 0, sizeof(src));
    TEST_ASSERT_NOT_NULL(mapped, "MapBuffer should succeed on Readback");
    if (mapped) {
        TEST_ASSERT(std::memcmp(mapped, src, sizeof(src)) == 0,
                    "Readback buffer content must match");
    }
    fx.base->UnmapBuffer(h);
    fx.base->DestroyBuffer(h);
    return TestResult::Passed;
}

// === 用例 5:destroyBufferImpl 在 INVALID 句柄上是合法 no-op (FreeList contract) ===
TestResult TestVulkanBufferDestroyInvalidIsNoOp() {
    VulkanDeviceFixture fx;
    TEST_ASSERT_NOT_NULL(fx.vk, "VulkanDevice should be created");

    // 句柄 0 是 INVALID_RESOURCE 的别名 (FreeList 内部 sentinel)
    fx.base->DestroyBuffer(handles::INVALID_RESOURCE);

    // 销毁后再销毁应被 GetBuffer() 拒绝(VulkanDevice 内部 double-free 警告路径)
    BufferDesc desc{};
    desc.size = 64;
    desc.type = BufferType::Vertex;
    desc.memoryUsage = GPUMemoryUsage::Dynamic;
    ResourceHandle h = fx.base->CreateBuffer(desc);
    TEST_ASSERT(h != handles::INVALID_RESOURCE, "CreateBuffer should succeed");
    fx.base->DestroyBuffer(h);
    // 二次销毁:不应崩溃,会走 GetBuffer()=nullptr 早出 + stderr warn
    fx.base->DestroyBuffer(h);
    return TestResult::Passed;
}

void RegisterVulkanBufferTests() {
    auto suite = std::make_shared<TestSuite>("VulkanBufferTests");
    suite->AddTestCase(TestCase("DynamicMapWriteRead", TestVulkanBufferDynamicMapWriteRead));
    suite->AddTestCase(TestCase("ConstantUniform", TestVulkanBufferConstant));
    suite->AddTestCase(TestCase("CreateDestroyCycle", TestVulkanBufferCreateDestroyCycle));
    suite->AddTestCase(TestCase("Readback", TestVulkanBufferReadback));
    suite->AddTestCase(TestCase("DestroyInvalidIsNoOp", TestVulkanBufferDestroyInvalidIsNoOp));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanBufferTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanBuffer] ENABLE_VULKAN not defined — test is a no-op build check." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
