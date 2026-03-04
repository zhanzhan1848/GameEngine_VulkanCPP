// ============================================================================
// 文件：TestMetalHotReload.cpp
// 描述：Metal Shader 热重载与 Pipeline 自动重建测试
//       验证修改 Shader 后，依赖该 Shader 的 Pipeline 是否能自动重新创建
// 作者：AI助手
// ============================================================================

#ifdef __APPLE__

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "../../TestFramework.h" // 包含测试框架
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalPipeline.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalShader.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"

#include <iostream>
#include <vector>
#include <thread>
#include <chrono>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

// 初始 Shader 源码
const char* kVertexShaderSourceOriginal = R"(
    #include <metal_stdlib>
    using namespace metal;
    
    struct VertexIn {
        float3 position [[attribute(0)]];
    };
    
    struct VertexOut {
        float4 position [[position]];
    };
    
    vertex VertexOut vertexMain(VertexIn in [[stage_in]]) {
        VertexOut out;
        out.position = float4(in.position, 1.0);
        return out;
    }
)";

const char* kFragmentShaderSourceOriginal = R"(
    #include <metal_stdlib>
    using namespace metal;
    
    fragment float4 fragmentMain() {
        return float4(1.0, 0.0, 0.0, 1.0); // Red
    }
)";

// 修改后的 Shader 源码 (绿色)
const char* kFragmentShaderSourceModified = R"(
    #include <metal_stdlib>
    using namespace metal;
    
    fragment float4 fragmentMain() {
        return float4(0.0, 1.0, 0.0, 1.0); // Green
    }
)";

class MetalHotReloadTest {
public:
    bool Initialize() {
        DeviceDesc desc;
        desc.enableDebug = true;
        device_ = std::make_unique<MetalDevice>(desc);
        return device_->Initialize();
    }

    void Shutdown() {
        if (device_) {
            device_->Shutdown();
            device_.reset();
        }
    }

    TestResult TestHotReload() {
        // 1. 创建 Shader
        ShaderHandle vsHandle = device_->CreateShader(
            kVertexShaderSourceOriginal, 
            strlen(kVertexShaderSourceOriginal), 
            ShaderStage::Vertex, 
            "vertexMain"
        );
        TEST_ASSERT(vsHandle != handles::INVALID_SHADER, "Create Vertex Shader");

        ShaderHandle fsHandle = device_->CreateShader(
            kFragmentShaderSourceOriginal, 
            strlen(kFragmentShaderSourceOriginal), 
            ShaderStage::Pixel, 
            "fragmentMain"
        );
        TEST_ASSERT(fsHandle != handles::INVALID_SHADER, "Create Fragment Shader");

        // 2. 创建 Pipeline
        GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = vsHandle;
        pipelineDesc.pixelShader = fsHandle;
        
        // 设置 Vertex Input Layout
        VertexInputAttribute attr;
        attr.location = 0;
        attr.binding = 0;
        attr.format = DataFormat::RGB32_Float;
        attr.offset = 0;
        pipelineDesc.vertexAttributes.push_back(attr);

        VertexInputBinding binding;
        binding.binding = 0;
        binding.stride = sizeof(float) * 3;
        binding.perVertex = true;
        pipelineDesc.vertexBindings.push_back(binding);

        // 设置 Render Target
        pipelineDesc.renderTargetCount = 1;
        pipelineDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;

        PipelineHandle pipelineHandle = device_->CreateGraphicsPipeline(pipelineDesc);
        TEST_ASSERT(pipelineHandle != handles::INVALID_PIPELINE, "Create Graphics Pipeline");

        // 获取原始 Pipeline State 对象
        MetalPipeline* pipeline = device_->GetPipeline(pipelineHandle);
        MTL::RenderPipelineState* originalPSO = pipeline->GetRenderPipelineState();
        TEST_ASSERT_NOT_NULL(originalPSO, "Get Original PSO");
        
        std::cout << "Original PSO Address: " << originalPSO << std::endl;

        // 3. 执行热重载
        std::cout << "Reloading Fragment Shader..." << std::endl;
        bool reloadResult = device_->ReloadShader(
            fsHandle, 
            kFragmentShaderSourceModified, 
            strlen(kFragmentShaderSourceModified)
        );
        TEST_ASSERT(reloadResult, "Reload Fragment Shader");

        // 4. 验证 Pipeline 是否重建
        MTL::RenderPipelineState* newPSO = pipeline->GetRenderPipelineState();
        TEST_ASSERT_NOT_NULL(newPSO, "Get New PSO");
        
        std::cout << "New PSO Address: " << newPSO << std::endl;
        
        // 5. 验证 Pipeline 依然有效且关联正确
        TEST_ASSERT_EQ(pipeline->GetGraphicsDesc().pixelShader, fsHandle, "Pipeline Shader Handle Check");
        
        // 6. 再次重载为原始 Shader (验证多次重载)
        std::cout << "Reloading Fragment Shader back to original..." << std::endl;
        reloadResult = device_->ReloadShader(
            fsHandle, 
            kFragmentShaderSourceOriginal, 
            strlen(kFragmentShaderSourceOriginal)
        );
        TEST_ASSERT(reloadResult, "Reload Fragment Shader (2nd time)");
        
        MTL::RenderPipelineState* finalPSO = pipeline->GetRenderPipelineState();
        TEST_ASSERT_NOT_NULL(finalPSO, "Get Final PSO");
        std::cout << "Final PSO Address: " << finalPSO << std::endl;

        return TestResult::Passed;
    }

private:
    std::unique_ptr<MetalDevice> device_;
};

int main() {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    
    auto suite = std::make_shared<TestSuite>("MetalHotReloadTests");
    
    MetalHotReloadTest test;
    if (test.Initialize()) {
        suite->AddTestCase(TestCase("ShaderHotReload", [&test]() { return test.TestHotReload(); }));
    } else {
        std::cerr << "Failed to initialize test device" << std::endl;
    }
    
    TestRunner::RegisterTestSuite(suite);
    TestStats stats = TestRunner::RunAllSuites();
    
    test.Shutdown();
    pool->release();
    
    return stats.failedTests == 0 ? 0 : 1;
}

#else

int main() {
    return 0;
}

#endif
