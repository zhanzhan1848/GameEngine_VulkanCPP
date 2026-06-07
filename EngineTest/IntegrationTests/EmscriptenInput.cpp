#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#include <cstring>

namespace {

bool keyState[512] = {};
bool mouseButtons[3] = {};
float mouseX = 0, mouseY = 0;
float mouseDX = 0, mouseDY = 0;

EM_BOOL keydown_callback(int, const EmscriptenKeyboardEvent* e, void*) {
    int code = e->keyCode;
    if (code >= 0 && code < 512) keyState[code] = true;
    return EM_TRUE;
}

EM_BOOL keyup_callback(int, const EmscriptenKeyboardEvent* e, void*) {
    int code = e->keyCode;
    if (code >= 0 && code < 512) keyState[code] = false;
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
