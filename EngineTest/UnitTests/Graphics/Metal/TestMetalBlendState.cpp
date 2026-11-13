/**
 * @file TestMetalBlendState.cpp
 * @brief Metal 混合状态测试
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-07
 * @version 1.0
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Graphics/RHI/Platforms/Metal/MetalShader.h"
#include "Graphics/RHI/Platforms/Metal/MetalPipeline.h"

using namespace primal::graphics::rhi;
using namespace Engine::Test;

// Simple MSL Shader Source for testing
const char* kTestShaderSource = R"(
    #include <metal_stdlib>
    using namespace metal;

    struct VertexOut {
        float4 position [[position]];
        float4 color;
    };

    vertex VertexOut vertexMain(uint vertexID [[vertex_id]]) {
        float4 positions[] = {
            float4(-0.5, -0.5, 0.0, 1.0),
            float4( 0.5, -0.5, 0.0, 1.0),
            float4( 0.0,  0.5, 0.0, 1.0)
        };
        VertexOut out;
        out.position = positions[vertexID];
        out.color = float4(1.0, 1.0, 1.0, 1.0);
        return out;
    }

    fragment float4 fragmentMain(VertexOut in [[stage_in]]) {
        return in.color;
    }
)";

TestResult TestBlendStateCreation() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        return TestResult::Failed;
    }

    // Create Shaders
    size_t sourceLen = strlen(kTestShaderSource);
    ShaderHandle vsHandle = device.CreateShader(kTestShaderSource, sourceLen, ShaderStage::Vertex, "vertexMain");
    ShaderHandle fsHandle = device.CreateShader(kTestShaderSource, sourceLen, ShaderStage::Pixel, "fragmentMain");

    TEST_ASSERT(vsHandle != handles::INVALID_SHADER, "Failed to create vertex shader");
    TEST_ASSERT(fsHandle != handles::INVALID_SHADER, "Failed to create fragment shader");

    // Test Case 1: Standard Alpha Blending
    {
        GraphicsPipelineDesc desc;
        desc.vertexShader = vsHandle;
        desc.pixelShader = fsHandle;
        desc.renderTargetCount = 1;
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.enableBlend = true;
        
        // Unified Blending: SrcAlpha, InvSrcAlpha
        desc.srcColorBlendFactor = BlendFactor::SrcAlpha;
        desc.dstColorBlendFactor = BlendFactor::InvSrcAlpha;
        desc.colorBlendOp = BlendOp::Add;
        
        desc.srcAlphaBlendFactor = BlendFactor::One;
        desc.dstAlphaBlendFactor = BlendFactor::Zero;
        desc.alphaBlendOp = BlendOp::Add;
        
        desc.depthStencilFormat = DataFormat::D32_Float; // Keep consistent with common setup
        desc.enableDepthTest = false;

        PipelineHandle pipeline = device.CreateGraphicsPipeline(desc);
        TEST_ASSERT(pipeline != handles::INVALID_PIPELINE, "Failed to create alpha blending pipeline");
        
        device.DestroyPipeline(pipeline);
    }

    // Test Case 2: Additive Blending
    {
        GraphicsPipelineDesc desc;
        desc.vertexShader = vsHandle;
        desc.pixelShader = fsHandle;
        desc.renderTargetCount = 1;
        desc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        desc.enableBlend = true;
        
        // Additive: One, One
        desc.srcColorBlendFactor = BlendFactor::One;
        desc.dstColorBlendFactor = BlendFactor::One;
        desc.colorBlendOp = BlendOp::Add;
        
        desc.srcAlphaBlendFactor = BlendFactor::One;
        desc.dstAlphaBlendFactor = BlendFactor::One;
        desc.alphaBlendOp = BlendOp::Add;

        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthTest = false;

        PipelineHandle pipeline = device.CreateGraphicsPipeline(desc);
        TEST_ASSERT(pipeline != handles::INVALID_PIPELINE, "Failed to create additive blending pipeline");
        
        device.DestroyPipeline(pipeline);
    }

    // Clean up
    device.DestroyShader(vsHandle);
    device.DestroyShader(fsHandle);
    device.Shutdown();

    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("MetalBlendStateTests");
    suite->AddTestCase(TestCase("TestBlendStateCreation", TestBlendStateCreation));
    
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    
    return 0;
}
