// ============================================================================
// 文件：TestMetalRenderer.cpp
// 描述：Engine_Test 类的 MacOS/Metal 实现
//       包含 Metal 设备初始化、资源管理和渲染循环
//       演示了多线程渲染命令生成、资源管理和性能监控
// 作者：AI助手
// ============================================================================

#ifdef __APPLE__

#include "TestRenderer.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalSwapChain.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"
#include "Engine/Graphics/RHI/Core/RHIMultiThreadedCommandGenerator.h"
#include "Engine/Platform/Window.h"
#include "Engine/Platform/Platform.h"
#include <memory>
#include <iostream>
#include <vector>
#include <cmath>
#include <string>
#include <chrono>

using namespace primal;
using namespace primal::graphics::rhi;
using namespace primal::platform;

namespace {
    // 简单的 MSL Shader 源码
    const char* kShaderSource = R"(
        #include <metal_stdlib>
        using namespace metal;

        struct Vertex {
            float4 position;
            float4 color;
        };

        struct VertexOut {
            float4 position [[position]];
            float4 color;
        };
        
        // 调试修改：直接使用 buffer(2) 读取顶点数据，绕过 stage_in 和 VertexDescriptor
        // 使用 device 地址空间，更通用
        vertex VertexOut vertexMain(uint vID [[vertex_id]], device const Vertex* vertices [[buffer(2)]]) {
            VertexOut out;
            Vertex v = vertices[vID];
            out.position = v.position;
            out.color = v.color;
            return out;
        }
        
        fragment float4 fragmentMain(VertexOut in [[stage_in]]) {
            return in.color;
        }
    )";

    struct alignas(16) Vertex {
        float position[4];
        float color[4];
    };
    
    struct Uniforms {
        float modelMatrix[16];
    };

    // 辅助函数：创建单位矩阵
    void MatrixIdentity(float* m) {
        for(int i=0; i<16; ++i) m[i] = 0;
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

    // 辅助函数：创建旋转矩阵
    void CreateRotationMatrix(float angle, float* matrix) {
        float c = cos(angle);
        float s = sin(angle);
        MatrixIdentity(matrix);
        // Z轴旋转
        matrix[0] = c; matrix[1] = s;
        matrix[4] = -s; matrix[5] = c;
    }

    struct MetalTestContext {
        platform::window window;
        std::unique_ptr<MetalDevice> device;
        std::unique_ptr<MetalSwapChain> swapChain;
        std::unique_ptr<RHIMultiThreadedCommandGenerator> cmdGenerator;
        
        // 渲染相关
        ShaderHandle vertexShader{handles::INVALID_SHADER};
        ShaderHandle pixelShader{handles::INVALID_SHADER};
        PipelineHandle pipeline{handles::INVALID_PIPELINE};
        ResourceHandle vertexBuffer{handles::INVALID_RESOURCE};
        ResourceHandle uniformBuffer{handles::INVALID_RESOURCE};
        
        // 动画状态
        float rotationAngle = 0.0f;
        
        // 性能统计
        uint32_t frameCount = 0;
        std::chrono::high_resolution_clock::time_point lastTime;
        std::chrono::high_resolution_clock::time_point startTime;
        float fps = 0.0f;
        
        bool initialized = false;
    };
    
    MetalTestContext g_Ctx;
}

#include <mutex>

static std::mutex g_cmdMutex;

/**
 * @brief 命令生成回调
 * @details 由 RHIMultiThreadedCommandGenerator 在工作线程调用
 */
CommandBufferHandle GenerateCommandsCallback(const MultiThreadRenderBatch& batch) {
    std::lock_guard<std::mutex> lock(g_cmdMutex);

    // 1. 创建命令缓冲区
    // 注意：在多线程环境下，CreateCommandBuffer 需要是线程安全的
    // 这里我们假设 RHI 层的分配器是线程安全的
    CommandBufferHandle cmdHandle = g_Ctx.device->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdHandle == handles::INVALID_COMMAND_BUFFER) return handles::INVALID_COMMAND_BUFFER;

    auto* cmdBuffer = g_Ctx.device->GetCommandBuffer(cmdHandle);
    if (!cmdBuffer) return handles::INVALID_COMMAND_BUFFER;

    cmdBuffer->Begin();
    
    // 2. 准备渲染通道
    RenderPassDesc passDesc;
    RenderPassDesc::Attachment colorAtt;
    colorAtt.texture = batch.renderTarget; // 使用 batch 中的 renderTarget
    colorAtt.loadOp = LoadAction::Clear;
    colorAtt.storeOp = StoreAction::Store;
    colorAtt.clearValue = ClearValue(0.0f, 0.0f, 0.5f, 1.0f); // 蓝色
    passDesc.colorAttachments.push_back(colorAtt);
    
    // 手动设置视口
    passDesc.viewport.topLeft = {0.0f, 0.0f};
    passDesc.viewport.size = {static_cast<float>(g_Ctx.window.width()), static_cast<float>(g_Ctx.window.height())};
    passDesc.viewport.minDepth = 0.0f;
    passDesc.viewport.maxDepth = 1.0f;
    
    passDesc.scissor.offset = {0, 0};
    passDesc.scissor.extent = {g_Ctx.window.width(), g_Ctx.window.height()};

    cmdBuffer->BeginRenderPass(passDesc);
    
    // 3. 绑定管线
    cmdBuffer->BindGraphicsPipeline(g_Ctx.pipeline);
    
    // 4. 绑定顶点缓冲区
    uint64_t vOffset = 0;
    // 使用 Slot 2 以避开 VertexDescriptor (通常使用 Slot 0/1) 的潜在干扰，并配合 Shader 的 buffer(2)
    cmdBuffer->BindVertexBuffers(2, 1, &g_Ctx.vertexBuffer, &vOffset);
    
    // 5. 绘制
    cmdBuffer->Draw(3, 0, 1, 0);
    
    cmdBuffer->EndRenderPass();
    cmdBuffer->End();
    
    return cmdHandle;
}

bool Engine_Test::initialize()
{
    // 1. 创建窗口
    window_init_info info{};
    info.caption = "Metal RHI Test Scene";
    info.width = 1280;
    info.height = 720;
    
    g_Ctx.window = create_window(&info);
    if (!g_Ctx.window.is_valid()) {
        std::cerr << "[MetalTest] Failed to create window!" << std::endl;
        return false;
    }

    // 2. 初始化 Metal 设备
    DeviceDesc deviceDesc{};
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.maxFramesInFlight = 3;
    deviceDesc.enableDebug = true;
    
    g_Ctx.device = std::make_unique<MetalDevice>(deviceDesc);
    if (!g_Ctx.device || !g_Ctx.device->Initialize()) {
        std::cerr << "[MetalTest] Failed to initialize Metal device!" << std::endl;
        return false;
    }

    // 3. 创建交换链
    SwapChainDesc swapChainDesc{};
    swapChainDesc.window = g_Ctx.window.handle();
    swapChainDesc.width = g_Ctx.window.width();
    swapChainDesc.height = g_Ctx.window.height();
    swapChainDesc.format = DataFormat::BGRA8_UNorm;
    swapChainDesc.bufferCount = 3;
    swapChainDesc.presentMode = PresentMode::FIFO;

    g_Ctx.swapChain = std::make_unique<MetalSwapChain>(*g_Ctx.device, swapChainDesc);
    if (!g_Ctx.swapChain->Initialize()) {
        std::cerr << "[MetalTest] Failed to initialize swap chain!" << std::endl;
        return false;
    }

    // 4. 创建着色器和管线
    g_Ctx.vertexShader = g_Ctx.device->CreateShader(kShaderSource, strlen(kShaderSource), ShaderStage::Vertex, "vertexMain");
    g_Ctx.pixelShader = g_Ctx.device->CreateShader(kShaderSource, strlen(kShaderSource), ShaderStage::Pixel, "fragmentMain");
    
    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = g_Ctx.vertexShader;
    pipelineDesc.pixelShader = g_Ctx.pixelShader;
    
    VertexInputAttribute attrPos{0, 1, DataFormat::RGBA32_Float, offsetof(Vertex, position)}; // Binding 1
    VertexInputAttribute attrColor{1, 1, DataFormat::RGBA32_Float, offsetof(Vertex, color)};  // Binding 1
    VertexInputBinding bindingDesc{1, sizeof(Vertex), true}; // Binding 1
    
    pipelineDesc.topology = PrimitiveTopology::TriangleList;
    pipelineDesc.vertexAttributes.clear();
    // 使用 Pull Model (直接 Buffer 访问)，禁用 Vertex Descriptor
    pipelineDesc.vertexBindings.clear();
    pipelineDesc.fillMode = FillMode::Solid;
    pipelineDesc.cullMode = CullMode::None;
    pipelineDesc.enableBlend = false;
    pipelineDesc.renderTargetCount = 1;
    pipelineDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    
    g_Ctx.pipeline = g_Ctx.device->CreateGraphicsPipeline(pipelineDesc);

    // 5. 创建并上传顶点数据
    // 使用全屏三角形覆盖 -> 改为屏幕中心小三角形，顺时针
    Vertex vertices[] = {
        { {  0.0f,  0.5f, 0.5f, 1.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } }, // Top, Red
        { {  0.5f, -0.5f, 0.5f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } }, // Bottom Right, Green
        { { -0.5f, -0.5f, 0.5f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } }  // Bottom Left, Blue
    };
    
    // 5. 创建顶点缓冲区
    BufferDesc vDesc{};
    vDesc.size = sizeof(vertices);
    vDesc.type = BufferType::Vertex;
    // 使用 Dynamic 模式以支持 Map/Unmap 操作
    vDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    vDesc.usage = GPUMemoryUsage::Dynamic;
    vDesc.name = "TestVertexBuffer";
    g_Ctx.vertexBuffer = g_Ctx.device->CreateBuffer(vDesc);
    if (g_Ctx.vertexBuffer != handles::INVALID_RESOURCE) {
        auto* buffer = g_Ctx.device->GetBuffer(g_Ctx.vertexBuffer);
        if (buffer) {
            void* data = buffer->Map();
            if (data) {
                memcpy(data, vertices, sizeof(vertices));
                buffer->Unmap();
            } else {
                std::cerr << "Failed to map vertex buffer!" << std::endl;
            }
        }
    } else {
        std::cerr << "Failed to create vertex buffer!" << std::endl;
    }

    // 6. 初始化多线程命令生成器
    g_Ctx.cmdGenerator = std::make_unique<RHIMultiThreadedCommandGenerator>(*g_Ctx.device);
    g_Ctx.cmdGenerator->Initialize();
    g_Ctx.cmdGenerator->SetCommandGenerationCallback(GenerateCommandsCallback);

    g_Ctx.startTime = std::chrono::high_resolution_clock::now();
    g_Ctx.lastTime = g_Ctx.startTime;
    g_Ctx.initialized = true;
    std::cout << "[MetalTest] Initialization successful!" << std::endl;
    return true;
}

void Engine_Test::run()
{
    if (!g_Ctx.initialized || g_Ctx.window.is_closed()) {
        shutdown();
        return;
    }

    // 1. 更新性能统计
    g_Ctx.frameCount++;
    auto currentTime = std::chrono::high_resolution_clock::now();
    float deltaTime = std::chrono::duration<float>(currentTime - g_Ctx.lastTime).count();
    if (deltaTime >= 1.0f) {
        g_Ctx.fps = static_cast<float>(g_Ctx.frameCount) / deltaTime;
        std::cout << "[MetalTest] FPS: " << g_Ctx.fps << std::endl;
        g_Ctx.frameCount = 0;
        g_Ctx.lastTime = currentTime;
    }

    // 2. 准备帧
    g_Ctx.device->BeginFrame();
    
    // 3. 获取当前交换链图像
    uint32_t backBufferIndex;
    if (!g_Ctx.swapChain->AcquireNextImage(&backBufferIndex, handles::INVALID_SYNC, handles::INVALID_SYNC)) {
        return;
    }
    ResourceHandle backBuffer = g_Ctx.swapChain->GetBackBuffer(backBufferIndex);

    // 4. 构建渲染场景
    RenderScene scene;
    scene.renderTarget = backBuffer;
    if (scene.renderTarget == handles::INVALID_RESOURCE) {
        std::cerr << "Invalid render target!" << std::endl;
        g_Ctx.device->EndFrame();
        return;
    }
    scene.totalDrawCalls = 1; // 演示用，1个DrawCall
    
    // 5. 生成命令
    std::vector<CommandBufferHandle> cmdBuffers;
    
    // --- 使用新的 Metal 并行渲染编码器 ---
    CommandBufferHandle mainCmdHandle = g_Ctx.device->CreateCommandBuffer(CommandQueueType::Graphics);
    if (mainCmdHandle != handles::INVALID_COMMAND_BUFFER) {
        MetalCommandBuffer* mainCmd = static_cast<MetalCommandBuffer*>(g_Ctx.device->GetCommandBuffer(mainCmdHandle));
        if (mainCmd && mainCmd->Begin()) {
            // 准备 RenderPassDesc
            RenderPassDesc passDesc;
            passDesc.colorAttachments.resize(1);
            passDesc.colorAttachments[0].texture = scene.renderTarget;
            passDesc.colorAttachments[0].loadOp = LoadAction::Clear;
            passDesc.colorAttachments[0].storeOp = StoreAction::Store;
            passDesc.colorAttachments[0].clearValue.color = primal::math::v4{
                0.1f * (sin(g_Ctx.rotationAngle) + 1.0f),
                0.1f,
                0.1f,
                1.0f
            }; // 动态背景色证明在运行

            passDesc.viewport.size.x = static_cast<float>(g_Ctx.window.width());
            passDesc.viewport.size.y = static_cast<float>(g_Ctx.window.height());
            passDesc.viewport.minDepth = 0.0f;
            passDesc.viewport.maxDepth = 1.0f;
            
            passDesc.scissor.offset = {0, 0};
            passDesc.scissor.extent = {g_Ctx.window.width(), g_Ctx.window.height()};

            // 创建 RenderPass 对象
            RenderPassHandle renderPassHandle = g_Ctx.device->CreateRenderPass(passDesc);

            // 开始并行 RenderPass (使用 Handle)
            mainCmd->BeginParallelRenderPass(renderPassHandle);
            
            // 启动并行线程
            const int numThreads = 4;
            std::vector<std::thread> threads;
            
            for (int i = 0; i < numThreads; ++i) {
                threads.emplace_back([mainCmd, passDesc]() {
                    // 创建子命令缓冲区
                    MetalCommandBuffer* subCmd = mainCmd->CreateSecondaryCommandBuffer();
                    if (subCmd) {
                        // 子编码器需要设置状态
                        subCmd->SetViewport(passDesc.viewport);
                        subCmd->SetScissor(passDesc.scissor);
                        subCmd->BindGraphicsPipeline(g_Ctx.pipeline);
                        
                        uint64_t vOffset = 0;
                        subCmd->BindVertexBuffers(2, 1, &g_Ctx.vertexBuffer, &vOffset);
                        
                        // 简单的偏移绘制，虽然顶点是固定的，但我们多次绘制
                        subCmd->Draw(3, 0, 1, 0);
                        
                        subCmd->EndRenderPass(); // 结束子编码器
                        delete subCmd; // 销毁临时对象
                    }
                });
            }
            
            for (auto& t : threads) {
                t.join();
            }
            
            // 结束并行 Pass
            mainCmd->EndRenderPass();
            mainCmd->End();
            
            cmdBuffers.push_back(mainCmdHandle);
        }
    }
    
    // 原有逻辑注释掉
    // g_Ctx.cmdGenerator->GenerateCommandsParallel(scene, cmdBuffers);
    
    if (cmdBuffers.empty()) {
        std::cerr << "Failed to generate command buffers!" << std::endl;
    }
    
    // 6. 提交命令
    for (auto cmd : cmdBuffers) {
        if (cmd != handles::INVALID_COMMAND_BUFFER) {
            auto* buffer = g_Ctx.device->GetCommandBuffer(cmd);
            if (buffer) buffer->Submit();
        }
    }

    // 7. 呈现
    g_Ctx.swapChain->Present(true);
    
    // 8. 结束帧
    g_Ctx.device->EndFrame();
}

void Engine_Test::shutdown()
{
    std::cout << "[MetalTest] Shutting down..." << std::endl;

    if (g_Ctx.cmdGenerator) g_Ctx.cmdGenerator->Shutdown();
    if (g_Ctx.swapChain) g_Ctx.swapChain->Destroy();
    if (g_Ctx.device) g_Ctx.device->Shutdown();
    if (g_Ctx.window.is_valid()) primal::platform::remove_window(g_Ctx.window.get_id());
    
    g_Ctx.initialized = false;
}

#endif // __APPLE__
