// ============================================================================
// 文件：TestMetalPipeline.cpp
// 描述：Metal 管线状态对象 (PSO) 单元测试
//       验证管线创建、状态配置和资源管理
// 作者：AI助手
// ============================================================================

#ifdef __APPLE__

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalPipeline.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace primal::graphics::rhi;

// 简单的测试框架宏
#define TEST_ASSERT(condition, message) \
    if (!(condition)) { \
        std::cerr << "[FAILED] " << message << " (" << #condition << ")" << std::endl; \
        return false; \
    } else { \
        std::cout << "[PASS] " << message << std::endl; \
    }

// 模拟的 Shader 源码 (用于创建 dummy shader)
const char* kVertexShaderSource = R"(
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

const char* kFragmentShaderSource = R"(
    #include <metal_stdlib>
    using namespace metal;
    
    fragment float4 fragmentMain() {
        return float4(1.0, 0.0, 0.0, 1.0);
    }
)";

const char* kComputeShaderSource = R"(
    #include <metal_stdlib>
    using namespace metal;
    kernel void computeMain(uint2 gid [[thread_position_in_grid]]) {
    }
)";

class MetalPipelineTest {
public:
    bool Initialize() {
        DeviceDesc desc;
        desc.enableDebug = true; // enableDebug, not debugMode
        device_ = std::make_unique<MetalDevice>(desc);
        return device_->Initialize();
    }

    void Shutdown() {
        std::cout << "Test Shutdown start..." << std::endl;
        if (device_) {
            device_->Shutdown();
            device_.reset();
        }
        std::cout << "Test Shutdown end." << std::endl;
    }

    bool TestGraphicsPipelineCreation() {
        // NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        std::cout << "\n--- Testing Graphics Pipeline Creation ---\n";

        // 1. 创建 Shader
        ShaderHandle vsHandle = device_->CreateShader(
            kVertexShaderSource, 
            strlen(kVertexShaderSource), 
            ShaderStage::Vertex, 
            "vertexMain"
        );
        TEST_ASSERT(vsHandle != handles::INVALID_SHADER, "Create Vertex Shader");

        ShaderHandle fsHandle = device_->CreateShader(
            kFragmentShaderSource, 
            strlen(kFragmentShaderSource), 
            ShaderStage::Pixel, 
            "fragmentMain"
        );
        TEST_ASSERT(fsHandle != handles::INVALID_SHADER, "Create Fragment Shader");

        // 2. 创建 Pipeline Layout (Metal backend might ignore desc content, but handle is needed)
        PipelineLayoutDesc layoutDesc;
        PipelineLayoutHandle layoutHandle = device_->CreatePipelineLayout(layoutDesc);
        TEST_ASSERT(layoutHandle != handles::INVALID_PIPELINE_LAYOUT, "Create Pipeline Layout");

        // 3. 配置 Pipeline
        GraphicsPipelineDesc pipelineDesc{};
        pipelineDesc.vertexShader = vsHandle;
        pipelineDesc.pixelShader = fsHandle;
        pipelineDesc.layout = layoutHandle;
        
        // 混合状态
        pipelineDesc.enableBlend = false;
        pipelineDesc.srcColorBlendFactor = BlendFactor::One;
        pipelineDesc.dstColorBlendFactor = BlendFactor::Zero;
        pipelineDesc.colorBlendOp = BlendOp::Add;
        
        // 光栅化状态
        pipelineDesc.cullMode = CullMode::None;
        pipelineDesc.fillMode = FillMode::Solid;
        // pipelineDesc.frontCounterClockwise = true; // Not supported in desc currently
        
        // 深度模板状态
        pipelineDesc.enableDepthTest = true;
        pipelineDesc.enableDepthWrite = true;
        pipelineDesc.depthFunc = ComparisonFunc::Less;
        
        // 顶点输入布局
        VertexInputAttribute attrDesc;
        attrDesc.location = 0;
        attrDesc.format = DataFormat::RGB32_Float; // Corrected format
        attrDesc.offset = 0;
        
        VertexInputBinding bindingDesc;
        bindingDesc.binding = 0;
        bindingDesc.stride = 12;
        bindingDesc.perVertex = true;
        
        pipelineDesc.vertexAttributes.push_back(attrDesc);
        pipelineDesc.vertexBindings.push_back(bindingDesc);
        
        pipelineDesc.topology = PrimitiveTopology::TriangleList; // Corrected member name

        // 渲染目标格式
        pipelineDesc.renderTargetCount = 1;
        pipelineDesc.renderTargetFormats[0] = DataFormat::BGRA8_UNorm; // Metal default swapchain format usually
        pipelineDesc.depthStencilFormat = DataFormat::D32_Float;

        // 4. 创建 Pipeline
        PipelineHandle pipelineHandle = device_->CreateGraphicsPipeline(pipelineDesc);
        TEST_ASSERT(pipelineHandle != handles::INVALID_PIPELINE, "Create Graphics Pipeline");

        // 5. 销毁
        device_->DestroyPipeline(pipelineHandle);
        device_->DestroyPipelineLayout(layoutHandle);
        device_->DestroyShader(vsHandle);
        device_->DestroyShader(fsHandle);
        
        // pool->release();
        return true;
    }

    bool TestComputePipelineCreation() {
        std::cout << "\n--- Testing Compute Pipeline Creation ---\n";

        // 1. 创建 Shader
        ShaderHandle csHandle = device_->CreateShader(
            kComputeShaderSource, 
            strlen(kComputeShaderSource), 
            ShaderStage::Compute, 
            "computeMain"
        );
        TEST_ASSERT(csHandle != handles::INVALID_SHADER, "Create Compute Shader");

        // 2. 创建 Pipeline Layout
        PipelineLayoutDesc layoutDesc;
        PipelineLayoutHandle layoutHandle = device_->CreatePipelineLayout(layoutDesc);
        TEST_ASSERT(layoutHandle != handles::INVALID_PIPELINE_LAYOUT, "Create Pipeline Layout");

        // 3. 配置 Pipeline
        ComputePipelineDesc pipelineDesc{};
        pipelineDesc.computeShader = csHandle;
        pipelineDesc.layout = layoutHandle;

        // 4. 创建 Pipeline
        PipelineHandle pipelineHandle = device_->CreateComputePipeline(pipelineDesc);
        TEST_ASSERT(pipelineHandle != handles::INVALID_PIPELINE, "Create Compute Pipeline");

        // 5. 销毁
        device_->DestroyPipeline(pipelineHandle);
        device_->DestroyPipelineLayout(layoutHandle);
        device_->DestroyShader(csHandle);
        return true;
    }

private:
    std::unique_ptr<MetalDevice> device_;
};

int main() {
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

    MetalPipelineTest test;
    if (!test.Initialize()) {
        std::cerr << "[FAILED] Initialize Metal Device" << std::endl;
        return 1;
    }

    if (!test.TestGraphicsPipelineCreation()) {
        std::cerr << "[FAILED] TestGraphicsPipelineCreation" << std::endl;
        test.Shutdown();
        return 1;
    }

    if (!test.TestComputePipelineCreation()) {
        std::cerr << "[FAILED] TestComputePipelineCreation" << std::endl;
        test.Shutdown();
        return 1;
    }

    test.Shutdown();

    pool->release();
    std::cout << "All Tests Passed!" << std::endl;

    return 0;
}

#else

int main() {
    return 0;
}

#endif
