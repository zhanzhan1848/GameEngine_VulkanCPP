#pragma once

// TestWFCStreaming.h — WFC Phase B.2 Task 4: streaming invariant test.
//
// Declares Engine_Test for Main.cpp. The test case class
// (WFCStreamingTestCase) and its implementation live entirely in
// TestWFCStreaming.cpp — this header only exists so Main.cpp's
// #elif TEST_WFC_STREAMING branch can construct Engine_Test.
//
// Mirrors TestWFCRendering.h's role as the Main.cpp entry point, but keeps
// the test case class in the .cpp (the plan's single-file intent).

#include "RenderTestFramework.h"
#include <memory>

class Engine_Test : public primal::test::RenderTestRunner {
public:
    Engine_Test();
};
