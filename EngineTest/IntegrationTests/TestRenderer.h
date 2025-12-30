// ============================================================================
// 文件：TestRenderer.h
// 描述：Engine_Test 测试渲染器头文件，支持 MacOS 下 Metal-CPP 渲染，
//       采用定时器驱动渲染循环，兼容多平台。
// 作者：AI助手
// ============================================================================

#pragma once
#include "Test.h"

#ifndef __APPLE__
class Engine_Test : public Test
{
public :
	bool initialize() override;
	void run() override;
	void shutdown() override;
};
#else
#include <AppKit/AppKit.hpp>
#include <CoreFoundation/CoreFoundation.h>

// ============================================================================
// 类名：Engine_Test
// 说明：MacOS 平台下的测试渲染器，继承自 Test 和 NS::ApplicationDelegate，
//       使用 CFRunLoopTimer 实现 60FPS 渲染循环。
// ============================================================================
class Engine_Test : public Test, public NS::ApplicationDelegate
{
public :
	bool initialize() override;
	void run() override;
	void shutdown() override;

	virtual void applicationDidFinishLaunching(NS::Notification* notification) override
	{
		NS::Application* pApp = reinterpret_cast< NS::Application* >( notification->object() );
    	pApp->activateIgnoringOtherApps( true );
		
		if (!initialize()) {
			NS::Application::sharedApplication()->terminate(nullptr);
			return;
		}

		// 使用自定义运行时循环
        setupCustomRunLoop();
        
        // 启动主运行循环
        CFRunLoopRun();
	}

	virtual void applicationWillFinishLaunching(NS::Notification* notification) override
	{
		// if(!initialize()) return;
		NS::Application* pApp = reinterpret_cast< NS::Application* >( notification->object() );
		pApp->setActivationPolicy( NS::ActivationPolicy::ActivationPolicyRegular );
	}

	virtual bool applicationShouldTerminateAfterLastWindowClosed([[maybe_unused]] NS::Application* pSender ) override
	{
		shutdown();
		return true;
	}
private:
	[[maybe_unused]] CFRunLoopTimerRef _displayLink{ nullptr };
	CFRunLoopRef _runLoop{ nullptr };
	CFRunLoopSourceRef _runLoopSource{ nullptr };

	// 自定义运行时循环方法
    void setupCustomRunLoop()
	{
		// 获取主线程的RunLoop
		_runLoop = CFRunLoopGetMain();
    
		// 创建一个定时器，用于控制渲染频率
		CFRunLoopTimerContext context = {0, this, NULL, NULL, NULL};
		// 创建一个60Hz的定时器 (1.0/60.0 秒间隔)
		_displayLink = CFRunLoopTimerCreate(
			kCFAllocatorDefault,  // 分配器
			CFAbsoluteTimeGetCurrent(),  // 开始时间
			1.0/60.0,  // 间隔时间 (60 FPS)
			0,  // flags
			0,  // 优先级
			displayLinkCallback,  // 回调函数
			&context  // 上下文
		);
		// 将定时器添加到主RunLoop
		CFRunLoopAddTimer(_runLoop, _displayLink, kCFRunLoopCommonModes);

		// // 创建一个RunLoopSource，而不是定时器
        // CFRunLoopSourceContext sourceContext = { 0 };
        // sourceContext.version = 0;
        // sourceContext.info = this;
		// // TODO: Use functions to restructure the code
        // sourceContext.perform = [](void* info) {
        //     Engine_Test* test = static_cast<Engine_Test*>(info);
        //     test->run();
            
        //     // 立即再次触发源，实现连续渲染
        //     CFRunLoopSourceSignal(test->_runLoopSource);
        //     CFRunLoopWakeUp(test->_runLoop);
        // };
        
        // _runLoopSource = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &sourceContext);
        
        // // 将源添加到RunLoop
        // CFRunLoopAddSource(_runLoop, _runLoopSource, kCFRunLoopCommonModes);
        
        // // 首次触发源
        // CFRunLoopSourceSignal(_runLoopSource);
        // CFRunLoopWakeUp(_runLoop);
	}

    static void displayLinkCallback([[maybe_unused]] CFRunLoopTimerRef timer, void* info)
	{
		// 从上下文中获取Engine_Test实例
		Engine_Test* test = static_cast<Engine_Test*>(info);
		
		// 调用run方法执行渲染
		test->run();
		
	}
};
#endif