#pragma once
#include "RenderTestFramework.h"
#include "Engine/Graphics/RenderPipeline/StandardRenderPipeline.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RenderView.h"
#include "Engine/Graphics/RenderScene.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Graphics/RHI/Systems/RenderSystem.h"

/**
 * @brief 标准渲染管线测试用例
 * @details 实现了具体的 StandardRenderPipeline 测试逻辑
 */
class StandardPipelineTestCase : public primal::test::RenderTestCase {
public:
    bool Initialize() override;
    void Run() override;
    void Shutdown() override;

protected:
    // 渲染资源
    std::unique_ptr<primal::graphics::rhi::RHIDeviceBase> device = nullptr;
    primal::platform::window window;
    primal::graphics::RenderSystem renderSystem;
    
    // 场景对象
    primal::graphics::StandardRenderPipeline* pipeline = nullptr;
    primal::graphics::RenderScene* scene = nullptr;
    primal::graphics::RenderView* view = nullptr;
};

/**
 * @brief 测试入口类
 * @details 继承自 RenderTestRunner，用于 Main.cpp 实例化
 */
class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
