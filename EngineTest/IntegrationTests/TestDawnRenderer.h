#pragma once

#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "Test.h"
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"

#include <string>
#include <functional>
#include <vector>

namespace primal::test {

struct DawnTestCase {
    std::string name;
    std::function<bool()> run;
};

class DawnRendererTest : public Test {
public:
    bool initialize() override;
    void run() override;
    void shutdown() override;

private:
    bool CreateDevice();
    void DestroyDevice();

    // Individual test cases
    bool TestDeviceInitShutdown();
    bool TestBufferCreateMapWrite();
    bool TestTextureCreateUpload();
    bool TestShaderCompileWGSL();
    bool TestGraphicsPipelineCreate();
    bool TestClearScreen();
    bool TestDrawTriangle();
    bool TestDescriptorSetBinding();
    bool TestComputeDispatch();
    bool TestSyncFence();
    bool TestMultiFrame();

    std::vector<DawnTestCase> testCases_;
    primal::graphics::rhi::DawnDevice* device_{nullptr};

    // Helper
    static std::string ReadShaderFile(const char* path);
};

} // namespace primal::test

// Engine_Test alias used by main entry point
using Engine_Test = primal::test::DawnRendererTest;

#endif // ENABLE_WEBGPU
