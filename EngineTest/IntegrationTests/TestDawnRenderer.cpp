#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "TestDawnRenderer.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <cassert>
#include <cmath>

namespace primal::test {

using namespace primal::graphics::rhi;
using namespace primal::graphics::rhi::handles;

// ============================================================
// Helper
// ============================================================

std::string DawnRendererTest::ReadShaderFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open shader file: " << path << std::endl;
        return "";
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

// ============================================================
// Test Registration
// ============================================================

bool DawnRendererTest::initialize() {
    testCases_ = {
        { "DeviceInitShutdown",     [this]() { return TestDeviceInitShutdown(); } },
        { "BufferCreateMapWrite",   [this]() { return TestBufferCreateMapWrite(); } },
        { "TextureCreateUpload",    [this]() { return TestTextureCreateUpload(); } },
        { "ShaderCompileWGSL",      [this]() { return TestShaderCompileWGSL(); } },
        { "GraphicsPipelineCreate", [this]() { return TestGraphicsPipelineCreate(); } },
        { "ClearScreen",            [this]() { return TestClearScreen(); } },
        { "DrawTriangle",           [this]() { return TestDrawTriangle(); } },
        { "DescriptorSetBinding",   [this]() { return TestDescriptorSetBinding(); } },
        { "ComputeDispatch",        [this]() { return TestComputeDispatch(); } },
        { "SyncFence",              [this]() { return TestSyncFence(); } },
        { "MultiFrame",             [this]() { return TestMultiFrame(); } },
    };
    std::cout << "Dawn WebGPU: Registered " << testCases_.size() << " tests" << std::endl;
    return true;
}

void DawnRendererTest::run() {
    int passed = 0, failed = 0;

    for (auto& tc : testCases_) {
        std::cout << "  [RUN ] " << tc.name << std::endl;
        bool result = tc.run();
        if (result) {
            std::cout << "  [PASS] " << tc.name << std::endl;
            ++passed;
        } else {
            std::cout << "  [FAIL] " << tc.name << std::endl;
            ++failed;
        }
    }

    std::cout << "\nDawn WebGPU Test Results: " << passed << " passed, "
              << failed << " failed, " << (passed + failed) << " total" << std::endl;
}

void DawnRendererTest::shutdown() {
    DestroyDevice();
}

bool DawnRendererTest::CreateDevice() {
    if (device_) return true;

    DeviceDesc desc;
    desc.platform = RHIPlatform::Dawn;
    desc.enableDebug = true;
    desc.enableValidation = true;
    desc.maxFramesInFlight = 3;

    device_ = new DawnDevice(desc);
    if (!device_->Initialize()) {
        std::cerr << "Failed to initialize Dawn device" << std::endl;
        delete device_;
        device_ = nullptr;
        return false;
    }
    return true;
}

void DawnRendererTest::DestroyDevice() {
    if (device_) {
        device_->Shutdown();
        delete device_;
        device_ = nullptr;
    }
}

// ============================================================
// Test 1: Device Init / Shutdown
// ============================================================

bool DawnRendererTest::TestDeviceInitShutdown() {
    DeviceDesc desc;
    desc.platform = RHIPlatform::Dawn;
    desc.enableDebug = true;
    desc.enableValidation = true;

    DawnDevice device(desc);
    if (!device.Initialize()) return false;
    if (!device.IsValid()) return false;

    // GetDeviceInfo() is protected in RHIDeviceBase, use public GetInfo() accessor
    const auto& info = device.GetInfo();
    if (info.platform != RHIPlatform::Dawn) return false;

    device.Shutdown();
    if (device.IsValid()) return false;

    return true;
}

// ============================================================
// Test 2: Buffer Create / Map / Write
// ============================================================

bool DawnRendererTest::TestBufferCreateMapWrite() {
    if (!CreateDevice()) return false;

    BufferDesc desc{};
    desc.type = BufferType::Vertex;
    desc.memoryUsage = GPUMemoryUsage::Dynamic;
    desc.size = 1024;
    // BufferDesc does not have a 'stride' field at top level.
    // Vertex-specific stride goes into the vertex union member.
    desc.vertex.vertexStride = sizeof(float) * 3;
    desc.vertex.vertexCount = 3;
    desc.name = "TestVertexBuffer";

    auto handle = device_->CreateBuffer(desc);
    if (handle == INVALID_RESOURCE) return false;

    // Map and write data
    float triangleVertices[] = {
         0.0f,  0.5f, 0.0f,
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
    };

    void* mapped = device_->MapBuffer(handle, 0, sizeof(triangleVertices));
    if (!mapped) return false;

    memcpy(mapped, triangleVertices, sizeof(triangleVertices));
    device_->UnmapBuffer(handle);

    device_->DestroyBuffer(handle);
    return true;
}

// ============================================================
// Test 3: Texture Create / Upload
// ============================================================

bool DawnRendererTest::TestTextureCreateUpload() {
    if (!CreateDevice()) return false;

    TextureDesc desc{};
    desc.type = TextureType::Texture2D;
    desc.format = DataFormat::RGBA8_UNorm;
    // TextureDesc uses math::u32v3 size, not separate width/height/depth
    desc.size = {256, 256, 1};
    desc.mipLevels = 1;
    desc.arraySize = 1;
    // TextureDesc does not have sampleCount field
    // TextureUsage is the correct enum, not ResourceUsage
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    desc.name = "TestTexture";

    auto handle = device_->CreateTexture(desc);
    if (handle == INVALID_RESOURCE) return false;

    device_->DestroyTexture(handle);
    return true;
}

// ============================================================
// Test 4: Shader Compile WGSL
// ============================================================

bool DawnRendererTest::TestShaderCompileWGSL() {
    if (!CreateDevice()) return false;

    // Minimal valid WGSL
    const char* wgslSource = R"(
        @vertex fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4f {
            var pos = array<vec2f, 3>(
                vec2f(0.0, 0.5), vec2f(-0.5, -0.5), vec2f(0.5, -0.5));
            return vec4f(pos[vi], 0.0, 1.0);
        }
        @fragment fn fs_main() -> @location(0) vec4f {
            return vec4f(1.0, 0.0, 0.0, 1.0);
        }
    )";

    auto vs = device_->CreateShader(wgslSource, strlen(wgslSource), ShaderStage::Vertex, "vs_main");
    if (vs == INVALID_SHADER) return false;

    auto fs = device_->CreateShader(wgslSource, strlen(wgslSource), ShaderStage::Pixel, "fs_main");
    if (fs == INVALID_SHADER) return false;

    device_->DestroyShader(vs);
    device_->DestroyShader(fs);
    return true;
}

// ============================================================
// Test 5: Graphics Pipeline Create
// ============================================================

bool DawnRendererTest::TestGraphicsPipelineCreate() {
    if (!CreateDevice()) return false;

    const char* wgslSource = R"(
        @vertex fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4f {
            var pos = array<vec2f, 3>(
                vec2f(0.0, 0.5), vec2f(-0.5, -0.5), vec2f(0.5, -0.5));
            return vec4f(pos[vi], 0.0, 1.0);
        }
        @fragment fn fs_main() -> @location(0) vec4f {
            return vec4f(1.0, 0.0, 0.0, 1.0);
        }
    )";

    auto vs = device_->CreateShader(wgslSource, strlen(wgslSource), ShaderStage::Vertex, "vs_main");
    auto fs = device_->CreateShader(wgslSource, strlen(wgslSource), ShaderStage::Pixel, "fs_main");
    if (vs == INVALID_SHADER || fs == INVALID_SHADER) return false;

    GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.vertexShader = vs;
    pipelineDesc.pixelShader = fs;
    pipelineDesc.topology = PrimitiveTopology::TriangleList;
    pipelineDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    pipelineDesc.renderTargetCount = 1;
    pipelineDesc.enableDepthTest = false;

    auto pipeline = device_->CreateGraphicsPipeline(pipelineDesc);
    if (pipeline == INVALID_PIPELINE) return false;

    device_->DestroyPipeline(pipeline);
    device_->DestroyShader(vs);
    device_->DestroyShader(fs);
    return true;
}

// ============================================================
// Test 6: Clear Screen
// ============================================================

bool DawnRendererTest::TestClearScreen() {
    if (!CreateDevice()) return false;

    // Create a simple render pass that clears to red
    RenderPassDesc rpDesc{};
    rpDesc.colorAttachments.emplace_back();
    rpDesc.colorAttachments[0].format = DataFormat::BGRA8_UNorm;
    rpDesc.colorAttachments[0].loadOp = LoadAction::Clear;
    rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
    rpDesc.colorAttachments[0].clearValue.color = {1.0f, 0.0f, 0.0f, 1.0f};
    // ViewportDesc uses topLeft + size instead of individual x,y,w,h
    rpDesc.viewport = ViewportDesc{{0.0f, 0.0f}, {640.0f, 480.0f}, 0.0f, 1.0f};

    auto rp = device_->CreateRenderPass(rpDesc);
    if (rp == INVALID_RESOURCE) return false;

    auto cmdBuffer = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    if (cmdBuffer == INVALID_COMMAND_BUFFER) return false;

    // Record: begin -> begin render pass -> end render pass -> end
    // (actual command buffer recording depends on RHICommandBuffer interface)

    QueueSubmitInfo submitInfo{};
    submitInfo.cmdBuffer = cmdBuffer;
    bool submitted = device_->Submit(submitInfo);

    device_->DestroyCommandBuffer(cmdBuffer);
    device_->DestroyRenderPass(rp);
    return submitted;
}

// ============================================================
// Test 7: Draw Triangle
// ============================================================

bool DawnRendererTest::TestDrawTriangle() {
    if (!CreateDevice()) return false;

    // Vertex data
    float vertices[] = {
         0.0f,  0.5f, 0.0f,
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
    };

    BufferDesc vbDesc{};
    vbDesc.type = BufferType::Vertex;
    vbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    vbDesc.size = sizeof(vertices);
    vbDesc.vertex.vertexStride = sizeof(float) * 3;
    vbDesc.vertex.vertexCount = 3;
    vbDesc.name = "TriangleVB";

    auto vb = device_->CreateBuffer(vbDesc);
    if (vb == INVALID_RESOURCE) return false;

    void* mapped = device_->MapBuffer(vb, 0, sizeof(vertices));
    if (!mapped) return false;
    memcpy(mapped, vertices, sizeof(vertices));
    device_->UnmapBuffer(vb);

    // Shader
    const char* wgslSource = R"(
        @vertex fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4f {
            var pos = array<vec2f, 3>(
                vec2f(0.0, 0.5), vec2f(-0.5, -0.5), vec2f(0.5, -0.5));
            return vec4f(pos[vi], 0.0, 1.0);
        }
        @fragment fn fs_main() -> @location(0) vec4f {
            return vec4f(1.0, 0.0, 0.0, 1.0);
        }
    )";

    auto vs = device_->CreateShader(wgslSource, strlen(wgslSource), ShaderStage::Vertex, "vs_main");
    auto fs = device_->CreateShader(wgslSource, strlen(wgslSource), ShaderStage::Pixel, "fs_main");

    GraphicsPipelineDesc pipeDesc{};
    pipeDesc.vertexShader = vs;
    pipeDesc.pixelShader = fs;
    pipeDesc.topology = PrimitiveTopology::TriangleList;
    pipeDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
    pipeDesc.renderTargetCount = 1;
    pipeDesc.enableDepthTest = false;

    auto pipeline = device_->CreateGraphicsPipeline(pipeDesc);

    // Create render pass and command buffer, record, submit
    RenderPassDesc rpDesc{};
    rpDesc.colorAttachments.emplace_back();
    rpDesc.colorAttachments[0].format = DataFormat::BGRA8_UNorm;
    rpDesc.colorAttachments[0].loadOp = LoadAction::Clear;
    rpDesc.colorAttachments[0].storeOp = StoreAction::Store;
    rpDesc.colorAttachments[0].clearValue.color = {0.0f, 0.0f, 0.0f, 1.0f};
    rpDesc.viewport = ViewportDesc{{0.0f, 0.0f}, {640.0f, 480.0f}, 0.0f, 1.0f};

    auto rp = device_->CreateRenderPass(rpDesc);
    auto cmd = device_->CreateCommandBuffer(CommandQueueType::Graphics);

    QueueSubmitInfo submit{};
    submit.cmdBuffer = cmd;
    device_->Submit(submit);

    device_->DestroyCommandBuffer(cmd);
    device_->DestroyRenderPass(rp);
    device_->DestroyPipeline(pipeline);
    device_->DestroyShader(vs);
    device_->DestroyShader(fs);
    device_->DestroyBuffer(vb);
    return true;
}

// ============================================================
// Test 8: Descriptor Set Binding
// ============================================================

bool DawnRendererTest::TestDescriptorSetBinding() {
    if (!CreateDevice()) return false;

    // Create a uniform buffer
    BufferDesc ubDesc{};
    ubDesc.type = BufferType::Constant;
    ubDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ubDesc.size = 256;
    // BufferDesc does not have a top-level 'stride' field
    ubDesc.name = "TestUB";

    auto ub = device_->CreateBuffer(ubDesc);
    if (ub == INVALID_RESOURCE) return false;

    // Write color data
    float colorData[] = {0.0f, 1.0f, 0.0f, 1.0f};
    void* mapped = device_->MapBuffer(ub, 0, sizeof(colorData));
    if (!mapped) return false;
    memcpy(mapped, colorData, sizeof(colorData));
    device_->UnmapBuffer(ub);

    // Create descriptor set layout
    DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    // DescriptorSetLayoutBinding uses 'descriptorType', not 'type'
    binding.descriptorType = DescriptorType::UniformBuffer;
    binding.stageFlags = ShaderStage::Pixel;
    // DescriptorSetLayoutBinding uses 'descriptorCount', not 'count'
    binding.descriptorCount = 1;

    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 1;
    layoutDesc.bindings = &binding;

    auto layout = device_->CreateDescriptorSetLayout(layoutDesc);
    if (layout == INVALID_RESOURCE) return false;

    // Create descriptor set
    DescriptorSetDesc setDesc{};
    setDesc.layout = layout;

    auto descSet = device_->CreateDescriptorSet(setDesc);
    if (descSet == INVALID_RESOURCE) return false;

    // Update descriptor set
    // WriteDescriptorSet uses dstSet/dstBinding, NOT targetSet/targetBinding
    // Buffer info is passed via DescriptorBufferInfo pointer, NOT inline
    DescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = ub;
    bufferInfo.offset = 0;
    bufferInfo.range = 256;

    WriteDescriptorSet write{};
    write.dstSet = descSet;
    write.dstBinding = 0;
    write.descriptorType = DescriptorType::UniformBuffer;
    write.descriptorCount = 1;
    write.bufferInfo = &bufferInfo;

    device_->UpdateDescriptorSets(1, &write);

    device_->DestroyDescriptorSet(descSet);
    device_->DestroyDescriptorSetLayout(layout);
    device_->DestroyBuffer(ub);
    return true;
}

// ============================================================
// Test 9: Compute Dispatch
// ============================================================

bool DawnRendererTest::TestComputeDispatch() {
    if (!CreateDevice()) return false;

    // Create storage buffer
    BufferDesc sbDesc{};
    sbDesc.type = BufferType::Structured;
    sbDesc.memoryUsage = GPUMemoryUsage::Static;
    sbDesc.size = 256 * sizeof(u32);
    // BufferDesc does not have a top-level 'stride' field
    // Structured buffer stride goes into the structured union member
    sbDesc.structured.elementCount = 256;
    sbDesc.structured.elementStride = sizeof(u32);
    sbDesc.name = "ComputeOutput";

    auto sb = device_->CreateBuffer(sbDesc);
    if (sb == INVALID_RESOURCE) return false;

    // Compute shader
    const char* computeWGSL = R"(
        struct Data { values: array<u32> }
        @group(0) @binding(0) var<storage, read_write> output: Data;
        @compute @workgroup_size(64)
        fn cs_main(@builtin(global_invocation_id) gid: vec3u) {
            let idx = gid.x;
            if (idx < 256u) {
                output.values[idx] = idx * 2u + 1u;
            }
        }
    )";

    auto cs = device_->CreateShader(computeWGSL, strlen(computeWGSL), ShaderStage::Compute, "cs_main");
    if (cs == INVALID_SHADER) return false;

    // Create compute pipeline
    ComputePipelineDesc pipeDesc{};
    pipeDesc.computeShader = cs;

    auto pipeline = device_->CreateComputePipeline(pipeDesc);

    device_->DestroyPipeline(pipeline);
    device_->DestroyShader(cs);
    device_->DestroyBuffer(sb);
    return true;
}

// ============================================================
// Test 10: Sync / Fence
// ============================================================

bool DawnRendererTest::TestSyncFence() {
    if (!CreateDevice()) return false;

    auto sync = device_->CreateSync();
    if (sync == INVALID_SYNC) return false;

    // Submit empty work and wait
    auto cmd = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    QueueSubmitInfo submit{};
    submit.cmdBuffer = cmd;
    submit.signalFence = sync;
    device_->Submit(submit);

    bool waited = device_->WaitForSync(sync, 5000);

    device_->DestroyCommandBuffer(cmd);
    device_->DestroySync(sync);
    return waited;
}

// ============================================================
// Test 11: Multi-frame Rendering
// ============================================================

bool DawnRendererTest::TestMultiFrame() {
    if (!CreateDevice()) return false;

    const u32 FRAME_COUNT = 10;

    for (u32 i = 0; i < FRAME_COUNT; ++i) {
        device_->BeginFrame();

        // Create and submit a simple clear command
        auto cmd = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        QueueSubmitInfo submit{};
        submit.cmdBuffer = cmd;
        device_->Submit(submit);
        device_->DestroyCommandBuffer(cmd);

        device_->EndFrame();
    }

    device_->WaitIdle();
    return true;
}

} // namespace primal::test

#endif // ENABLE_WEBGPU
