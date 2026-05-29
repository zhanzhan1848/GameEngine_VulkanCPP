// Copyright (c) Contributors of Primal+
// Distributed under the MIT license. See the LICENSE file in the project root for more information.
// Test selection is controlled via CMake compile definitions
// Do not add hardcoded #define here - use CMakeLists.txt to configure
#include "Test.h"

#if defined(_MSC_VER)
#pragma comment(lib, "Engine.lib")
#endif

#if TEST_ENTITY_COMPONENTS
#include "TestEntityComponents.h"
#elif TEST_WINDOW
#include "TestWindow.h"
#elif TEST_STANDARD_PIPELINE
#include "TestStandardPipeline.h"
#elif defined(TEST_MODULAR_PIPELINE)
#include "TestModularPipeline.h"
#elif TEST_RENDERER
#include "TestRenderer.h"
#elif TEST_CSM_INTEGRATION
#include "TestCSMIntegration.h"
#elif TEST_CSM_RENDERGRAPH
#include "TestCSMIntegrationRenderGraph.h"
#elif TEST_MULTIVIEW
#include "TestMultiView.h"
#elif defined(TEST_SPONZA_RENDERGRAPH)
#include "TestSponzaRenderGraph.h"
#elif defined(TEST_GEOMETRY_DEBUG_SPONZA)
#include "TestGeometryDebugSponza.h"
#elif defined(TEST_PARTICLE_SPONZA)
#include "TestParticleSponza.h"
#elif defined(TEST_NANITE_STREAMING_PIPELINE)
#include "TestNaniteStreamingPipeline.h"
#else
#error One of the tests need to be enabled - check CMakeLists.txt compile definitions
#endif

#ifdef _WIN64
#include <Windows.h>
#include <filesystem>

// TODO: duplicate!
std::filesystem::path
set_current_directory_to_executable_path()
{
    // set the working directory to the executable path
    wchar_t path[MAX_PATH];
    const uint32_t length{ GetModuleFileName(0, &path[0], MAX_PATH) };
    if (!length || GetLastError() == ERROR_INSUFFICIENT_BUFFER) return {};
    std::filesystem::path p{ path };
    std::filesystem::current_path(p.parent_path());
    return std::filesystem::current_path();
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
#if _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    //set_current_directory_to_executable_path();
    Engine_Test test{};
    if (test.initialize())
    {
        MSG msg{};
        bool is_running{ true };
        while (is_running)
        {
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                is_running &= (msg.message != WM_QUIT);
            }

            test.run();
        }
    }
    test.shutdown();
    return 0;
}

#elif __linux__
#include <X11/Xlib.h>

int main(int argc, char* argv[])
{
    XInitThreads();

    engine_test test{};

    // Open an X server connection
    Display* display{ XOpenDisplay(NULL) };
    if (display == NULL) return 1;

    // Set up custom client messages
    Atom wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", false);
    Atom quit_msg = XInternAtom(display, "QUIT_MSG", false);

    if (test.initialize(display))
    {
        XEvent xev;
        bool is_running{ true };
        while (is_running)
        {
            // NOTE: we use an if statement here because we are not handling all events in this translation
            //       unit, so XPending(display) will often not ever be 0, and therefore this can create
            //       an infinite loop... but this protects XNextEvent from blocking if there are no events.
            if (XPending(display) > 0)
            {
                XNextEvent(display, &xev);

                switch (xev.type)
                {
                case KeyPress:

                    break;
                case ClientMessage:
                    if ((Atom)xev.xclient.data.l[0] == wm_delete_window)
                    {
                        // Dont handle this here
                        XPutBackEvent(display, &xev);
                    }
                    if ((Atom)xev.xclient.data.l[0] == quit_msg)
                    {
                        is_running = false;
                    }
                    break;
                }
            }
            test.run(display);
        }
        test.shutdown();
        XCloseDisplay(display);
        return 0;
    }
}

#elif __APPLE__
#include <iostream>

#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <AppKit/AppKit.hpp>
#include "MacKeyboard.h"

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    std::cout << "Hello, World!" << std::endl;
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    try
    {
        Engine_Test test{};
        NS::Application* app = NS::Application::sharedApplication();
        monitorKeyboardInput();
        app->setDelegate(&test);
        app->run();
    }
    catch(const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    
    pool->release();
    return 0;
}
#endif // platforms