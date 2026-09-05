#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "player_window.h"

namespace {
NSApplication* g_app = nullptr;
NSWindow* g_window = nullptr;
NSView* g_content = nullptr;
bool g_shouldClose = false;

void SetShouldClose() { g_shouldClose = true; }
} // namespace

// Close-box handler: keep the window alive, flip the flag — the engine
// loop owns teardown order (surface → pipeline → engine must unwind
// before NSWindow goes away).
@interface PlayerWindowDelegate : NSObject <NSWindowDelegate>
@end
@implementation PlayerWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)window
{
    (void)window;
    SetShouldClose();
    return NO;
}
@end

void* PlayerWindowCreate(int width, int height, const char* title, int resizable)
{
    @autoreleasepool {
        g_app = [NSApplication sharedApplication];
        [g_app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [g_app finishLaunching];

        NSRect frame = NSMakeRect(0, 0, width, height);
        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable;
        if (resizable) style |= NSWindowStyleMaskResizable;
        g_window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:style
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
        NSString* nsTitle = [NSString stringWithUTF8String: title ? title : "Primal Player"];
        [g_window setTitle:nsTitle];
        if (resizable) [g_window setContentMinSize:NSMakeSize(320, 240)];
        [g_window center];

        g_content = [[NSView alloc] initWithFrame:frame];
        g_content.wantsLayer = YES;
        g_content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [g_window setContentView:g_content];

        PlayerWindowDelegate* delegate = [[PlayerWindowDelegate alloc] init];
        [g_window setDelegate:delegate];
        // Keep the delegate alive for the process lifetime (no ARC here —
        // matches the -fno-exceptions non-ARC build of the other tools).
        [delegate retain];

        // Phase 1 退出路径 = 关闭按钮（windowShouldClose → 置位标志）。
        // Esc 监听暂缺（addLocalMonitorForEventsMatchingMask 在无 GUI 会话
        // 报 unrecognized selector，待查）；主循环泵事件不依赖它。
        [g_window makeKeyAndOrderFront:nil];
        [g_app activateIgnoringOtherApps:YES];
        return (void*)g_content;
    }
}

int PlayerWindowPump(void)
{
    if (!g_app || g_shouldClose) return 0;
    @autoreleasepool {
        for (;;) {
            NSEvent* event = [g_app
                nextEventMatchingMask:NSEventMaskAny
                            untilDate:[NSDate distantPast]
                               inMode:NSDefaultRunLoopMode
                              dequeue:YES];
            if (!event) break;
            [g_app sendEvent:event];
        }
        [g_app updateWindows];
    }
    return g_shouldClose ? 0 : 1;
}

void PlayerWindowDestroy(void)
{
    @autoreleasepool {
        if (g_window) {
            [g_window orderOut:nil];
            [g_window release];
            g_window = nullptr;
        }
        g_content = nullptr;
    }
}
