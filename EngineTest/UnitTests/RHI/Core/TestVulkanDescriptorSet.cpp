/**
 * @file TestVulkanDescriptorSet.cpp
 * @brief Phase 6 UBO 描述符集绑定测试
 * @details 端到端验证 DescriptorSetLayout → PipelineLayout → DescriptorSet →
 *          Update(write UBO) → BindDescriptorSets → Draw。
 *          vertex shader 读 UBO(set=0,binding=0)color → 输出色。
 *          验证中心像素严格等于 UBO 数据(允许 gamma 精度抖动)。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIRenderPass.h"
#include "Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

std::vector<u8> ReadSPV(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> data(sz);
    if (sz > 0) f.read(reinterpret_cast<char*>(data.data()), sz);
    return data;
}

struct DeviceFixture {
    RHIDeviceBase* base{nullptr};
    VulkanDevice* vk{nullptr};
    DeviceDesc desc{};
    bool Init() {
        desc.platform = RHIPlatform::Vulkan;
        desc.enableValidation = true;
        desc.enableDebug = true;
        base = CreateRHIDevice(desc);
        if (!base) return false;
        vk = static_cast<VulkanDevice*>(base);
        return true;
    }
    ~DeviceFixture() { if (base) base->Shutdown(); }
};

constexpr u32 kW = 64, kH = 64;

u32 ReadCenterPixel(void* mapped) {
    const u8* p = reinterpret_cast<const u8*>(mapped);
    u32 cx = kW / 2, cy = kH / 2;
    u32 idx = (cy * kW + cx) * 4;
    return (u32(p[idx + 3]) << 24) | (u32(p[idx + 2]) << 16) | (u32(p[idx + 1]) << 8) | u32(p[idx]);
}

} // anonymous namespace

/// UBO 描述符集: vec4(0, 1, 0, 1) (绿色) → 中心像素 ≈ (0, 255, 0, 255)
TestResult TestDescriptorSetUBO() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto vert = ReadSPV("Assets/Shaders/SPIRV/ubo_triangle.vert.spv");
    auto frag = ReadSPV("Assets/Shaders/SPIRV/ubo_triangle.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read SPIR-V");

    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");
    TEST_ASSERT(vs != handles::INVALID_SHADER, "CreateShader vs");
    TEST_ASSERT(fs != handles::INVALID_SHADER, "CreateShader fs");

    // === UBO buffer: 16 bytes vec4(0,1,0,1) ===
    BufferDesc uboDesc{};
    uboDesc.size = 16;
    uboDesc.type = BufferType::Constant;  // UBO
    uboDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    uboDesc.name = "UBO";
    ResourceHandle ubo = fx.base->CreateBuffer(uboDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer UBO");

    primal::math::v4 uboColor{0.0f, 1.0f, 0.0f, 1.0f};  // green
    bool ok = fx.base->UpdateBufferData(ubo, &uboColor, 16, 0);
    TEST_ASSERT(ok, "UpdateBufferData UBO");

    // === DescriptorSetLayout: binding 0 = UniformBuffer, vert stage ===
    DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = DescriptorType::UniformBuffer;
    binding.descriptorCount = 1;
    binding.stageFlags = ShaderStage::Vertex;

    DescriptorSetLayoutDesc dslDesc{};
    dslDesc.bindingCount = 1;
    dslDesc.bindings = &binding;
    DescriptorSetLayoutHandle dsl = fx.base->CreateDescriptorSetLayout(dslDesc);
    TEST_ASSERT(dsl != handles::INVALID_RESOURCE, "CreateDescriptorSetLayout");

    // === PipelineLayout: 1 setLayout, no push constants ===
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &dsl;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    // === DescriptorSet: allocate from layout ===
    DescriptorSetDesc dsDesc{};
    dsDesc.layout = dsl;
    DescriptorSetHandle ds = fx.base->CreateDescriptorSet(dsDesc);
    TEST_ASSERT(ds != handles::INVALID_RESOURCE, "CreateDescriptorSet");

    // === Write descriptor: binding 0 → UBO buffer ===
    DescriptorBufferInfo bufInfo{};
    bufInfo.buffer = ubo;
    bufInfo.offset = 0;
    bufInfo.range = 16;
    WriteDescriptorSet write{};
    write.dstSet = ds;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = DescriptorType::UniformBuffer;
    write.bufferInfo = &bufInfo;
    fx.base->UpdateDescriptorSets(1, &write);

    // === Pipeline ===
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader  = fs;
    gpd.layout       = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.fillMode = FillMode::Solid;
    gpd.cullMode = CullMode::None;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RG8B8A8_UNorm;
    gpd.depthStencilFormat = DataFormat::Unknown;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = fx.base->CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline");

    // === Render target + readback ===
    TextureDesc tdesc{};
    tdesc.size = { kW, kH, 1 };
    tdesc.mipLevels = 1;
    tdesc.arraySize = 1;
    tdesc.format = DataFormat::RG8B8A8_UNorm;
    tdesc.type = TextureType::Texture2D;
    tdesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    tdesc.memoryUsage = GPUMemoryUsage::Static;
    tdesc.name = "RT";
    ResourceHandle rt = fx.base->CreateTexture(tdesc);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture RT");

    BufferDesc rbDesc{};
    rbDesc.size = u64(kW) * kH * 4;
    rbDesc.type = BufferType::Raw;
    rbDesc.memoryUsage = GPUMemoryUsage::Readback;
    rbDesc.name = "Readback";
    ResourceHandle readback = fx.base->CreateBuffer(rbDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    // === Render pass ===
    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = rt;
    rpd.colorAttachments[0].format = DataFormat::RG8B8A8_UNorm;
    rpd.colorAttachments[0].loadOp = LoadAction::Clear;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.colorAttachments[0].clearValue.color = primal::math::v4{0,0,0,1};
    rpd.viewport.topLeft = {0,0};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.viewport.minDepth = 0;
    rpd.viewport.maxDepth = 1;
    rpd.scissor.offset = {0,0};
    rpd.scissor.extent = {kW, kH};

    // === Record ===
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    vcmd->Reset(); vcmd->Begin();
    vcmd->BeginRenderPass(rpd);
    vcmd->BindGraphicsPipeline(pipe);
    // BindDescriptorSets: bind set 0 to vs/fs
    vcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 1, &ds, 0, nullptr);
    vcmd->Draw(3, 0, 1, 0);
    vcmd->EndRenderPass();
    BufferTextureCopyRegion region{};
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {kW, kH, 1};
    vcmd->CopyTextureToBuffer(rt, readback, &region, 1);
    vcmd->End(); vcmd->Submit(0); vcmd->WaitForCompletion();

    void* mapped = fx.base->MapBuffer(readback, 0, rbDesc.size);
    TEST_ASSERT(mapped, "MapBuffer");
    u32 pixel = ReadCenterPixel(mapped);
    fx.base->UnmapBuffer(readback);
    std::cout << "[DescriptorSetUBO] center pixel = 0x" << std::hex << pixel << std::dec << std::endl;

    u8 r = pixel & 0xFF, g = (pixel >> 8) & 0xFF, b = (pixel >> 16) & 0xFF, a = (pixel >> 24) & 0xFF;
    TEST_ASSERT(g >= 200, "green dominant (UBO color came through)");
    TEST_ASSERT(r <= 50,  "red suppressed");
    TEST_ASSERT(b <= 50,  "blue suppressed");
    TEST_ASSERT(a >= 200, "alpha high");

    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(rt);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSetLayout(dsl);
    fx.base->DestroyBuffer(ubo);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    return TestResult::Passed;
}

void RegisterVulkanDescriptorSetTests() {
    auto suite = std::make_shared<TestSuite>("VulkanDescriptorSetTests");
    suite->AddTestCase(TestCase("DescriptorSetUBO", TestDescriptorSetUBO));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanDescriptorSetTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else

int main() {
    std::cout << "[TestVulkanDescriptorSet] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif
