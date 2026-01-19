#pragma once
#include "Test.h"
#include <memory>
#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <iostream>

namespace primal::test {

/**
 * @brief 渲染测试用例基类
 * @details 所有具体的渲染测试逻辑都应继承此类并实现核心生命周期函数
 */
class RenderTestCase {
public:
    virtual ~RenderTestCase() = default;

    /**
     * @brief 初始化测试用例
     * @return 成功返回 true，失败返回 false
     */
    virtual bool Initialize() = 0;

    /**
     * @brief 执行单帧测试逻辑
     */
    virtual void Run() = 0;

    /**
     * @brief 清理资源
     */
    virtual void Shutdown() = 0;
};

/**
 * @brief 渲染测试运行器
 * @details 负责管理平台相关的应用程序生命周期、事件循环，并将核心逻辑委托给 RenderTestCase
 */
#ifdef __APPLE__

class RenderTestRunner : public Test, public NS::ApplicationDelegate {
public:
    explicit RenderTestRunner(std::unique_ptr<RenderTestCase> testCase)
        : _testCase(std::move(testCase)) {}

    // Test 接口实现
    bool initialize() override {
        if (_testCase) {
            return _testCase->Initialize();
        }
        return false;
    }

    void run() override {
        if (_testCase) {
            _testCase->Run();
        }
    }

    void shutdown() override {
        if (_testCase) {
            _testCase->Shutdown();
        }
    }

    // NS::ApplicationDelegate 接口实现
    virtual void applicationDidFinishLaunching(NS::Notification* notification) override {
        NS::Application* pApp = reinterpret_cast<NS::Application*>(notification->object());
        pApp->activateIgnoringOtherApps(true);
        
        if (!initialize()) {
            NS::Application::sharedApplication()->terminate(nullptr);
            return;
        }

        setupCustomRunLoop();
    }

    virtual void applicationWillFinishLaunching(NS::Notification* notification) override {
        NS::Application* pApp = reinterpret_cast<NS::Application*>(notification->object());
        pApp->setActivationPolicy(NS::ActivationPolicy::ActivationPolicyRegular);
    }

    virtual bool applicationShouldTerminateAfterLastWindowClosed([[maybe_unused]] NS::Application* pSender) override {
        shutdown();
        return true;
    }

private:
    std::unique_ptr<RenderTestCase> _testCase;
    [[maybe_unused]] CFRunLoopTimerRef _displayLink{ nullptr };
    CFRunLoopRef _runLoop{ nullptr };

    void setupCustomRunLoop() {
        std::cout << "Setting up custom run loop..." << std::endl;
        _runLoop = CFRunLoopGetMain();
    
        CFRunLoopTimerContext context = {0, this, NULL, NULL, NULL};
        // 60 FPS
        _displayLink = CFRunLoopTimerCreate(
            kCFAllocatorDefault,
            CFAbsoluteTimeGetCurrent(),
            1.0/60.0,
            0,
            0,
            displayLinkCallback,
            &context
        );
        CFRunLoopAddTimer(_runLoop, _displayLink, kCFRunLoopCommonModes);
        std::cout << "Timer added to run loop." << std::endl;
    }

    static void displayLinkCallback([[maybe_unused]] CFRunLoopTimerRef timer, void* info) {
        RenderTestRunner* runner = static_cast<RenderTestRunner*>(info);
        runner->run();
    }
};

#else
// Windows/Linux 实现 (简化版，仅作为占位，实际需参考 Test.h 的其他平台实现)
class RenderTestRunner : public Test {
public:
    explicit RenderTestRunner(std::unique_ptr<RenderTestCase> testCase)
        : _testCase(std::move(testCase)) {}

#ifdef __linux__
    bool initialize(void* disp) override { return _testCase ? _testCase->Initialize() : false; }
    void run(void* disp) override { if (_testCase) _testCase->Run(); }
#else
    bool initialize() override { return _testCase ? _testCase->Initialize() : false; }
    void run() override { if (_testCase) _testCase->Run(); }
#endif
    void shutdown() override { if (_testCase) _testCase->Shutdown(); }

private:
    std::unique_ptr<RenderTestCase> _testCase;
};
#endif

} // namespace primal::test
