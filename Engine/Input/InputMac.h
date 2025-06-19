#pragma once
#if __APPLE__
#include "CommonHeaders.h"

#include <Cocoa/Cocoa.h>


namespace primal::input 
{
    class InputProessMacOS;

    void process_input_message(NSEvent* event);
}

#endif // __APPLE__