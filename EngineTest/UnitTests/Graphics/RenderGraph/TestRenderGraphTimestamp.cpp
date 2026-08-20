
// stale-test port: Metal headers must come first — RenderGraphDefinitions.h opens a
// global `using namespace rhi`, which clashes with ::Rect from MacTypes.h (pulled in
// by MetalCommon.h) if RenderGraph.h is included before the Metal chain.
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "../../TestFramework.h"
#include <thread>
#include <chrono>

using namespace primal::graphics::rendergraph;
using namespace primal::graphics::rhi;
using namespace Engine::Test;

// 辅助函数：创建 Metal 设备
static MetalDevice* CreateTestDevice() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Metal;
    desc.enableDebug = true;
    
    MetalDevice* device = new MetalDevice(desc);
    if (!device->Initialize()) {
        delete device;
        return nullptr;
    }
    return device;
}

TestResult TestRenderGraphTimestampIntegration() {
    MetalDevice* device = CreateTestDevice();
    if (!device) {
        printf("Skipping test: Metal device creation failed (not on macOS?)\n");
        return TestResult::Skipped;
    }

    CommandBufferHandle cmdBufHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
    RHICommandBuffer* cmdBuffer = device->GetCommandBuffer(cmdBufHandle);

    printf("Starting Frame Loop (5 frames)...\n");

    bool dataReceived = false;

    {
        RenderGraph graph(*device);

        for (int frame = 0; frame < 5; ++frame) {
            printf("Frame %d Begin\n", frame);
            
            // 1. Clear Graph (Recycles resources, resolves timestamps from prev frames)
            graph.Clear();
            
            // Check results from previous frames
            const auto& times = graph.GetPassExecutionTimes();
            if (!times.empty()) {
                dataReceived = true;
                printf("  [Frame %d] Received Timestamp Data:\n", frame);
                for (const auto& [name, time] : times) {
                    printf("    Pass '%s': %.4f ms\n", name.c_str(), time);
                    // On Apple Silicon, timestamp is 1ns.
                    // We expect some duration.
                    TEST_ASSERT(time >= 0.0, "Pass time should be non-negative");
                }
            }

            // 2. Setup Graph
            struct PassData {
                RGResourceHandle output;
            };

            graph.AddPass<PassData>("TestPass", RGPassType::Graphics, RGPassCategory::Main,
                [&](PassData& data, RenderGraphBuilder& builder) {
                    TextureDesc desc;
                    desc.size = {512, 512, 1};
                    desc.format = DataFormat::RGBA8_UNorm;
                    desc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
                    desc.memoryUsage = GPUMemoryUsage::Dynamic;

                    // Create texture and declare it as output
                    data.output = builder.CreateTexture("OutputTex", desc);
                    
                    // Setup Render Pass Descriptor
                    RGRenderPassDesc rgDesc;
                    RGAttachmentDesc colorAtt;
                    colorAtt.texture = data.output;
                    colorAtt.loadOp = LoadAction::Clear;
                    colorAtt.storeOp = StoreAction::Store;
                    colorAtt.clearColor = ClearValue{primal::math::v4{0.0f, 1.0f, 0.0f, 1.0f}};
                    rgDesc.colors.push_back(colorAtt);
                    
                    builder.DeclareRenderPass(rgDesc);
                    builder.SideEffect(); // Prevent culling
                },
                [&](const PassData& data, RenderGraphContext& ctx) {
                    // Do some work
                    // Since RenderGraph handles Begin/End RenderPass, we don't need to do anything here for this test.
                    // The Clear op will take some time.
                }
            );

            // 3. Compile
            graph.Compile();
            
            // 4. Execute
            if (!cmdBuffer->Begin()) {
                std::cerr << "Frame " << frame << ": Failed to begin command buffer. State: " << (int)cmdBuffer->GetState() << std::endl;
                return TestResult::Failed;
            }
            graph.Execute(cmdBuffer);
            cmdBuffer->End();
            
            // 5. Submit
            QueueSubmitInfo submitInfo;
            submitInfo.cmdBuffer = cmdBufHandle;
            device->Submit(submitInfo);
            
            // Wait for completion to ensure timestamps are written and update command buffer state
            cmdBuffer->WaitForCompletion();
            
            // Simulate frame time
            std::this_thread::sleep_for(std::chrono::milliseconds(16));

            // Reset command buffer for next frame
            if (!cmdBuffer->Reset()) {
                std::cerr << "Frame " << frame << ": Failed to reset command buffer. State: " << (int)cmdBuffer->GetState() << std::endl;
                return TestResult::Failed;
            }
        }
    } // RenderGraph destroyed here

    TEST_ASSERT(dataReceived, "Should have received timestamp data after a few frames");

    // Cleanup
    device->DestroyCommandBuffer(cmdBufHandle);
    device->Shutdown();
    delete device;

    return TestResult::Passed;
}

int main() {
    return TestRenderGraphTimestampIntegration() == TestResult::Passed ? 0 : 1;
}
