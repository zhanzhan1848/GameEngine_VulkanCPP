#pragma once
#include "RenderTestFramework.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderMesh.h"
#include "Engine/Graphics/Material.h"
#include "Engine/Graphics/MaterialInstance.h"
#include "Engine/Platform/Platform.h"

/**
 * @brief CSM (Cascaded Shadow Maps) 集成测试用例
 * @details 验证 CSM 阴影渲染流程，包括 Shadow Pass 和新的 CSM Shader
 */
class CSMIntegrationTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

protected:
    // 基础资源
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device = nullptr;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    
    // 场景对象
    primal::graphics::RenderScene scene;
    primal::graphics::RenderView view;
    
    // 渲染资源
    primal::graphics::RenderMesh* cubeMesh = nullptr;
    primal::graphics::Material* material = nullptr;
    primal::graphics::MaterialInstance* materialInstance = nullptr;
    primal::graphics::MaterialInstance* floorMaterialInstance = nullptr;
    
    // RHI 资源
    primal::graphics::rhi::DescriptorSetLayoutHandle dsLayout = primal::graphics::rhi::handles::INVALID_RESOURCE;
    primal::graphics::rhi::PipelineLayoutHandle pipelineLayout = primal::graphics::rhi::handles::INVALID_RESOURCE;
    
    // 测试状态
    float rotationAngle = 0.0f;
    
    // 辅助方法
    std::string ReadShaderFile(const std::string& filepath);
};

/**
 * @brief 测试入口类
 */
class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
