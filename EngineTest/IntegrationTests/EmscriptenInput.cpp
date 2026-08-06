#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#include <cstring>

#include "Engine/Common/CommonHeaders.h"
#include "Engine/EngineAPI/Input.h"
#include "Engine/Input/Input.h"

namespace {

bool keyState[512] = {};
bool mouseButtons[3] = {};
float mouseX = 0, mouseY = 0;
float mouseDX = 0, mouseDY = 0;

// Map emscripten keyCode → primal::input::input_code::code.
// Returns u32_invalid_id when no mapping exists. We only need the codes the
// engine actually reads — WASDQE, arrows, shift, OP, RT, [](),.-+ — anything
// else is silently dropped.
u32 emscripten_key_to_input_code(int code) {
    using ic = primal::input::input_code;
    // Letters A-Z (keyCodes 65-90)
    if (code >= 65 && code <= 90) {
        return static_cast<u32>(ic::key_a) + (code - 65);
    }
    // Digits 0-9 (keyCodes 48-57)
    if (code >= 48 && code <= 57) {
        return static_cast<u32>(ic::key_0) + (code - 48);
    }
    // Arrow keys (keyCodes 37-40)
    if (code >= 37 && code <= 40) {
        // 37=left, 38=up, 39=right, 40=down
        return static_cast<u32>(ic::key_left) + (code - 37);
    }
    switch (code) {
        case 16:   return static_cast<u32>(ic::key_shift);
        case 17:   return static_cast<u32>(ic::key_control);
        case 18:   return static_cast<u32>(ic::key_atl);
        case 32:   return static_cast<u32>(ic::key_space);
        case 9:    return static_cast<u32>(ic::key_tab);
        case 13:   return static_cast<u32>(ic::key_return);
        case 27:   return static_cast<u32>(ic::key_escape);
        case 8:    return static_cast<u32>(ic::key_backspace);
        case 33:   return static_cast<u32>(ic::key_page_up);
        case 34:   return static_cast<u32>(ic::key_page_down);
        case 36:   return static_cast<u32>(ic::key_home);
        case 35:   return static_cast<u32>(ic::key_end);
        case 45:   return static_cast<u32>(ic::key_insert);
        case 46:   return static_cast<u32>(ic::key_delete);
        case 188:  return static_cast<u32>(ic::key_comma);       // ,
        case 190:  return static_cast<u32>(ic::key_period);      // .
        case 189:  return static_cast<u32>(ic::key_minus);       // -
        case 187:  return static_cast<u32>(ic::key_plus);        // = / +
        case 219:  return static_cast<u32>(ic::key_bracket_open);// [
        case 221:  return static_cast<u32>(ic::key_brack_close); // ]
        case 186:  return static_cast<u32>(ic::key_colon);       // ; / :
        case 191:  return static_cast<u32>(ic::key_question);    // / / ?
        default:   return u32_invalid_id;
    }
}

void push_key_event(int code, bool down) {
    if (code >= 0 && code < 512) keyState[code] = down;
    const u32 ic_code = emscripten_key_to_input_code(code);
    if (ic_code == u32_invalid_id) return;
    // current.x in [0,1]: 1.f while held, 0.f on release. Input.cpp's set()
    // already swaps previous/current; we just supply new current.
    const float v = down ? 1.0f : 0.0f;
    primal::math::v3 value{ v, 0.0f, 0.0f };
    primal::input::set(primal::input::input_source::keyboard,
                       static_cast<primal::input::input_code::code>(ic_code),
                       value);
}

EM_BOOL keydown_callback(int, const EmscriptenKeyboardEvent* e, void*) {
    push_key_event(e->keyCode, true);
    return EM_TRUE;
}

EM_BOOL keyup_callback(int, const EmscriptenKeyboardEvent* e, void*) {
    push_key_event(e->keyCode, false);
    return EM_TRUE;
}

EM_BOOL mousemove_callback(int, const EmscriptenMouseEvent* e, void*) {
    mouseDX += static_cast<float>(e->movementX);
    mouseDY += static_cast<float>(e->movementY);
    mouseX = static_cast<float>(e->targetX);
    mouseY = static_cast<float>(e->targetY);
    return EM_TRUE;
}

EM_BOOL mousedown_callback(int, const EmscriptenMouseEvent* e, void*) {
    if (e->button >= 0 && e->button < 3) mouseButtons[e->button] = true;
    return EM_TRUE;
}

EM_BOOL mouseup_callback(int, const EmscriptenMouseEvent* e, void*) {
    if (e->button >= 0 && e->button < 3) mouseButtons[e->button] = false;
    return EM_TRUE;
}

bool inputInitialized = false;

} // anonymous namespace

extern "C" {

void EmscriptenInitInput() {
    if (inputInitialized) return;
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, EM_TRUE, keydown_callback);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, nullptr, EM_TRUE, keyup_callback);
    emscripten_set_mousemove_callback("#canvas", nullptr, EM_TRUE, mousemove_callback);
    emscripten_set_mousedown_callback("#canvas", nullptr, EM_TRUE, mousedown_callback);
    emscripten_set_mouseup_callback("#canvas", nullptr, EM_TRUE, mouseup_callback);
    inputInitialized = true;
}

bool EmscriptenGetKeyState(int keyCode) {
    if (keyCode >= 0 && keyCode < 512) return keyState[keyCode];
    return false;
}

bool EmscriptenGetMouseButton(int button) {
    if (button >= 0 && button < 3) return mouseButtons[button];
    return false;
}

void EmscriptenGetMouseDelta(float* dx, float* dy) {
    *dx = mouseDX;
    *dy = mouseDY;
    // Always clear deltas to prevent accumulation when mouse button is not held
    mouseDX = 0;
    mouseDY = 0;
}

void EmscriptenGetMousePosition(float* x, float* y) {
    *x = mouseX;
    *y = mouseY;
}

} // extern "C"

#endif // __EMSCRIPTEN__
