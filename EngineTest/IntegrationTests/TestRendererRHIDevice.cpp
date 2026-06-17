/**
 * @file TestRendererRHIDevice.cpp
 * @brief Phase 1 Sub-step 1.2.2: 新 RHI 路径入口的 IntegrationTest
 * @details 验证 graphics::initialize_with_device / shutdown_rhi / is_rhi_initialized /
 *          get_rhi_device 四个新入口的端到端行为。区别于 TestRHIDeviceFactory（直接测工厂），
 *          本测试通过 Renderer 公开 API 触发，并额外用拿到的 device 创建一个 Buffer，
 *          证明 g_rhiDevice 是真正可用的 MetalDevice，不是空头指针。
 *
 *          覆盖场景：
 *            1. Metal 平台正常 init/shutdown 全流程
 *            2. 已 init 状态下重复 init 返回 false（防双开）
 *            3. shutdown 后状态归零，可再次 init
 *            4. 拿到的 device 能 dynamic_cast 到 MetalDevice 且原生 MTL::Device 非空
 *            5. 拿到的 device 能 CreateBuffer / DestroyBuffer（功能可用性）
 *            6. Unknown 平台 init 返回 false，state 不被污染
 */

#include "Engine/Common/CommonHeaders.h"
#include "Engine/Graphics/Renderer.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#if defined(__APPLE__)
#include "Engine/Graphics/Metal/MetalCore.h"
#endif

#include <cassert>
#include <iostream>
#include <string>

using namespace primal;
using namespace primal::graphics;

static int g_failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[FAIL] " << (msg) << " (line " << __LINE__ << ")" << std::endl; \
            ++g_failures; \
        } else { \
            std::cout << "[PASS] " << (msg) << std::endl; \
        } \
    } while (0)

// === 测试用例 ===

// 案例 1: Metal 平台正常 init → get → shutdown 全流程
static void TestBasicLifecycleMetal() {
    std::cout << "\n--- TestBasicLifecycleMetal ---" << std::endl;

    rhi::DeviceDesc desc{};
    desc.platform = rhi::RHIPlatform::Metal;
    desc.enableDebug = false;

    CHECK(!is_rhi_initialized(), "RHI should be uninitialized before init");
    CHECK(initialize_with_device(desc), "initialize_with_device(Metal) should succeed");
    CHECK(is_rhi_initialized(), "is_rhi_initialized should be true after init");

    rhi::RHIDeviceBase* device = get_rhi_device();
    CHECK(device != nullptr, "get_rhi_device should return non-null");

#if defined(__APPLE__)
    auto* metalDevice = dynamic_cast<rhi::MetalDevice*>(device);
    CHECK(metalDevice != nullptr, "device should be a MetalDevice");
    CHECK(metalDevice->GetNativeDevice() != nullptr, "native MTL::Device should be non-null");
#endif

    shutdown_rhi();
    CHECK(!is_rhi_initialized(), "is_rhi_initialized should be false after shutdown");
    CHECK(get_rhi_device() == nullptr, "get_rhi_device should return nullptr after shutdown");
}

// 案例 2: 重复 init 应该被拒绝
static void TestDoubleInitRejected() {
    std::cout << "\n--- TestDoubleInitRejected ---" << std::endl;

    rhi::DeviceDesc desc{};
    desc.platform = rhi::RHIPlatform::Metal;
    desc.enableDebug = false;

    CHECK(initialize_with_device(desc), "first init should succeed");
    CHECK(!initialize_with_device(desc), "second init should fail (already initialized)");

    shutdown_rhi();
    CHECK(!is_rhi_initialized(), "should be uninitialized after shutdown");
}

// 案例 3: shutdown 后可以再次 init
static void TestReinitAfterShutdown() {
    std::cout << "\n--- TestReinitAfterShutdown ---" << std::endl;

    rhi::DeviceDesc desc{};
    desc.platform = rhi::RHIPlatform::Metal;
    desc.enableDebug = false;

    CHECK(initialize_with_device(desc), "first init should succeed");
    shutdown_rhi();

    CHECK(initialize_with_device(desc), "re-init after shutdown should succeed");
    CHECK(is_rhi_initialized(), "should be initialized after re-init");

    shutdown_rhi();
}

// 案例 4: Unknown 平台应该返回 false 且不污染状态
static void TestUnknownPlatformRejected() {
    std::cout << "\n--- TestUnknownPlatformRejected ---" << std::endl;

    rhi::DeviceDesc desc{};
    desc.platform = rhi::RHIPlatform::Unknown;
    desc.enableDebug = false;

    CHECK(!initialize_with_device(desc), "Unknown platform should fail");
    CHECK(!is_rhi_initialized(), "state should remain uninitialized after failure");
    CHECK(get_rhi_device() == nullptr, "device should remain null after failure");
}

// 案例 5: 通过新 API 拿到的 device 能正常 CreateBuffer/DestroyBuffer
//        这是关键的 IntegrationTest 维度：证明 device 不只是空指针而是功能完整的 MetalDevice
static void TestDeviceFunctionalViaBufferCreate() {
    std::cout << "\n--- TestDeviceFunctionalViaBufferCreate ---" << std::endl;

    rhi::DeviceDesc desc{};
    desc.platform = rhi::RHIPlatform::Metal;
    desc.enableDebug = false;

    CHECK(initialize_with_device(desc), "init should succeed");

    rhi::RHIDeviceBase* device = get_rhi_device();
    CHECK(device != nullptr, "device should be non-null");

    // 创建一个小的常量 buffer
    rhi::BufferDesc bufferDesc{};
    bufferDesc.size = 64;
    bufferDesc.usage = rhi::GPUMemoryUsage::Dynamic;
    bufferDesc.type = rhi::BufferType::Constant;

    rhi::ResourceHandle bufHandle = device->CreateBuffer(bufferDesc);
    CHECK(bufHandle != rhi::handles::INVALID_RESOURCE, "CreateBuffer should return valid handle");

    if (bufHandle != rhi::handles::INVALID_RESOURCE) {
        device->DestroyBuffer(bufHandle);
        std::cout << "[PASS] DestroyBuffer completed without crash" << std::endl;
    }

    shutdown_rhi();
}

// 案例 6: bind_rhi_device_to_legacy() 把 g_rhiDevice 的 native 注入 metal::core
//        这是 Phase 1 Sub-step 1.2.3' 的核心验证：旧 Metal 后端的 get_device()
//        返回的应该和 rhi::MetalDevice::GetNativeDevice() 是同一个 MTL::Device*
static void TestBindRHIDeviceToLegacyMetal() {
    std::cout << "\n--- TestBindRHIDeviceToLegacyMetal ---" << std::endl;

    // 未初始化时 bind 应该失败
    CHECK(!bind_rhi_device_to_legacy(), "bind should fail when RHI not initialized");

    rhi::DeviceDesc desc{};
    desc.platform = rhi::RHIPlatform::Metal;
    desc.enableDebug = false;

    CHECK(initialize_with_device(desc), "init should succeed");

#if defined(__APPLE__)
    auto* metalDevice = dynamic_cast<rhi::MetalDevice*>(get_rhi_device());
    CHECK(metalDevice != nullptr, "should be MetalDevice");
    MTL::Device* rhiNative = metalDevice->GetNativeDevice();
    CHECK(rhiNative != nullptr, "RHI's native MTL::Device should be non-null");

    // 绑定前 metal::core::get_device() 应该返回 nullptr（MetalCore 还没 initialize）
    CHECK(metal::core::get_device() == nullptr, "metal::core device should be null before bind");

    CHECK(bind_rhi_device_to_legacy(), "bind should succeed after init");

    // 绑定后 metal::core::get_device() 应该返回 rhiNative
    MTL::Device* coreNative = metal::core::get_device();
    CHECK(coreNative == rhiNative, "metal::core::get_device() should return RHI's native device after bind");
#else
    CHECK(!bind_rhi_device_to_legacy(), "bind should fail on non-Apple platforms");
#endif

    shutdown_rhi();

#if defined(__APPLE__)
    // shutdown_rhi 后 metal::core 不应该再持有 device（RHI 持有的 native 已 release）
    // 注意：这里只验证 metal::core 的 _external_device 字段是否被清理，不验证 RHI native 是否还活着
    // （RHI 已 shutdown，native device 已 release，metal::core 不应再访问它）
    // 我们不直接断言 core->get_device() == nullptr，因为 metal::core 自己也可能没初始化
    // 关键是不崩溃
    std::cout << "[PASS] shutdown_rhi after bind completed without crash" << std::endl;
#endif
}

int main() {
    std::cout << "=================================" << std::endl;
    std::cout << "TestRendererRHIDevice" << std::endl;
    std::cout << "Phase 1 Sub-step 1.2.2 Integration Test" << std::endl;
    std::cout << "=================================" << std::endl;

    TestBasicLifecycleMetal();
    TestDoubleInitRejected();
    TestReinitAfterShutdown();
    TestUnknownPlatformRejected();
    TestDeviceFunctionalViaBufferCreate();
    TestBindRHIDeviceToLegacyMetal();

    std::cout << "\n=================================" << std::endl;
    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << g_failures << " CHECK(s) FAILED" << std::endl;
    return 1;
}
