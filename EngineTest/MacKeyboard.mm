#include <iostream>
#import <Cocoa/Cocoa.h>
#include "MacKeyboard.h"
#include "Input/InputMac.h"

@interface KeyboardMonitor : NSObject
@end

@implementation KeyboardMonitor

- (void)startMonitoring {
    [NSEvent addLocalMonitorForEventsMatchingMask:(NSEventMaskKeyDown | 
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
        // std::cout << "Key pressed: " << [[event characters] UTF8String] << std::endl;
        primal::input::process_input_message(event);
        return event;
    }];
}

@end

void monitorKeyboardInput() {
    KeyboardMonitor *monitor = [[KeyboardMonitor alloc] init];
    [monitor startMonitoring];
}