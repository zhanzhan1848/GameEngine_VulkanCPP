// T4.6.5 part 29 — macOS NSWindowWillCloseNotification observer.
//
// Engine's PlatformMac.cpp uses metal-cpp (NS::Window*) for window creation.
// metal-cpp doesn't expose NSWindowDelegate protocol cleanly through its C++
// shim. The simplest path to detect "user clicked close button" is to install
// a block-based observer on NSWindowWillCloseNotification via
// NSNotificationCenter. This file does that, calling back into
// primal::platform::notify_any_window_closed() (defined in PlatformMac.cpp)
// which sets the global g_any_window_closed flag polled by window::is_closed().
//
// Install point: primal_install_window_close_observer() is called once on the
// first window creation. The function is idempotent (dispatch_once).

#import <Cocoa/Cocoa.h>
#import <dispatch/dispatch.h>

#include "Engine/Platform/Platform.h"

extern "C" void primal_install_window_close_observer()
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowWillCloseNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification* _note) {
                        primal::platform::notify_any_window_closed();
                    }];
    });
}
