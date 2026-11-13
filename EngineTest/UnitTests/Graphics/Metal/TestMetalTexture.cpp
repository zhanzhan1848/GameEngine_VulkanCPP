#include "../../TestFramework.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalTexture.h"

using namespace primal::graphics::rhi;
using namespace Engine::Test;

TestResult TestTextureCreation() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;
    
    MetalDevice device(deviceDesc);
    bool initResult = device.Initialize();
    if (!initResult) return TestResult::Skipped; // Skip if no Metal device
    
    // Test various formats
    std::vector<DataFormat> formats = {
        DataFormat::RGBA8_UNorm,
        DataFormat::BGRA8_UNorm,
        DataFormat::R8_UNorm,
        DataFormat::RGBA16_Float,
        DataFormat::RGBA32_Float,
        DataFormat::D32_Float
    };
    
    for (auto format : formats) {
        TextureDesc desc;
        desc.name = "TestTexture";
        desc.type = TextureType::Texture2D;
        desc.format = format;
        desc.size = {128, 128, 1};
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.usage = TextureUsage::ShaderResource;
        if (format == DataFormat::D32_Float) {
            desc.usage = TextureUsage::DepthStencil;
        }
        
        ResourceHandle handle = device.CreateTexture(desc);
        TEST_ASSERT(handle != handles::INVALID_RESOURCE, "Texture creation failed for format");
        
        MetalTexture* texture = device.GetTexture(handle);
        TEST_ASSERT(texture != nullptr, "GetTexture failed");
        TEST_ASSERT(texture->GetNativeTexture() != nullptr, "Native texture is null");
        
        device.DestroyTexture(handle);
    }
    
    device.Shutdown();
    return TestResult::Passed;
}

TestResult TestTextureUpdate() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    
    MetalDevice device(deviceDesc);
    if (!device.Initialize()) return TestResult::Skipped;
    
    TextureDesc desc;
    desc.name = "UpdateTestTexture";
    desc.type = TextureType::Texture2D;
    desc.format = DataFormat::RGBA8_UNorm;
    desc.size = {64, 64, 1};
    desc.usage = TextureUsage::ShaderResource;
    desc.memoryUsage = GPUMemoryUsage::Static;
    
    ResourceHandle handle = device.CreateTexture(desc);
    TEST_ASSERT(handle != handles::INVALID_RESOURCE, "CreateTexture failed");
    
    MetalTexture* texture = device.GetTexture(handle);
    
    // Create test data (Red)
    std::vector<uint32_t> data(64 * 64, 0xFF0000FF); // RGBA (LE) -> R=FF, G=00, B=00, A=FF
    
    bool updateResult = texture->UpdateData(data.data(), data.size() * sizeof(uint32_t), 0);
    TEST_ASSERT(updateResult, "UpdateData failed");
    
    // In a real rendering test we would verify the content, but for unit test 
    // we just ensure the API call succeeds and doesn't crash.
    
    device.DestroyTexture(handle);
    device.Shutdown();
    return TestResult::Passed;
}

TestResult TestTextureStagingUpdate() {
    // Test update using staging buffer (implicitly used for Private textures)
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    
    MetalDevice device(deviceDesc);
    if (!device.Initialize()) return TestResult::Skipped;
    
    TextureDesc desc;
    desc.name = "StagingUpdateTest";
    desc.type = TextureType::Texture2D;
    desc.format = DataFormat::RGBA8_UNorm;
    desc.size = {64, 64, 1};
    desc.usage = TextureUsage::ShaderResource;
    desc.memoryUsage = GPUMemoryUsage::Immutable; // Force Private + Staging
    
    ResourceHandle handle = device.CreateTexture(desc);
    TEST_ASSERT(handle != handles::INVALID_RESOURCE, "CreateTexture failed");
    
    MetalTexture* texture = device.GetTexture(handle);
    
    std::vector<uint32_t> data(64 * 64, 0xFFFFFFFF);
    
    bool updateResult = texture->UpdateData(data.data(), data.size() * sizeof(uint32_t), 0);
    TEST_ASSERT(updateResult, "UpdateData via staging failed");
    
    device.DestroyTexture(handle);
    device.Shutdown();
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("MetalTextureTests");
    suite->AddTestCase(TestCase("Creation", TestTextureCreation));
    suite->AddTestCase(TestCase("Update", TestTextureUpdate));
    suite->AddTestCase(TestCase("StagingUpdate", TestTextureStagingUpdate));
    
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    return 0;
}
