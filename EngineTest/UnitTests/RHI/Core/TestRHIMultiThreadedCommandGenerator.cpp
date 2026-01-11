/**
 * @file TestRHIMultiThreadedCommandGenerator.cpp
 * @brief RHI多线程命令生成器单元测试
 * @details 测试多线程命令缓冲区生成的各种功能和性能
 * 
 * @author Engine开发团队
 * @date 2025-12-29
 * @version 1.0
 */

#include "Engine/Common/CommonHeaders.h"
#include "../../TestFramework.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIMultiThreadedCommandGenerator.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>

using namespace Engine::Test;
using namespace primal::graphics::rhi;

// === 模拟设备类 ===

// 简单的RHIDevice实现用于测试
class MockRHIDevice : public RHIDevice<MockRHIDevice> {
public:
    MockRHIDevice() : RHIDevice<MockRHIDevice>(DeviceDesc{}) {
        auto& desc = const_cast<DeviceDesc&>(GetDesc());
        desc.platform = RHIPlatform::Metal;
        desc.enableDebug = false;
        // 初始化设备
        Initialize();
    }
    
    // === CRTP实现方法 ===
    bool initializeImpl() {
        return true;
    }
    
    void queryDeviceInfo(DeviceInfo& info) {
        strcpy(info.deviceName, "MockDevice");
        strcpy(info.driverVersion, "1.0.0");
    }
    
    uint32_t getCurrentFrameIndexImpl() const {
        return 0;
    }
    
    void waitIdleImpl() const {
        // Mock implementation
    }
    
    void shutdownImpl() {
        // Mock implementation
    }
    
    // 资源创建实现（测试用，返回无效句柄）
    ResourceHandle createBufferImpl(const BufferDesc& desc) {
        return handles::INVALID_RESOURCE;
    }
    
    ResourceHandle createTextureImpl(const TextureDesc& desc) {
        return handles::INVALID_RESOURCE;
    }
    
    ShaderHandle createShaderImpl(const void* data, size_t size, ShaderStage stage, const char* entryPoint) {
        return handles::INVALID_SHADER;
    }
    
    PipelineHandle createGraphicsPipelineImpl(const GraphicsPipelineDesc& desc) {
        return handles::INVALID_PIPELINE;
    }
    
    PipelineHandle createComputePipelineImpl(const ComputePipelineDesc& desc) {
        return handles::INVALID_PIPELINE;
    }
    
    CommandBufferHandle createCommandBufferImpl(CommandQueueType type) {
        return nextCommandBufferHandle_++;
    }
    
    // 资源销毁实现（测试用，空实现）
    void destroyBufferImpl(ResourceHandle handle) {}
    void destroyTextureImpl(ResourceHandle handle) {}
    void destroyShaderImpl(ShaderHandle handle) {}
    void destroyPipelineImpl(PipelineHandle handle) {}
    void* mapBufferImpl(ResourceHandle handle, u64 offset, u64 size) { return nullptr; }
    void unmapBufferImpl(ResourceHandle handle) {}
    void destroyCommandBufferImpl(CommandBufferHandle handle) {}
    
    // 命令提交实现
    bool submitCommandBufferImpl(CommandBufferHandle handle) {
        return true; // Mock实现，总是返回成功
    }
    
    // 同步对象创建实现
    SyncHandle createSyncImpl() {
        return nextSyncHandle_++;
    }
    
    void destroySyncImpl(SyncHandle handle) {}

    QueryPoolHandle createQueryPoolImpl(const QueryPoolDesc& desc) { return handles::INVALID_QUERY_POOL; }
    void destroyQueryPoolImpl(QueryPoolHandle handle) {}

    SamplerHandle createSamplerImpl(const SamplerDesc& desc) { return handles::INVALID_SAMPLER; }
    void destroySamplerImpl(SamplerHandle handle) {}

    DescriptorSetLayoutHandle createDescriptorSetLayoutImpl(const DescriptorSetLayoutDesc& desc) { return handles::INVALID_DESCRIPTOR_SET_LAYOUT; }
    void destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle handle) {}

    PipelineLayoutHandle createPipelineLayoutImpl(const PipelineLayoutDesc& desc) { return handles::INVALID_PIPELINE_LAYOUT; }
    void destroyPipelineLayoutImpl(PipelineLayoutHandle handle) {}

    DescriptorSetHandle createDescriptorSetImpl(const DescriptorSetDesc& desc) { return handles::INVALID_DESCRIPTOR_SET; }
    void destroyDescriptorSetImpl(DescriptorSetHandle handle) {}

    void updateDescriptorSetsImpl(uint32_t writeCount, const WriteDescriptorSet* writes) {}

    // 同步等待实现
    bool waitForSyncImpl(SyncHandle handle, u32 timeoutMs) {
        return WaitForSync(handle, timeoutMs);
    }
    
    SyncHandle CreateSync() {
        return 1; // Mock实现，返回固定句柄
    }
    
    bool WaitForSync(SyncHandle handle, u32 timeoutMs) {
        return true; // Mock实现，总是返回成功
    }
    
private:
    // === 私有成员变量 ===
    std::atomic<uint64_t> nextSyncHandle_{1};
    std::atomic<uint64_t> nextCommandBufferHandle_{1};
};

/**
 * @brief 创建测试用的RHI设备
 * @return RHI设备指针
 */
static std::unique_ptr<MockRHIDevice> CreateTestDevice() {
    return std::make_unique<MockRHIDevice>();
}

/**
 * @brief 创建测试用的渲染场景
 * @param meshCount 网格数量
 * @param drawCallCount 绘制调用数量
 * @return 渲染场景
 */
static RenderScene CreateTestRenderScene(uint32_t meshCount, uint32_t drawCallCount) {
    RenderScene scene;
    
    // 预分配空间
    scene.meshes.reserve(meshCount);
    scene.materials.reserve(meshCount);
    scene.textures.reserve(meshCount * 4);
    scene.transforms.reserve(meshCount);
    scene.materialIndices.reserve(meshCount);
    scene.meshIndices.reserve(meshCount);
    scene.drawCalls.reserve(drawCallCount);
    
    // 填充测试数据
    for (uint32_t i = 0; i < meshCount; ++i) {
        scene.meshes.push_back(static_cast<ResourceHandle>(0x200000000ULL | i));
        scene.materials.push_back(static_cast<ResourceHandle>(0x300000000ULL | i));
        scene.transforms.emplace_back(); // 使用默认构造函数
        scene.materialIndices.push_back(i % 10);
        scene.meshIndices.push_back(i);
        
        // 每个网格添加4个纹理
        for (uint32_t j = 0; j < 4; ++j) {
            scene.textures.push_back(static_cast<ResourceHandle>(0x400000000ULL | (i * 4 + j)));
        }
    }
    
    for (uint32_t i = 0; i < drawCallCount; ++i) {
        scene.drawCalls.push_back(i);
    }
    
    scene.totalDrawCalls = drawCallCount;
    scene.totalVertices = meshCount * 1000;
    scene.totalTriangles = meshCount * 500;
    scene.boundingSphereRadius = 100.0f;
    scene.boundingBoxMin = math::v3{-50.0f, -50.0f, -50.0f};
    scene.boundingBoxMax = math::v3{50.0f, 50.0f, 50.0f};
    
    return scene;
}

/**
 * @brief 测试基本初始化功能
 * @return 测试结果
 */
TestResult TestBasicInitialization() {
    auto device = CreateTestDevice();
    TEST_ASSERT_NOT_NULL(device.get(), "创建RHI设备");
    
    MultiThreadConfig config;
    config.workerThreadCount = 4;
    config.enablePerformanceMonitoring = true;
    config.maxConcurrentTasks = 16;
    
    RHIMultiThreadedCommandGenerator generator(*device, config);
    
    // 测试初始化
    bool initResult = generator.Initialize();
    TEST_ASSERT(initResult, "初始化多线程命令生成器");
    
    // 验证配置
    TEST_ASSERT_EQ(4, generator.GetWorkerThreadCount(), "验证工作线程数量");
    
    // 验证系统状态
    bool systemState = generator.ValidateSystemState();
    TEST_ASSERT(systemState, "验证系统状态");
    
    // 测试关闭
    generator.Shutdown();
    bool stateAfterShutdown = generator.ValidateSystemState();
    TEST_ASSERT(!stateAfterShutdown, "验证关闭后系统状态");
    
    return TestResult::Passed;
}

/**
 * @brief 测试并行命令生成功能
 * @return 测试结果
 */
TestResult TestParallelCommandGeneration() {
    auto device = CreateTestDevice();
    RHIMultiThreadedCommandGenerator generator(*device);
    
    TEST_ASSERT(generator.Initialize(), "初始化生成器");
    
    // 创建中等规模的测试场景
    RenderScene scene = CreateTestRenderScene(100, 500);
    TEST_ASSERT_EQ(500, scene.totalDrawCalls, "验证场景绘制调用数量");
    TEST_ASSERT_EQ(100, scene.meshes.size(), "验证场景网格数量");
    
    // 测试并行生成
    std::vector<CommandBufferHandle> outputs;
    
    // 使用现有场景进行测试
    
    bool generationResult = generator.GenerateCommandsParallel(scene, outputs);
    TEST_ASSERT(generationResult, "并行生成命令缓冲区");
    
    TEST_ASSERT(!outputs.empty(), "验证输出不为空");
    
    // 验证生成的命令缓冲区数量合理
    TEST_ASSERT(outputs.size() <= scene.totalDrawCalls, "命令缓冲区数量不应超过绘制调用数量");
    
    // 测试等待所有任务完成
    bool waitResult = generator.WaitForAllTasks(5000); // 5秒超时
    TEST_ASSERT(waitResult, "等待所有任务完成");
    
    generator.Shutdown();
    return TestResult::Passed;
}

/**
 * @brief 测试单命令缓冲区生成功能
 * @return 测试结果
 */
TestResult TestSingleCommandBufferGeneration() {
    auto device = CreateTestDevice();
    RHIMultiThreadedCommandGenerator generator(*device);
    
    TEST_ASSERT(generator.Initialize(), "初始化生成器");
    
    // 创建小规模测试场景
    RenderScene scene = CreateTestRenderScene(10, 50);
    
    CommandBufferHandle commandBuffer;
    bool generationResult = generator.GenerateCommandBuffer(scene, commandBuffer);
    TEST_ASSERT(generationResult, "生成单个命令缓冲区");
    TEST_ASSERT_NE(handles::INVALID_COMMAND_BUFFER, commandBuffer, "验证命令缓冲区句柄有效性");
    
    generator.Shutdown();
    return TestResult::Passed;
}

/**
 * @brief 测试线程管理功能
 * @return 测试结果
 */
TestResult TestThreadManagement() {
    auto device = CreateTestDevice();
    RHIMultiThreadedCommandGenerator generator(*device);
    
    TEST_ASSERT(generator.Initialize(), "初始化生成器");
    
    // 测试动态调整线程数量
    generator.SetWorkerThreadCount(8);
    TEST_ASSERT_EQ(8, generator.GetWorkerThreadCount(), "设置8个工作线程");
    
    generator.SetWorkerThreadCount(1);
    TEST_ASSERT_EQ(1, generator.GetWorkerThreadCount(), "设置1个工作线程");
    
    generator.SetWorkerThreadCount(std::thread::hardware_concurrency());
    uint32_t hardwareThreads = generator.GetWorkerThreadCount();
    TEST_ASSERT(hardwareThreads > 0, "设置硬件并发线程数");
    
    // 测试活跃线程查询
    uint32_t activeThreads = generator.GetActiveThreadCount();
    TEST_ASSERT(activeThreads > 0, "查询活跃线程数量");
    TEST_ASSERT(activeThreads <= hardwareThreads, "活跃线程数不超过设置的最大值");
    
    generator.Shutdown();
    return TestResult::Passed;
}

/**
 * @brief 测试配置管理功能
 * @return 测试结果
 */
TestResult TestConfigurationManagement() {
    auto device = CreateTestDevice();
    
    // 创建自定义配置
    MultiThreadConfig customConfig;
    customConfig.workerThreadCount = 6;
    customConfig.enablePerformanceMonitoring = false;
    customConfig.enableAdaptiveTuning = true;
    customConfig.maxConcurrentTasks = 32;
    customConfig.commandCacheSize = 2048;
    customConfig.resourceCacheSize = 1024;
    
    RHIMultiThreadedCommandGenerator generator(*device, customConfig);
    TEST_ASSERT(generator.Initialize(), "使用自定义配置初始化生成器");
    
    // 验证初始配置
    const MultiThreadConfig& initialConfig = generator.GetConfig();
    TEST_ASSERT_EQ(6, initialConfig.workerThreadCount, "验证初始线程数量");
    TEST_ASSERT(!initialConfig.enablePerformanceMonitoring, "验证性能监控初始状态");
    TEST_ASSERT(initialConfig.enableAdaptiveTuning, "验证自适应调优初始状态");
    TEST_ASSERT_EQ(32, initialConfig.maxConcurrentTasks, "验证最大并发任务数");
    
    // 动态更新配置
    MultiThreadConfig updatedConfig;
    updatedConfig.workerThreadCount = 12;
    updatedConfig.enablePerformanceMonitoring = true;
    updatedConfig.enableAdaptiveTuning = false;
    updatedConfig.maxConcurrentTasks = 64;
    
    generator.UpdateConfig(updatedConfig);
    
    // 验证更新后配置
    const MultiThreadConfig& newConfig = generator.GetConfig();
    TEST_ASSERT_EQ(12, newConfig.workerThreadCount, "验证更新后线程数量");
    TEST_ASSERT(newConfig.enablePerformanceMonitoring, "验证更新后性能监控状态");
    TEST_ASSERT(!newConfig.enableAdaptiveTuning, "验证更新后自适应调优状态");
    TEST_ASSERT_EQ(64, newConfig.maxConcurrentTasks, "验证更新后最大并发任务数");
    
    generator.Shutdown();
    return TestResult::Passed;
}

/**
 * @brief 测试性能监控功能
 * @return 测试结果
 */
TestResult TestPerformanceMonitoring() {
    auto device = CreateTestDevice();
    RHIMultiThreadedCommandGenerator generator(*device);
    
    TEST_ASSERT(generator.Initialize(), "初始化生成器");
    
    // 启用性能监控
    generator.SetPerformanceMonitoringEnabled(true);
    
    // 创建测试场景并生成命令以产生性能数据
    RenderScene scene = CreateTestRenderScene(200, 1000);
    std::vector<CommandBufferHandle> outputs;
    
    auto startTime = std::chrono::high_resolution_clock::now();
    bool generationResult = generator.GenerateCommandsParallel(scene, outputs);
    auto endTime = std::chrono::high_resolution_clock::now();
    
    TEST_ASSERT(generationResult, "生成命令以收集性能数据");
    
    // 等待所有任务完成
    generator.WaitForAllTasks(10000);
    
    // 获取性能指标
    const MultiThreadPerformanceMetrics& metrics = generator.GetPerformanceMetrics();
    TEST_ASSERT_NOT_NULL(&metrics, "获取性能指标结构");
    
    // 验证性能指标的合理性
    TEST_ASSERT(metrics.commandsGeneratedPerSecond >= 0.0, "验证命令生成速率");
    TEST_ASSERT(metrics.averageGenerationTimeMs >= 0.0, "验证平均生成时间");
    TEST_ASSERT(metrics.cpuUtilizationPercent >= 0.0 && metrics.cpuUtilizationPercent <= 100.0, 
                "验证CPU使用率范围");
    TEST_ASSERT(metrics.activeWorkerThreads > 0, "验证活跃工作线程数");
    
    // 重置性能指标
    generator.ResetPerformanceMetrics();
    const MultiThreadPerformanceMetrics& resetMetrics = generator.GetPerformanceMetrics();
    TEST_ASSERT_EQ(0.0, resetMetrics.commandsGeneratedPerSecond, "验证重置后的命令生成速率");
    TEST_ASSERT_EQ(0.0, resetMetrics.averageGenerationTimeMs, "验证重置后的平均生成时间");
    
    // 测试性能报告导出
    const char* reportFile = "test_performance_report.txt";
    bool exportResult = generator.ExportPerformanceReport(reportFile);
    TEST_ASSERT(exportResult, "导出性能报告文件");
    
    // 清理测试文件
    std::remove(reportFile);
    
    generator.Shutdown();
    return TestResult::Passed;
}

/**
 * @brief 测试调试功能
 * @return 测试结果
 */
TestResult TestDebugFeatures() {
    auto device = CreateTestDevice();
    RHIMultiThreadedCommandGenerator generator(*device);
    
    TEST_ASSERT(generator.Initialize(), "初始化生成器");
    
    // 测试系统状态查询
    const char* status = generator.GetSystemStatus();
    TEST_ASSERT_NOT_NULL(status, "获取系统状态字符串");
    TEST_ASSERT_STR_EQ("Initialized", status, "验证系统状态为已初始化");
    
    generator.Shutdown();
    status = generator.GetSystemStatus();
    TEST_ASSERT_STR_EQ("Shutdown", status, "验证关闭后系统状态");
    
    // 重新初始化以测试调试信息
    generator.Initialize();
    
    // 测试调试信息获取
    char debugBuffer[512];
    uint32_t infoLength = generator.GetDebugInfo(debugBuffer, sizeof(debugBuffer));
    TEST_ASSERT(infoLength > 0, "获取调试信息长度");
    TEST_ASSERT(infoLength < sizeof(debugBuffer), "调试信息长度在缓冲区范围内");
    TEST_ASSERT(strlen(debugBuffer) > 0, "验证调试信息内容不为空");
    
    generator.Shutdown();
    return TestResult::Passed;
}

/**
 * @brief 测试边界条件和异常情况
 * @return 测试结果
 */
TestResult TestEdgeCasesAndErrorHandling() {
    auto device = CreateTestDevice();
    RHIMultiThreadedCommandGenerator generator(*device);
    
    // 测试未初始化状态下的操作
    RenderScene scene = CreateTestRenderScene(10, 50);
    std::vector<CommandBufferHandle> outputs;
    
    bool uninitializedResult = generator.GenerateCommandsParallel(scene, outputs);
    TEST_ASSERT(!uninitializedResult, "未初始化状态下生成命令应失败");
    
    CommandBufferHandle commandBuffer;
    bool uninitializedSingleResult = generator.GenerateCommandBuffer(scene, commandBuffer);
    TEST_ASSERT(!uninitializedSingleResult, "未初始化状态下生成单个命令缓冲区应失败");
    
    // 初始化后测试空场景
    TEST_ASSERT(generator.Initialize(), "初始化生成器");
    
    RenderScene emptyScene;
    bool emptySceneResult = generator.GenerateCommandsParallel(emptyScene, outputs);
    TEST_ASSERT(emptySceneResult, "空场景生成命令应该成功"); // 空场景也是有效的
    
    // 测试零线程配置
    generator.Shutdown();
    MultiThreadConfig zeroThreadConfig;
    zeroThreadConfig.workerThreadCount = 0;
    RHIMultiThreadedCommandGenerator zeroThreadGenerator(*device, zeroThreadConfig);
    
    bool zeroThreadInit = zeroThreadGenerator.Initialize();
    TEST_ASSERT(!zeroThreadInit, "零线程配置应该初始化失败");
    
    generator.Shutdown();
    return TestResult::Passed;
}

/**
 * @brief 测试大规模场景性能
 * @return 测试结果
 */
TestResult TestLargeScalePerformance() {
    auto device = CreateTestDevice();
    
    // 创建高性能配置
    MultiThreadConfig perfConfig;
    perfConfig.workerThreadCount = std::thread::hardware_concurrency();
    perfConfig.enablePerformanceMonitoring = true;
    perfConfig.enableAdaptiveTuning = true;
    perfConfig.enableMemoryOptimization = true;
    perfConfig.enableCacheOptimization = true;
    perfConfig.maxConcurrentTasks = 64;
    
    RHIMultiThreadedCommandGenerator generator(*device, perfConfig);
    TEST_ASSERT(generator.Initialize(), "初始化高性能配置生成器");
    
    // 创建大规模测试场景
    RenderScene largeScene = CreateTestRenderScene(10000, 50000);
    TEST_ASSERT_EQ(50000, largeScene.totalDrawCalls, "验证大规模场景绘制调用数量");
    
    // 测试大规模并行生成
    std::vector<CommandBufferHandle> outputs;
    
    auto startTime = std::chrono::high_resolution_clock::now();
    bool generationResult = generator.GenerateCommandsParallel(largeScene, outputs);
    auto endTime = std::chrono::high_resolution_clock::now();
    
    TEST_ASSERT(generationResult, "大规模场景并行生成命令");
    TEST_ASSERT(!outputs.empty(), "大规模场景输出不应为空");
    
    // 等待所有任务完成
    bool waitResult = generator.WaitForAllTasks(30000); // 30秒超时
    TEST_ASSERT(waitResult, "等待大规模场景任务完成");
    
    // 计算实际耗时
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    double processingTimeMs = static_cast<double>(duration.count());
    
    // 验证性能在合理范围内（这里设置为10秒，实际应根据硬件调整）
    TEST_ASSERT(processingTimeMs < 10000.0, "大规模场景处理时间应在合理范围内");
    
    // 获取并验证性能指标
    const MultiThreadPerformanceMetrics& metrics = generator.GetPerformanceMetrics();
    TEST_ASSERT(metrics.commandsGeneratedPerSecond > 0.0, "验证命令生成速率大于0");
    TEST_ASSERT(metrics.averageGenerationTimeMs > 0.0, "验证平均生成时间大于0");
    
    generator.Shutdown();
    return TestResult::Passed;
}

/**
 * @brief 测试并发安全性
 * @return 测试结果
 */
TestResult TestConcurrencySafety() {
    auto device = CreateTestDevice();
    RHIMultiThreadedCommandGenerator generator(*device);
    
    TEST_ASSERT(generator.Initialize(), "初始化生成器");
    
    // 创建多个线程同时访问生成器
    const int numThreads = 8;
    const int operationsPerThread = 10;
    std::vector<std::thread> threads;
    std::atomic<int> successCount(0);
    std::atomic<int> failureCount(0);
    
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&generator, &successCount, &failureCount, operationsPerThread, t]() {
            for (int i = 0; i < operationsPerThread; ++i) {
                RenderScene scene = CreateTestRenderScene(50, 200);
                std::vector<CommandBufferHandle> outputs;
                
                bool result = generator.GenerateCommandsParallel(scene, outputs);
                if (result) {
                    successCount.fetch_add(1);
                } else {
                    failureCount.fetch_add(1);
                }
                
                // 短暂休眠以增加并发冲突的可能性
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }
    
    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }
    
    // 验证并发操作结果
    int totalOperations = numThreads * operationsPerThread;
    int actualOperations = successCount.load() + failureCount.load();
    TEST_ASSERT_EQ(totalOperations, actualOperations, "验证总操作数量");
    
    // 等待所有任务完成
    generator.WaitForAllTasks(15000);
    
    generator.Shutdown();
    return TestResult::Passed;
}

/**
 * @brief 主测试函数
 * @return 测试结果
 */
int main() {
    // 创建测试套件
    TestSuite suite("RHI多线程命令生成器测试");
    
    // 添加测试用例
    TEST_CASE(suite, "基本初始化测试", TestBasicInitialization);
    TEST_CASE(suite, "并行命令生成测试", TestParallelCommandGeneration);
    TEST_CASE(suite, "单命令缓冲区生成测试", TestSingleCommandBufferGeneration);
    TEST_CASE(suite, "线程管理测试", TestThreadManagement);
    TEST_CASE(suite, "配置管理测试", TestConfigurationManagement);
    TEST_CASE(suite, "性能监控测试", TestPerformanceMonitoring);
    TEST_CASE(suite, "调试功能测试", TestDebugFeatures);
    TEST_CASE(suite, "边界条件测试", TestEdgeCasesAndErrorHandling);
    TEST_CASE(suite, "大规模性能测试", TestLargeScalePerformance);
    TEST_CASE(suite, "并发安全性测试", TestConcurrencySafety);
    
    // 运行所有测试
    TestStats stats = suite.RunAllTests();
    
    // 返回测试结果
    return (stats.failedTests == 0) ? 0 : 1;
}