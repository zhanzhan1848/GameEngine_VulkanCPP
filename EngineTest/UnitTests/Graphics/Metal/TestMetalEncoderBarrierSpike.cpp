/**
 * @file TestMetalEncoderBarrierSpike.cpp
 * @brief PR4 Spike: 验证 intra-encoder memoryBarrier 是否足够同步 compute dispatches
 *
 * @details
 * 验证目标:
 *   - 同一 MTL::ComputeCommandEncoder 内
 *   - Dispatch A (writer) → memoryBarrier(Buffers) → Dispatch B (reader)
 *   - 不 endCurrentEncoder
 *   - B 是否能读到 A 的写入?
 *
 * 预期结果(根据 Metal spec):
 *   - YES: memoryBarrierWithScope 强制 dispatch A 完成后才允许 dispatch B 读/写
 *
 * 决策含义:
 *   - 若 PASS: 可以保留 computeEncoder->memoryBarrier(scope),删除冗余的 endCurrentEncoder()
 *   - 若 FAIL: 保留当前保守策略,关闭 PR4
 *
 * 测试用 raw metal-cpp API,绕开 RHI MemoryBarrier 的 endCurrentEncoder 副作用。
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-07-02
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"

#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>
#include <cstring>
#include <iostream>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

namespace {

// 辅助:从 MSL 源码编译 library
MTL::Library* CompileLibrary(MTL::Device* device, const char* src) {
    NS::String* nsSrc = NS::String::string(src, NS::UTF8StringEncoding);
    NS::Error* err = nullptr;
    MTL::Library* lib = device->newLibrary(nsSrc, nullptr, &err);
    if (err) {
        std::cerr << "[Spike] Library compile error: "
                  << err->localizedDescription()->utf8String() << std::endl;
        if (lib) lib->release();
        return nullptr;
    }
    return lib;
}

MTL::ComputePipelineState* MakeComputePSO(MTL::Device* device, MTL::Library* lib, const char* fnName) {
    NS::String* nsName = NS::String::string(fnName, NS::UTF8StringEncoding);
    MTL::Function* fn = lib->newFunction(nsName);
    if (!fn) {
        std::cerr << "[Spike] Function not found: " << fnName << std::endl;
        return nullptr;
    }
    NS::Error* err = nullptr;
    MTL::ComputePipelineState* pso = device->newComputePipelineState(fn, &err);
    fn->release();
    if (err) {
        std::cerr << "[Spike] PSO error: "
                  << err->localizedDescription()->utf8String() << std::endl;
        if (pso) pso->release();
        return nullptr;
    }
    return pso;
}

// Writer: writes known pattern 0xDEADBEEF to buf[id]
constexpr const char* kWriterSrc = R"(
    #include <metal_stdlib>
    using namespace metal;
    kernel void writer(device uint* buf [[buffer(0)]],
                       uint id [[thread_position_in_grid]]) {
        buf[id] = 0xDEADBEEFu;
    }
)";

// Reader: atomic-increment counter if buf[id] == 0xDEADBEEF
constexpr const char* kReaderSrc = R"(
    #include <metal_stdlib>
    using namespace metal;
    kernel void reader(device uint* buf [[buffer(0)]],
                       device atomic_uint* counter [[buffer(1)]],
                       uint id [[thread_position_in_grid]]) {
        if (buf[id] == 0xDEADBEEFu) {
            atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
        }
    }
)";

constexpr uint32_t kExpected = 0xDEADBEEFu;
constexpr uint32_t kThreads  = 64;

// 清零 buffer
void ZeroBuffer(MTL::Buffer* b, size_t size) {
    void* p = b->contents();
    std::memset(p, 0, size);
    if (b->storageMode() == MTL::StorageModeManaged) {
        b->didModifyRange(NS::Range::Make(0, size));
    }
}

} // namespace

// ============================================================================
// SPIKE TEST 1: Tracked resource + intra-encoder memoryBarrier
//              预期: reader 读到 writer 写入,counter == kThreads
// ============================================================================
TestResult TestSpike_IntraEncoderBarrier_Tracked() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = true;
    MetalDevice device(desc);
    if (!device.Initialize()) return TestResult::Failed;

    MTL::Device* mtl = device.GetNativeDevice();

    // 编译两个 compute shader
    MTL::Library* writerLib = CompileLibrary(mtl, kWriterSrc);
    MTL::Library* readerLib = CompileLibrary(mtl, kReaderSrc);
    TEST_ASSERT(writerLib != nullptr, "writer library compile failed");
    TEST_ASSERT(readerLib != nullptr, "reader library compile failed");

    MTL::ComputePipelineState* writerPSO = MakeComputePSO(mtl, writerLib, "writer");
    MTL::ComputePipelineState* readerPSO = MakeComputePSO(mtl, readerLib, "reader");
    TEST_ASSERT(writerPSO != nullptr, "writer PSO failed");
    TEST_ASSERT(readerPSO != nullptr, "reader PSO failed");

    // 创建 Tracked buffer(显式)
    MTL::ResourceOptions opts = MTL::ResourceStorageModeShared
                              | MTL::ResourceHazardTrackingModeTracked;
    MTL::Buffer* dataBuf = mtl->newBuffer(kThreads * sizeof(uint32_t), opts);
    MTL::Buffer* counterBuf = mtl->newBuffer(sizeof(uint32_t), opts);
    TEST_ASSERT(dataBuf != nullptr, "dataBuf alloc failed");
    TEST_ASSERT(counterBuf != nullptr, "counterBuf alloc failed");
    ZeroBuffer(dataBuf, kThreads * sizeof(uint32_t));
    ZeroBuffer(counterBuf, sizeof(uint32_t));

    // Queue + cmdbuf
    MTL::CommandQueue* queue = mtl->newCommandQueue();
    MTL::CommandBuffer* cmd = queue->commandBuffer();
    TEST_ASSERT(cmd != nullptr, "cmdbuf alloc failed");

    // === SPIKE: 同 encoder 内 dispatch A → memoryBarrier → dispatch B ===
    MTL::ComputeCommandEncoder* enc = cmd->computeCommandEncoder();
    TEST_ASSERT(enc != nullptr, "compute encoder alloc failed");

    // Dispatch A: writer
    enc->setComputePipelineState(writerPSO);
    enc->setBuffer(dataBuf, 0, 0);
    enc->dispatchThreadgroups(MTL::Size::Make(kThreads, 1, 1),
                              MTL::Size::Make(1, 1, 1));

    // Intra-encoder barrier — 我们要验证的关键调用
    enc->memoryBarrier(MTL::BarrierScopeBuffers);

    // Dispatch B: reader(同 encoder,没有 endEncoding)
    enc->setComputePipelineState(readerPSO);
    enc->setBuffer(dataBuf, 0, 0);
    enc->setBuffer(counterBuf, 0, 1);
    enc->dispatchThreadgroups(MTL::Size::Make(kThreads, 1, 1),
                              MTL::Size::Make(1, 1, 1));

    enc->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();

    // 验证 counter
    uint32_t counter = *static_cast<uint32_t*>(counterBuf->contents());
    std::cerr << "[Spike1] Tracked + intra-encoder barrier: counter="
              << counter << " expected=" << kThreads << std::endl;

    TEST_ASSERT(counter == kThreads,
                "Intra-encoder barrier should sync writer→reader");

    // Cleanup
    cmd->release();
    queue->release();
    counterBuf->release();
    dataBuf->release();
    readerPSO->release();
    writerPSO->release();
    readerLib->release();
    writerLib->release();
    device.Shutdown();
    return TestResult::Passed;
}

// ============================================================================
// SPIKE TEST 2: Untracked resource + intra-encoder memoryBarrier
//              验证 memoryBarrier(scope) 是否独立于 Tracked 模式工作
//              预期: 也应工作 — barrier 是 encoder 级别的,不依赖资源 flag
// ============================================================================
TestResult TestSpike_IntraEncoderBarrier_Untracked() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = true;
    MetalDevice device(desc);
    if (!device.Initialize()) return TestResult::Failed;

    MTL::Device* mtl = device.GetNativeDevice();

    MTL::Library* writerLib = CompileLibrary(mtl, kWriterSrc);
    MTL::Library* readerLib = CompileLibrary(mtl, kReaderSrc);
    TEST_ASSERT(writerLib != nullptr, "writer library compile failed");
    TEST_ASSERT(readerLib != nullptr, "reader library compile failed");

    MTL::ComputePipelineState* writerPSO = MakeComputePSO(mtl, writerLib, "writer");
    MTL::ComputePipelineState* readerPSO = MakeComputePSO(mtl, readerLib, "reader");
    TEST_ASSERT(writerPSO != nullptr, "writer PSO failed");
    TEST_ASSERT(readerPSO != nullptr, "reader PSO failed");

    // 注意:Untracked(默认) — 没有 HazardTrackingModeTracked
    MTL::ResourceOptions opts = MTL::ResourceStorageModeShared; // default untracked
    MTL::Buffer* dataBuf = mtl->newBuffer(kThreads * sizeof(uint32_t), opts);
    MTL::Buffer* counterBuf = mtl->newBuffer(sizeof(uint32_t), opts);
    TEST_ASSERT(dataBuf != nullptr, "dataBuf alloc failed");
    TEST_ASSERT(counterBuf != nullptr, "counterBuf alloc failed");
    ZeroBuffer(dataBuf, kThreads * sizeof(uint32_t));
    ZeroBuffer(counterBuf, sizeof(uint32_t));

    MTL::CommandQueue* queue = mtl->newCommandQueue();
    MTL::CommandBuffer* cmd = queue->commandBuffer();
    TEST_ASSERT(cmd != nullptr, "cmdbuf alloc failed");

    MTL::ComputeCommandEncoder* enc = cmd->computeCommandEncoder();
    TEST_ASSERT(enc != nullptr, "compute encoder alloc failed");

    enc->setComputePipelineState(writerPSO);
    enc->setBuffer(dataBuf, 0, 0);
    enc->dispatchThreadgroups(MTL::Size::Make(kThreads, 1, 1),
                              MTL::Size::Make(1, 1, 1));

    enc->memoryBarrier(MTL::BarrierScopeBuffers);

    enc->setComputePipelineState(readerPSO);
    enc->setBuffer(dataBuf, 0, 0);
    enc->setBuffer(counterBuf, 0, 1);
    enc->dispatchThreadgroups(MTL::Size::Make(kThreads, 1, 1),
                              MTL::Size::Make(1, 1, 1));

    enc->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();

    uint32_t counter = *static_cast<uint32_t*>(counterBuf->contents());
    std::cerr << "[Spike2] Untracked + intra-encoder barrier: counter="
              << counter << " expected=" << kThreads << std::endl;

    TEST_ASSERT(counter == kThreads,
                "Intra-encoder barrier should work regardless of tracking mode");

    cmd->release();
    queue->release();
    counterBuf->release();
    dataBuf->release();
    readerPSO->release();
    writerPSO->release();
    readerLib->release();
    writerLib->release();
    device.Shutdown();
    return TestResult::Passed;
}

// ============================================================================
// SPIKE TEST 3 (Control): Untracked + NO barrier — 验证如果没有 barrier,可能读到 stale
//                       注意:在 Apple Silicon 上,compute dispatch 在同 encoder 内
//                       可能被硬件串行化,本测试可能仍 PASS(counter == kThreads)
//                       这并不说明 barrier 不需要,而是硬件实现更严格
//                       如果测试 FAIL(counter < kThreads),则说明硬件确实需要 barrier
// ============================================================================
TestResult TestSpike_NoBarrier_Control() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = true;
    MetalDevice device(desc);
    if (!device.Initialize()) return TestResult::Failed;

    MTL::Device* mtl = device.GetNativeDevice();

    MTL::Library* writerLib = CompileLibrary(mtl, kWriterSrc);
    MTL::Library* readerLib = CompileLibrary(mtl, kReaderSrc);
    MTL::ComputePipelineState* writerPSO = MakeComputePSO(mtl, writerLib, "writer");
    MTL::ComputePipelineState* readerPSO = MakeComputePSO(mtl, readerLib, "reader");
    TEST_ASSERT(writerPSO && readerPSO, "PSO creation failed");

    MTL::ResourceOptions opts = MTL::ResourceStorageModeShared;
    MTL::Buffer* dataBuf = mtl->newBuffer(kThreads * sizeof(uint32_t), opts);
    MTL::Buffer* counterBuf = mtl->newBuffer(sizeof(uint32_t), opts);
    ZeroBuffer(dataBuf, kThreads * sizeof(uint32_t));
    ZeroBuffer(counterBuf, sizeof(uint32_t));

    MTL::CommandQueue* queue = mtl->newCommandQueue();
    MTL::CommandBuffer* cmd = queue->commandBuffer();

    MTL::ComputeCommandEncoder* enc = cmd->computeCommandEncoder();

    enc->setComputePipelineState(writerPSO);
    enc->setBuffer(dataBuf, 0, 0);
    enc->dispatchThreadgroups(MTL::Size::Make(kThreads, 1, 1),
                              MTL::Size::Make(1, 1, 1));

    // === NO memoryBarrier ===

    enc->setComputePipelineState(readerPSO);
    enc->setBuffer(dataBuf, 0, 0);
    enc->setBuffer(counterBuf, 0, 1);
    enc->dispatchThreadgroups(MTL::Size::Make(kThreads, 1, 1),
                              MTL::Size::Make(1, 1, 1));

    enc->endEncoding();
    cmd->commit();
    cmd->waitUntilCompleted();

    uint32_t counter = *static_cast<uint32_t*>(counterBuf->contents());
    std::cerr << "[Spike3] (Control) Untracked + NO barrier: counter="
              << counter << " expected=" << kThreads
              << " (either OK or <expected; both are valid Metal behavior)"
              << std::endl;
    // 不做断言 — 这只是观察控制组
    // counter == kThreads 也合法(hw 串行化),counter < kThreads 也合法(spec 允许)

    cmd->release();
    queue->release();
    counterBuf->release();
    dataBuf->release();
    readerPSO->release();
    writerPSO->release();
    readerLib->release();
    writerLib->release();
    device.Shutdown();
    return TestResult::Passed;
}

void RegisterMetalEncoderBarrierSpikeTests() {
    auto suite = std::make_shared<TestSuite>("MetalEncoderBarrierSpike");
    suite->AddTestCase(TestCase("Spike_IntraBarrier_Tracked",
                                TestSpike_IntraEncoderBarrier_Tracked));
    suite->AddTestCase(TestCase("Spike_IntraBarrier_Untracked",
                                TestSpike_IntraEncoderBarrier_Untracked));
    suite->AddTestCase(TestCase("Spike_NoBarrier_Control",
                                TestSpike_NoBarrier_Control));
    TestRunner::RegisterTestSuite(suite);
}

#ifndef UNIT_TEST_LIB
int main() {
    RegisterMetalEncoderBarrierSpikeTests();
    TestRunner::RunAllSuites();
    return 0;
}
#endif
