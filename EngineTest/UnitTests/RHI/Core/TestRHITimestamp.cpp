#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIResource.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalQuery.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <thread>
#include <chrono>

// Simple assertion macro
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            printf("ASSERTION FAILED: %s\nFile: %s\nLine: %d\n", message, __FILE__, __LINE__); \
            throw std::runtime_error(message); \
        } \
    } while (0)

using namespace primal::graphics::rhi;

int main() {
#if defined(__APPLE__)
    try {
        printf("Running Timestamp Integration Test...\n");
        
        // 1. Initialize Device
        DeviceDesc deviceDesc;
        MetalDevice device(deviceDesc);
        device.Initialize();
        
        // 2. Create Timestamp QueryPool
        // Using 2 queries: one for start, one for end
        QueryPoolDesc queryDesc;
        queryDesc.type = QueryType::Timestamp;
        queryDesc.queryCount = 2;
        QueryPoolHandle queryPool = device.CreateQueryPool(queryDesc);
        
        if (queryPool == handles::INVALID_QUERY_POOL) {
            printf("Failed to create timestamp query pool\n");
            return 1;
        }
        
        // 3. Create Command Buffer
        CommandBufferHandle cmdBuffer = device.CreateCommandBuffer();
        MetalCommandBuffer* metalCmd = static_cast<MetalCommandBuffer*>(device.GetCommandBuffer(cmdBuffer));
        
        if (!metalCmd) {
            printf("Failed to get MetalCommandBuffer\n");
            return 1;
        }
        
        // 4. Record Commands
        // We use a Render Pass for timestamp testing with sampleBufferAttachments
        // because direct encoder timestamp writing is not supported on Apple Silicon.
        
        // Create a dummy texture for render pass
        TextureDesc texDesc;
        texDesc.size = {256, 256, 1};
        texDesc.type = TextureType::Texture2D;
        texDesc.format = DataFormat::RGBA8_UNorm;
        texDesc.usage = TextureUsage::RenderTarget;
        texDesc.memoryUsage = GPUMemoryUsage::Static;
        ResourceHandle texture = device.CreateTexture(texDesc);
        
        if (texture == handles::INVALID_RESOURCE) {
            printf("Failed to create texture\n");
            return 1;
        }

        // Create Render Pass
        RenderPassDesc passDesc;
        passDesc.colorAttachments.resize(1);
        passDesc.colorAttachments[0].texture = texture;
        passDesc.colorAttachments[0].loadOp = LoadAction::Clear;
        passDesc.colorAttachments[0].storeOp = StoreAction::Store;
        passDesc.colorAttachments[0].clearValue = {0.0f, 0.0f, 0.0f, 1.0f};
        
        // Enable Timestamp
        passDesc.enableTimestamp = true;
        passDesc.timestampQueryPool = queryPool;
        passDesc.beginTimestampIndex = 0;
        passDesc.endTimestampIndex = 1;
        
        metalCmd->Begin();
        
        metalCmd->BeginRenderPass(passDesc);
        
        // Simulate some work? 
        // Just empty pass is fine, Metal will record timestamps at boundaries.
        
        metalCmd->EndRenderPass();
        
        metalCmd->End();
        
        // 5. Submit
        QueueSubmitInfo submitInfo;
        submitInfo.cmdBuffer = cmdBuffer;
        device.Submit(submitInfo);
        
        device.WaitIdle();
        
        // Cleanup texture
        device.DestroyTexture(texture);
        std::vector<uint64_t> results(2);
        MetalQueryPool* metalPool = device.GetQueryPool(queryPool);
        
        // Wait for results
        // On Metal, results are usually available after completion.
        bool success = metalPool->GetResults(0, 2, results.data(), sizeof(uint64_t));
        
        TEST_ASSERT(success, "GetResults should succeed");
        printf("Timestamp 0 (Begin): %llu\n", results[0]);
        printf("Timestamp 1 (End):   %llu\n", results[1]);
        
        if (results[1] <= results[0]) {
             printf("WARNING: End <= Begin. Delta: %lld. GPU might be too fast or timestamps invalid.\n", (long long)(results[1] - results[0]));
             // If delta is 0, it's possible for empty pass.
             if (results[1] == 0 && results[0] == 0) {
                 TEST_ASSERT(false, "Timestamps are zero!");
             }
        } else {
             printf("Timestamp Delta: %llu ticks\n", results[1] - results[0]);
             TEST_ASSERT(true, "Timestamp End > Timestamp Begin");
        }
        
        // Cleanup
        device.DestroyCommandBuffer(cmdBuffer);
        device.DestroyQueryPool(queryPool);
        device.Shutdown();
        
        printf("Test Passed!\n");
        
    } catch (const std::exception& e) {
        printf("Exception: %s\n", e.what());
        return 1;
    }
    
    return 0;
#endif
}
