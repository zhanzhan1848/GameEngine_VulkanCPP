# 游戏引擎测试系统文档

本文档详细描述了游戏引擎项目的测试架构，包括单元测试和渲染集成测试的框架组成、核心依赖及使用方法。

## 1. 单元测试框架 (Unit Test Framework)

单元测试用于验证引擎核心模块（如容器、算法、工具类等）的功能正确性。

*   **核心框架文件**: [`EngineTest/UnitTests/TestFramework.h`](UnitTests/TestFramework.h)
    *   该文件实现了一个轻量级的、Header-only 的 C++ 单元测试框架。
*   **主要组件**:
    *   `Engine::Test::TestCase`: 定义单个测试用例，包含测试名称、测试函数（`std::function`）和描述。
    *   `Engine::Test::TestSuite`: 管理一组测试用例，负责执行测试并统计结果（通过/失败/跳过）。
    *   `TEST_ASSERT`: 用于断言测试条件的宏。
*   **使用的库函数**:
    *   **C++ 标准库 (STL)**:
        *   `std::function`: 存储测试函数回调。
        *   `std::vector`: 存储测试用例列表。
        *   `std::chrono`: 用于高精度的测试耗时统计。
        *   `std::iostream`: 输出测试日志和结果。
*   **示例**:
    ```cpp
    #include "TestFramework.h"
    
    // 定义测试函数
    Engine::Test::TestResult MyTest() {
        TEST_ASSERT(1 + 1 == 2, "Math error");
        return Engine::Test::TestResult::Passed;
    }
    
    // 注册并运行
    Engine::Test::TestSuite suite("MyModuleTests");
    suite.AddTestCase({"Addition", MyTest});
    suite.RunAllTests();
    ```

## 2. 渲染集成测试框架 (Rendering Integration Test Framework)

渲染集成测试用于验证图形API（Metal/Vulkan/D3D12）、渲染管线、窗口系统及核心渲染组件的协同工作情况。

*   **核心框架文件**:
    1.  **接口定义**: [`EngineTest/IntegrationTests/RenderTestFramework.h`](IntegrationTests/RenderTestFramework.h)
        *   定义了 `primal::test::RenderTestCase` 抽象基类，规范了渲染测试的生命周期 (`Initialize`, `Run`, `Shutdown`)。
        *   实现了 `primal::test::RenderTestRunner`，封装了平台相关的应用循环（如 macOS 的 `NSApplicationDelegate` 和 `CFRunLoop`）。
    2.  **基础接口**: [`EngineTest/IntegrationTests/Test.h`](IntegrationTests/Test.h)
        *   定义了最底层的 `Test` 接口和 `time_it` 计时工具。
    3.  **入口文件**: [`EngineTest/IntegrationTests/Main.cpp`](IntegrationTests/Main.cpp)
        *   包含 `main` 函数，负责实例化测试用例并启动平台消息循环。
*   **包含的核心层文件**:
    *   **渲染系统**: [`Engine/Graphics/RHI/Systems/RenderSystem.h`](../Engine/Graphics/RHI/Systems/RenderSystem.h) (负责 SwapChain 管理、帧同步)
    *   **渲染管线**: [`Engine/Graphics/RenderPipeline/StandardRenderPipeline.h`](../Engine/Graphics/RenderPipeline/StandardRenderPipeline.h)
    *   **RHI 核心**: [`Engine/Graphics/RHI/Core/RHIDevice.h`](../Engine/Graphics/RHI/Core/RHIDevice.h)
    *   **平台层**: [`Engine/Platform/Platform.h`](../Engine/Platform/Platform.h) (窗口创建与管理)
    *   **场景与视图**: [`Engine/Graphics/RenderScene.h`](../Engine/Graphics/RenderScene.h), [`Engine/Graphics/RenderView.h`](../Engine/Graphics/RenderView.h)
*   **使用的库函数**:
    *   **引擎内部库**: `primal::platform` (窗口), `primal::graphics` (渲染), `primal::math` (数学库)。
    *   **系统图形库 (macOS)**:
        *   `AppKit`: 应用程序生命周期管理。
        *   `CoreFoundation`: 运行循环 (`CFRunLoop`) 和定时器 (`CFRunLoopTimer`) 管理，用于驱动渲染循环。
        *   `Metal`: 底层图形 API 调用。

## 3. 测试构建与运行

项目使用 CMake 管理构建，提供了以下 Target 用于运行测试：

*   `run_all_tests`: 运行所有测试（单元测试 + 集成测试）。
*   `run_unit_tests`: 仅运行 `RHI_UnitTests`。
*   `run_integration_tests`: 仅运行 `IntegrationTests`。

### 目录结构参考
```
EngineTest/
├── UnitTests/
│   ├── TestFramework.h       <-- 单元测试框架核心
│   └── ...                   <-- 具体单元测试文件
├── IntegrationTests/
│   ├── RenderTestFramework.h <-- 渲染测试框架核心
│   ├── Test.h                <-- 基础测试接口
│   ├── Main.cpp              <-- 集成测试入口
│   ├── TestStandardPipeline.h/cpp <-- 具体渲染测试用例
│   └── ...
└── TESTING.md                <-- 本文档
```
