//-------------------------------------------------------------------------------------------------------------------------------------------------------------
//
// MacKeyboard.mm - macOS 键盘输入监控实现
// 
// 游戏引擎 - macOS 平台输入处理模块
// 提供键盘和鼠标事件的监控和处理功能
//
//-------------------------------------------------------------------------------------------------------------------------------------------------------------

#include <iostream>
#import <Cocoa/Cocoa.h>
#include "MacKeyboard.h"
#include "Input/InputMac.h"

@interface KeyboardMonitor : NSObject
@property (strong, nonatomic) id eventMonitor;
@end

@implementation KeyboardMonitor

/**
 * 开始监控键盘和鼠标事件
 * 使用 NSEvent 的本地监控器来捕获系统事件
 */
- (void)startMonitoring {
    @autoreleasepool {
        self.eventMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskKeyDown | 
                                                      NSEventMaskKeyUp |
                                                      NSEventMaskLeftMouseDown |
                                                      NSEventMaskLeftMouseUp //|
                                                    //   NSEventMaskRightMouseDown |
                                                    //   NSEventMaskRightMouseUp |
                                                    //   NSEventMaskOtherMouseDown |
                                                    //   NSEventMaskOtherMouseUp |
                                                    //   NSEventMaskMouseMoved |
                                                    //   NSEventMaskLeftMouseDragged |
                                                    //   NSEventMaskRightMouseDragged |
                                                    //   NSEventMaskOtherMouseDragged |
                                                    //   NSEventMaskScrollWheel
                                                      )
         handler:^NSEvent *(NSEvent *event) {
            @autoreleasepool {
                // std::cout << "Key pressed: " << [[event characters] UTF8String] << std::endl;
                primal::input::process_input_message(event);
                return event;
            }
        }];
    }
}

/**
 * 停止监控事件并清理资源
 */
- (void)stopMonitoring {
    @autoreleasepool {
        if (self.eventMonitor) {
            [NSEvent removeMonitor:self.eventMonitor];
            self.eventMonitor = nil;
        }
    }
}

/**
 * 析构函数，确保资源被正确释放
 */
- (void)dealloc {
    [self stopMonitoring];
    [super dealloc];
}

@end

// 全局监控器实例
static KeyboardMonitor *g_keyboardMonitor = nil;

/**
 * 开始监控键盘输入
 * 创建并启动键盘监控器实例
 */
void monitorKeyboardInput() {
    @autoreleasepool {
        if (!g_keyboardMonitor) {
            g_keyboardMonitor = [[KeyboardMonitor alloc] init];
            [g_keyboardMonitor startMonitoring];
        }
    }
}

/**
 * 停止监控键盘输入
 * 清理全局监控器实例
 */
void stopKeyboardMonitoring() {
    @autoreleasepool {
        if (g_keyboardMonitor) {
            [g_keyboardMonitor stopMonitoring];
            g_keyboardMonitor = nil;
        }
    }
}