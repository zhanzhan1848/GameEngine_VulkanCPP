#if defined(ENABLE_WEBGPU) && ENABLE_WEBGPU

#include "Graphics/WFC/TestKenneyTilePreview.h"
#include "Engine/Graphics/WFC/IWFCTileStyle.h"
#include "Engine/Graphics/WFC/WFCTileStyleRegistry.h"
#include <iostream>
#include <vector>

#ifdef __APPLE__
#include <AppKit/AppKit.hpp>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

extern "C" void EmscriptenInitInput();

static Engine_Test* g_test = nullptr;

void main_loop() {
    if (g_test) g_test->run();
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    std::cout << "=== WFC Showcase (Kenney Tiles) — Dawn WebGPU WASM ===" << std::endl;

    Engine_Test test;
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

// ============================================================
// WASM↔JS bridge — exports invoked from the debug panel (shell.html).
// Each one latches a "pending" value; the test case drains them at the
// top of the next frame inside HandleGridEditKeys(). The singleton
// pointer is set by KenneyTilePreviewTestCase::Initialize() — if JS
// calls these before init or after shutdown, they no-op.
// ============================================================
extern "C" {

EMSCRIPTEN_KEEPALIVE void wfc_set_observer(int kind) {
    if (auto* t = KenneyTilePreviewTestCase::Instance()) t->RequestObserver(static_cast<u32>(kind));
}

EMSCRIPTEN_KEEPALIVE void wfc_set_origin(int preset) {
    if (auto* t = KenneyTilePreviewTestCase::Instance()) t->RequestOrigin(static_cast<u32>(preset));
}

EMSCRIPTEN_KEEPALIVE void wfc_set_grid_w(int w) {
    if (auto* t = KenneyTilePreviewTestCase::Instance()) t->RequestGridW(static_cast<u32>(w));
}

EMSCRIPTEN_KEEPALIVE void wfc_set_grid_h(int h) {
    if (auto* t = KenneyTilePreviewTestCase::Instance()) t->RequestGridH(static_cast<u32>(h));
}

EMSCRIPTEN_KEEPALIVE void wfc_set_grid_d(int d) {
    if (auto* t = KenneyTilePreviewTestCase::Instance()) t->RequestGridD(static_cast<u32>(d));
}

EMSCRIPTEN_KEEPALIVE void wfc_reseed_same() {
    if (auto* t = KenneyTilePreviewTestCase::Instance()) t->RequestReseedSame();
}

EMSCRIPTEN_KEEPALIVE void wfc_reseed_new() {
    if (auto* t = KenneyTilePreviewTestCase::Instance()) t->RequestReseedNew();
}

// Phase C.1 Task 16: IWFCTileStyle composition. Replace the prior
// wfc_set_solve_mode / wfc_get_solve_mode pair (binary KenneyOnly ↔
// MixedMulti toggle) with a registry-aware API. The dropdown in shell.html
// auto-populates from wfc_get_style_count + wfc_get_style_display_name;
// the "Compose…" multi-select panel calls wfc_compose(indices, count).
// Switching composition rebuilds registry+adjacency from scratch via
// WFCTileStyleRegistry::Compose — heavier than a reseed.
EMSCRIPTEN_KEEPALIVE int wfc_get_style_count() {
    return static_cast<int>(
        primal::graphics::wfc::WFCTileStyleRegistry::Instance().GetStyleCount());
}

EMSCRIPTEN_KEEPALIVE const char* wfc_get_style_name(int idx) {
    auto* s = primal::graphics::wfc::WFCTileStyleRegistry::Instance().GetStyle(
        static_cast<u32>(idx));
    return s ? s->GetName() : "";
}

EMSCRIPTEN_KEEPALIVE const char* wfc_get_style_display_name(int idx) {
    auto* s = primal::graphics::wfc::WFCTileStyleRegistry::Instance().GetStyle(
        static_cast<u32>(idx));
    return s ? s->GetDisplayName() : "";
}

EMSCRIPTEN_KEEPALIVE int wfc_get_active_style_count() {
    auto* t = KenneyTilePreviewTestCase::Instance();
    return t ? static_cast<int>(t->GetActiveStyles().size()) : 0;
}

EMSCRIPTEN_KEEPALIVE int wfc_get_active_style(int idx) {
    auto* t = KenneyTilePreviewTestCase::Instance();
    if (!t) return -1;
    const auto& v = t->GetActiveStyles();
    if (idx < 0 || static_cast<u32>(idx) >= v.size()) return -1;
    return static_cast<int>(v[static_cast<u32>(idx)]);
}

// Compose a subset of registered styles. `indices` points to a heap u32
// array of length `count` (JS allocates via _malloc). Empty selection is
// a no-op. On success the test case latches a pending composition that
// HandleGridEditKeys drains next frame.
EMSCRIPTEN_KEEPALIVE int wfc_compose(const int* indices, int count) {
    if (!indices || count <= 0) return 0;
    auto* t = KenneyTilePreviewTestCase::Instance();
    if (!t) return 0;
    std::vector<u32> v;
    v.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        v.push_back(static_cast<u32>(indices[i]));
    }
    t->RequestCompose(std::move(v));
    return 1;
}

EMSCRIPTEN_KEEPALIVE int wfc_get_observer() {
    auto* t = KenneyTilePreviewTestCase::Instance();
    return t ? static_cast<int>(t->GetObserverKind()) : 0;
}

EMSCRIPTEN_KEEPALIVE int wfc_get_origin() {
    auto* t = KenneyTilePreviewTestCase::Instance();
    return t ? static_cast<int>(t->GetOriginPreset()) : 0;
}

EMSCRIPTEN_KEEPALIVE int wfc_get_grid_w() {
    auto* t = KenneyTilePreviewTestCase::Instance();
    return t ? static_cast<int>(t->GetGridW()) : 0;
}

EMSCRIPTEN_KEEPALIVE int wfc_get_grid_h() {
    auto* t = KenneyTilePreviewTestCase::Instance();
    return t ? static_cast<int>(t->GetGridH()) : 0;
}

EMSCRIPTEN_KEEPALIVE int wfc_get_grid_d() {
    auto* t = KenneyTilePreviewTestCase::Instance();
    return t ? static_cast<int>(t->GetGridD()) : 0;
}

} // extern "C"

#else // !__EMSCRIPTEN__

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    std::cout << "=== WFC Showcase (Kenney Tiles) — Dawn WebGPU Native ===" << std::endl;

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
        std::cerr << "Failed to initialize" << std::endl;
        test.shutdown();
        return 1;
    }
    test.run();
    test.shutdown();
#endif

    std::cout << "=== WFC Showcase Complete ===" << std::endl;
    return 0;
}

#endif // __EMSCRIPTEN__

#else // !ENABLE_WEBGPU
int main() { return 0; }
#endif
