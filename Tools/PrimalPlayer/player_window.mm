#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include "player_window.h"

#include <cstdio>

namespace {
NSApplication* g_app = nullptr;
NSWindow* g_window = nullptr;
NSView* g_content = nullptr;
bool g_shouldClose = false;

// 输入状态（仅主线程触达——事件泵与 poll 都在主循环线程）。
unsigned g_keys = 0;
float g_look_dx = 0.f;
float g_look_dy = 0.f;
float g_scroll_dy = 0.f;
bool g_dragging = false;

void SetShouldClose() { g_shouldClose = true; }

// keyCode → kPlayerKey* 位集（ANSI 布局虚拟键码；布局无关性够用——
// 运行时相机桥不追求文本输入级精确）。
unsigned KeyCodeBit(NSEvent* event)
{
    switch (event.keyCode) {
        case 13: return kPlayerKeyW;
        case 0:  return kPlayerKeyA;
        case 1:  return kPlayerKeyS;
        case 2:  return kPlayerKeyD;
        case 12: return kPlayerKeyQ;
        case 14: return kPlayerKeyE;
        case 126: return kPlayerKeyUpArrow;
        case 125: return kPlayerKeyDownArrow;
        case 123: return kPlayerKeyLeftArrow;
        case 124: return kPlayerKeyRightArrow;
        default: return 0;
    }
}
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

// 输入视图：吃掉内容区全部键鼠事件（hitTest 返回 self，引擎挂进来的
// MTKView 只显示不参与命中），累计进全局状态供 PlayerWindowPollInput
// 快照消费。不做 addLocalMonitorForEventsMatchingMask（无 GUI 会话报
// unrecognized selector 的旧坑，2026-09-03 留痕①）。
@interface PlayerInputView : NSView
@end
@implementation PlayerInputView

- (BOOL)acceptsFirstResponder { return YES; }

// 引擎的 MTKView 是本视图的子视图且铺满内容区——默认命中测试会优先
// 派给最深的子视图；截获为本视图才能收到鼠标事件。
- (NSView*)hitTest:(NSPoint)point { (void)point; return self; }

- (void)keyDown:(NSEvent*)event
{
    if (event.keyCode == 53) { // Escape —— 与关闭按钮同一退出路径
        SetShouldClose();
        return;
    }
    const unsigned bit = KeyCodeBit(event);
    if (bit != 0) {
        g_keys |= bit; // 按住语义（位集幂等，无 key repeat 重复计数）
        return;
    }
    [super keyDown:event];
}

- (void)keyUp:(NSEvent*)event
{
    const unsigned bit = KeyCodeBit(event);
    if (bit != 0) g_keys &= ~bit;
}

- (void)flagsChanged:(NSEvent*)event
{
    const bool shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    if (shift) g_keys |= kPlayerKeyShift; else g_keys &= ~kPlayerKeyShift;
}

- (void)mouseDown:(NSEvent*)event { (void)event; g_dragging = true; }
- (void)mouseUp:(NSEvent*)event { (void)event; g_dragging = false; }
- (void)rightMouseDown:(NSEvent*)event { (void)event; g_dragging = true; }
- (void)rightMouseUp:(NSEvent*)event { (void)event; g_dragging = false; }

- (void)mouseDragged:(NSEvent*)event
{
    // deltaX/deltaY 单位是点（points），主循环按灵敏度换算成度。
    g_look_dx += static_cast<float>(event.deltaX);
    g_look_dy += static_cast<float>(event.deltaY);
}
- (void)rightMouseDragged:(NSEvent*)event { [self mouseDragged:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self mouseDragged:event]; }

- (void)scrollWheel:(NSEvent*)event
{
    g_scroll_dy += static_cast<float>(event.scrollingDeltaY);
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

        g_content = [[PlayerInputView alloc] initWithFrame:frame];
        g_content.wantsLayer = YES;
        g_content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [g_window setContentView:g_content];

        PlayerWindowDelegate* delegate = [[PlayerWindowDelegate alloc] init];
        [g_window setDelegate:delegate];
        // Keep the delegate alive for the process lifetime (no ARC here —
        // matches the -fno-exceptions non-ARC build of the other tools).
        [delegate retain];

        // 相机桥：内容视图兼任 first responder 接收键鼠（Esc 在其 keyDown
        // 里走关闭路径）。退出路径 = 关闭按钮 / Esc，行为一致。
        [g_window makeFirstResponder:g_content];
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

void PlayerWindowPollInput(PlayerInputState* out)
{
    if (!out) return;
    out->keys = g_keys;
    out->look_dx = g_look_dx;
    out->look_dy = g_look_dy;
    out->scroll_dy = g_scroll_dy;
    out->dragging = g_dragging ? 1 : 0;
    g_look_dx = 0.f;
    g_look_dy = 0.f;
    g_scroll_dy = 0.f;
}

// audio S1 的停止入口在文件尾部定义（AVFoundation 段），此处前置声明。
static void PlayerAudioStopAll(void);

void PlayerWindowDestroy(void)
{
    @autoreleasepool {
        PlayerAudioStopAll();
        if (g_window) {
            [g_window orderOut:nil];
            [g_window release];
            g_window = nullptr;
        }
        g_content = nullptr;
    }
}

// ---- audio S1（2026-10-14）：AVAudioPlayer 播放 --------------------------
#import <AVFoundation/AVFoundation.h>

static NSMutableArray* g_audio_players = nil;

int PlayerAudioPlayFile(const char* path, float volume, int loop)
{
    if (!path || !*path) return 0;
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]
                                isDirectory:NO];
        NSError* error = nil;
        AVAudioPlayer* player = [[AVAudioPlayer alloc] initWithContentsOfURL:url
                                                                       error:&error];
        if (!player || error) {
            std::fprintf(stderr, "[player] audio open failed: %s (%s)\n", path,
                         error ? error.localizedDescription.UTF8String : "unknown");
            return 0;
        }
        player.volume = volume < 0.f ? 0.f : (volume > 1.f ? 1.f : volume);
        player.numberOfLoops = loop ? -1 : 0;   // -1 = 循环到 stop
        player.enableRate = NO;
        if (![player play]) {
            std::fprintf(stderr, "[player] audio play failed: %s\n", path);
            return 0;
        }
        if (!g_audio_players) g_audio_players = [[NSMutableArray alloc] init];
        [g_audio_players addObject:player];
        return 1;
    }
}

static void PlayerAudioStopAll(void)
{
    @autoreleasepool {
        if (!g_audio_players) return;
        for (AVAudioPlayer* p in g_audio_players) [p stop];
        [g_audio_players removeAllObjects];
    }
}
