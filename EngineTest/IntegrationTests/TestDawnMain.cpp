#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "TestDawnForwardRenderer.h"
#include <iostream>

#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include "TestDawnSponza.h"
#include <cstdlib>

extern "C" void EmscriptenInitInput();

static DawnSponzaRenderer* referenceRenderer = nullptr;
bool SetReferenceSunDirection(float yaw, float pitch) {
    if (!referenceRenderer) return false;
    referenceRenderer->SetSunDirection(yaw, pitch);
    return true;
}

static Engine_Test* g_test = nullptr;

void main_loop() {
    if (g_test) g_test->RenderFrame();
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    std::cout << "=== Dawn WebGPU WASM ===" << std::endl;

    if (EM_ASM_INT({ return new URLSearchParams(location.search).get("mode") === "11"; })) {
        static DawnSponzaRenderer renderer;
        setenv("PRIMAL_DAWN_SCREEN_PROBES_STAGE1", "1", 1);
        setenv("PRIMAL_DAWN_REFERENCE_RESOLUTION", "1", 1);
        setenv("PRIMAL_TEST_MODEL", "SponzaTangentFrames.model", 1);
        if (!renderer.initialize()) {
            std::cerr << "Failed to initialize Mode 11" << std::endl;
            return 1;
        }
        referenceRenderer = &renderer;
        EmscriptenInitInput();
        EM_ASM({
            window.setRenderMode?.(11, "Dynamic GI", "Sponza");
            window.hideLoading?.();
        });
        emscripten_set_main_loop([] { renderer.run(); }, 0, true);
    }
    setenv("PRIMAL_DAWN_FORWARD_SHOWCASE", "1", 1);
    static Engine_Test test;
    g_test = &test;
    if (!test.initialize()) {
        std::cerr << "Failed to initialize" << std::endl;
        return 1;
    }
    EmscriptenInitInput();
    emscripten_run_script("if(window.hideLoading) window.hideLoading();");
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
