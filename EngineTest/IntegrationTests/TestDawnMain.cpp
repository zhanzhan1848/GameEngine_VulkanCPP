#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "TestDawnForwardRenderer.h"
#include <iostream>

#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

extern "C" void EmscriptenInitInput();

static Engine_Test* g_test = nullptr;

void main_loop() {
    if (g_test) g_test->RenderFrame();
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    std::cout << "=== Dawn WebGPU WASM ===" << std::endl;

    Engine_Test test;
    g_test = &test;
    if (!test.initialize()) {
        std::cerr << "Failed to initialize" << std::endl;
        return 1;
    }
    EmscriptenInitInput();
    emscripten_set_main_loop(main_loop, 0, 1);
    test.shutdown();
    return 0;
}

#else // !__EMSCRIPTEN__

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    std::cout << "=== Dawn WebGPU RHI Backend Tests ===" << std::endl;

#ifdef __APPLE__
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    Engine_Test test{};
    NS::Application* app = NS::Application::sharedApplication();
    app->setDelegate(&test);
    app->run();
    pool->release();
#else
    Engine_Test test{};
    if (!test.initialize()) {
        std::cerr << "Failed to initialize Dawn test suite" << std::endl;
        test.shutdown();
        return 1;
    }
    test.run();
    test.shutdown();
#endif

    std::cout << "=== Dawn WebGPU Tests Complete ===" << std::endl;
    return 0;
}

#endif // __EMSCRIPTEN__

#endif // ENABLE_WEBGPU
