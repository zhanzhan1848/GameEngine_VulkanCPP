// TestMiscRenderingAPIs.cpp - Phase 5 C ABI round-trip tests for
// DebugDrawAPI, RenderTargetAPI, and PCGAPI.
//
// These three APIs don't fit cleanly into TestLightCameraAPI (ECS component
// round-trip) or TestRenderPipelineAPI (pipeline introspection), so they get
// their own combined file:
//
//   DebugDrawAPI   — line queue CRUD via the engine singleton.
//   RenderTargetAPI — defensive cases (no RHI device in test env → valid calls
//                     still fail predictably; invalid args fail).
//   PCGAPI         — graph lifecycle, node CRUD, param reflection.
//
// All tests are headless: no GPU, no surface, no window.

#include "../UnitTests/TestFramework.h"
#include "Engine/Common/PrimitiveTypes.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using Engine::Test::TestCase;

// PCGAddNode returns u32(-1) on failure (not 0). The first successfully added
// node gets ID 0 — so we must compare against the sentinel, not zero.
constexpr u32 INVALID_NODE_ID = ~0u;

// === EngineDLL C ABI declarations ===========================================

extern "C" {
// --- DebugDrawAPI ---
void DrawDebugLine(f32 x0, f32 y0, f32 z0,
                   f32 x1, f32 y1, f32 z1, u32 rgb);
void DrawDebugBoxWireframe(f32 cx, f32 cy, f32 cz,
                           f32 ex, f32 ey, f32 ez, u32 rgb);
void DrawDebugSphereWireframe(f32 cx, f32 cy, f32 cz, f32 radius, u32 segments, u32 rgb);
void DrawDebugFrustum(const f32* view_proj_4x4_row_major, u32 rgb);
u32  GetDebugLineCount();
u32  GetQueuedDebugLines(f32* out_vertices, u32* out_rgb, u32 max_lines);
void ClearDebugLines();
void FlushDebugDraw();

// --- RenderTargetAPI ---
u64  CreateRenderTarget(u32 width, u32 height, u32 format);
void DestroyRenderTarget(u64 handle);

// --- PCGAPI ---
void PCGCreateGraph();
void PCGDestroyGraph();
u32  PCGAddNode(const char* type_name);
void PCGRemoveNode(u32 node_id);
u32  PCGGetNodeCount();
const char* PCGGetNodeTypeName(u32 node_id);
void PCGConnect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin);
void PCGDisconnect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin);
u32  PCGGetNodeParamCount(u32 node_id);
u32  PCGSetNodeParamFloat(u32 node_id, const char* name, f32 value);
}

// ============================================================================
// DebugDrawAPI tests
// ============================================================================

TestResult TestDebugDrawLineRoundTrip() {
    ClearDebugLines();
    TEST_ASSERT_EQ(GetDebugLineCount(), 0u, "Queue empty after clear");

    DrawDebugLine(0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0xFFFFFFu);
    DrawDebugLine(1.f, 0.f, 0.f, 2.f, 0.f, 0.f, 0xFF0000u);
    TEST_ASSERT_EQ(GetDebugLineCount(), 2u, "Count after 2 DrawDebugLine");

    std::vector<f32> verts(2 * 6, 0.f);
    std::vector<u32> rgbs(2, 0);
    u32 drained = GetQueuedDebugLines(verts.data(), rgbs.data(), 2u);
    TEST_ASSERT_EQ(drained, 2u, "Drained 2 lines");
    // First line endpoints
    TEST_ASSERT(std::abs(verts[0] - 0.f) < 0.0001f, "Line 0 ax");
    TEST_ASSERT(std::abs(verts[3] - 1.f) < 0.0001f, "Line 0 bx");
    TEST_ASSERT_EQ(rgbs[0], 0xFFFFFFu, "Line 0 color");
    // GetQueuedDebugLines drains, so the queue is now empty
    TEST_ASSERT_EQ(GetDebugLineCount(), 0u, "Queue drained after Get");

    ClearDebugLines();
    return TestResult::Passed;
}

TestResult TestDebugDrawBoxWireframe() {
    ClearDebugLines();
    DrawDebugBoxWireframe(0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0x00FF00u);
    // A box has 12 edges.
    TEST_ASSERT_EQ(GetDebugLineCount(), 12u, "Box wireframe = 12 lines");
    ClearDebugLines();
    return TestResult::Passed;
}

TestResult TestDebugDrawSphereWireframe() {
    ClearDebugLines();
    // Sphere with N segments produces N*(N-1) latitude + N*N longitude lines.
    const u32 N = 8;
    DrawDebugSphereWireframe(0.f, 0.f, 0.f, 1.0f, N, 0u);
    const u32 expected = N * (N - 1) + N * N;
    TEST_ASSERT_EQ(GetDebugLineCount(), expected, "Sphere line count matches formula");
    ClearDebugLines();
    return TestResult::Passed;
}

TestResult TestDebugDrawQueueCapable() {
    // Asking to drain more than what's queued returns only what's queued.
    ClearDebugLines();
    DrawDebugLine(0, 0, 0, 1, 1, 1, 0);
    DrawDebugLine(0, 0, 0, 1, 1, 1, 0);

    std::vector<f32> verts(100 * 6, 0.f);
    std::vector<u32> rgbs(100, 0);
    u32 drained = GetQueuedDebugLines(verts.data(), rgbs.data(), 100u);
    TEST_ASSERT_EQ(drained, 2u, "Drain capped by queue, not max_lines");
    ClearDebugLines();
    return TestResult::Passed;
}

TestResult TestDebugDrawFrustumNullSafe() {
    // DrawDebugFrustum with null must not crash (defensive check).
    DrawDebugFrustum(nullptr, 0xFFFFFFu);
    // (No assert — reaching here means no crash.)
    return TestResult::Passed;
}

// ============================================================================
// RenderTargetAPI tests (no RHI device available in test env)
// ============================================================================

TestResult TestRenderTargetInvalidArgs() {
    // Zero dimensions → invalid.
    TEST_ASSERT_EQ(CreateRenderTarget(0, 0, 0), 0ull, "Zero dimensions");
    TEST_ASSERT_EQ(CreateRenderTarget(0, 480, 0), 0ull, "Zero width");
    TEST_ASSERT_EQ(CreateRenderTarget(640, 0, 0), 0ull, "Zero height");
    // Unknown format (4 is past the last enum) → invalid.
    TEST_ASSERT_EQ(CreateRenderTarget(640, 480, 99u), 0ull, "Unknown format");
    return TestResult::Passed;
}

TestResult TestRenderTargetRequiresDevice() {
    // In test env, RHI device is not initialized. CreateRenderTarget should
    // fail with a valid argument set because device lookup returns null.
    u64 h = CreateRenderTarget(640, 480, 0u);
    TEST_ASSERT_EQ(h, 0ull, "CreateRenderTarget fails without RHI device");
    return TestResult::Passed;
}

TestResult TestRenderTargetDestroySafety() {
    // Destroy calls with bogus handles must be safe no-ops.
    DestroyRenderTarget(0);
    DestroyRenderTarget(99);
    DestroyRenderTarget(0xDEADBEEFULL);
    return TestResult::Passed;
}

// ============================================================================
// PCGAPI tests (full graph lifecycle, no GPU required)
// ============================================================================

TestResult TestPCGGraphLifecycle() {
    PCGCreateGraph();
    TEST_ASSERT_EQ(PCGGetNodeCount(), 0u, "Empty graph after create");

    PCGDestroyGraph();
    TEST_ASSERT_EQ(PCGGetNodeCount(), 0u, "GetNodeCount on null graph returns 0");

    PCGCreateGraph();
    TEST_ASSERT_EQ(PCGGetNodeCount(), 0u, "Fresh graph empty");

    PCGDestroyGraph();
    return TestResult::Passed;
}

TestResult TestPCGAddNodesByType() {
    PCGCreateGraph();

    u32 n1 = PCGAddNode("ReferenceField");
    u32 n2 = PCGAddNode("NoiseField");
    u32 n3 = PCGAddNode("FieldScatter");
    TEST_ASSERT(n1 != INVALID_NODE_ID, "ReferenceField add succeeded");
    TEST_ASSERT(n2 != INVALID_NODE_ID, "NoiseField add succeeded");
    TEST_ASSERT(n3 != INVALID_NODE_ID, "FieldScatter add succeeded");
    TEST_ASSERT_EQ(PCGGetNodeCount(), 3u, "3 nodes after 3 adds");

    const char* t1 = PCGGetNodeTypeName(n1);
    const char* t2 = PCGGetNodeTypeName(n2);
    const char* t3 = PCGGetNodeTypeName(n3);
    TEST_ASSERT(t1 && std::string(t1) == "ReferenceField", "Type name 1");
    TEST_ASSERT(t2 && std::string(t2) == "NoiseField",     "Type name 2");
    TEST_ASSERT(t3 && std::string(t3) == "FieldScatter",   "Type name 3");

    // Unknown type name — AddNode should return the invalid sentinel.
    u32 bad = PCGAddNode("DefinitelyNotANode");
    TEST_ASSERT_EQ(bad, INVALID_NODE_ID, "Unknown type returns invalid sentinel");

    PCGDestroyGraph();
    return TestResult::Passed;
}

TestResult TestPCGNodeParamReflection() {
    PCGCreateGraph();
    u32 n = PCGAddNode("NoiseField");
    TEST_ASSERT(n != INVALID_NODE_ID, "NoiseField added");

    // NoiseField should expose at least one parameter.
    u32 param_count = PCGGetNodeParamCount(n);
    TEST_ASSERT(param_count > 0, "NoiseField has params");

    // PCGSetNodeParamFloat round-trip on a known param ("frequency").
    // We can't easily read back without a getter, so just verify the call
    // succeeds (returns 1) for a valid param name and fails (returns 0) for
    // an unknown name.
    u32 ok = PCGSetNodeParamFloat(n, "frequency", 0.07f);
    TEST_ASSERT_EQ(ok, 1u, "SetNodeParamFloat on known name succeeds");
    u32 bad = PCGSetNodeParamFloat(n, "definitely_not_a_param", 1.0f);
    TEST_ASSERT_EQ(bad, 0u, "SetNodeParamFloat on unknown name fails");

    // Operating on invalid node id is a safe no-op.
    u32 bad_node = PCGSetNodeParamFloat(9999u, "frequency", 1.0f);
    TEST_ASSERT_EQ(bad_node, 0u, "SetNodeParamFloat on invalid node fails");

    PCGDestroyGraph();
    return TestResult::Passed;
}

TestResult TestPCGConnectDisconnect() {
    PCGCreateGraph();
    u32 noise = PCGAddNode("NoiseField");
    u32 scatter = PCGAddNode("FieldScatter");
    TEST_ASSERT(noise != INVALID_NODE_ID && scatter != INVALID_NODE_ID, "Both nodes added");

    // Connect output pin 0 of NoiseField → input pin 0 of FieldScatter.
    // We don't assert success (pin index validity depends on internal schema),
    // just verify the call doesn't crash and the graph still has 2 nodes.
    PCGConnect(noise, 0, scatter, 0);
    TEST_ASSERT_EQ(PCGGetNodeCount(), 2u, "Node count unchanged after connect");

    // Disconnect same.
    PCGDisconnect(noise, 0, scatter, 0);
    TEST_ASSERT_EQ(PCGGetNodeCount(), 2u, "Node count unchanged after disconnect");

    PCGDestroyGraph();
    return TestResult::Passed;
}

TestResult TestPCGRemoveNode() {
    PCGCreateGraph();
    // Add 3 nodes — indices 0, 1, 2. PCGRemoveNode shifts remaining indices
    // down, so removing an in-range index always removes a real node.
    (void)PCGAddNode("NoiseField");
    (void)PCGAddNode("FieldScatter");
    (void)PCGAddNode("ReferenceField");
    TEST_ASSERT_EQ(PCGGetNodeCount(), 3u, "3 after add");

    // Remove index 1 — count drops, remaining indices shift down.
    PCGRemoveNode(1u);
    TEST_ASSERT_EQ(PCGGetNodeCount(), 2u, "2 after remove");

    // Out-of-range index is a safe no-op (graph still has 2 nodes).
    PCGRemoveNode(9999u);
    TEST_ASSERT_EQ(PCGGetNodeCount(), 2u, "Still 2 after bogus remove");

    // Type-name lookup on out-of-range index returns null.
    const char* t = PCGGetNodeTypeName(9999u);
    TEST_ASSERT(t == nullptr, "TypeName of out-of-range index is null");

    PCGDestroyGraph();
    return TestResult::Passed;
}

// ============================================================================
// Runner
// ============================================================================

void RunDebugDrawTests() {
    TestSuite suite("DebugDraw API C ABI Tests (Phase 5)");
    suite.AddTestCase(TestCase("DrawDebugLine round-trip",
        TestDebugDrawLineRoundTrip,
        "Draw → count → drain → verify"));
    suite.AddTestCase(TestCase("DrawDebugBoxWireframe = 12 lines",
        TestDebugDrawBoxWireframe,
        "Box produces exactly 12 edges"));
    suite.AddTestCase(TestCase("DrawDebugSphereWireframe formula",
        TestDebugDrawSphereWireframe,
        "Sphere line count matches N*(N-1) + N*N"));
    suite.AddTestCase(TestCase("Drain capped by queue size",
        TestDebugDrawQueueCapable,
        "max_lines > queue returns only queue"));
    suite.AddTestCase(TestCase("DrawDebugFrustum null safety",
        TestDebugDrawFrustumNullSafe,
        "Null VP pointer does not crash"));
    suite.RunAllTests();
}

void RunRenderTargetTests() {
    TestSuite suite("RenderTarget API C ABI Tests (Phase 5)");
    suite.AddTestCase(TestCase("Invalid args rejected",
        TestRenderTargetInvalidArgs,
        "Zero dims and unknown format return 0"));
    suite.AddTestCase(TestCase("Requires RHI device",
        TestRenderTargetRequiresDevice,
        "Valid args fail without device (test env)"));
    suite.AddTestCase(TestCase("Destroy bogus handles safe",
        TestRenderTargetDestroySafety,
        "DestroyRenderTarget(0/99/...) does not crash"));
    suite.RunAllTests();
}

void RunPCGTests() {
    TestSuite suite("PCG API C ABI Tests (Phase 5)");
    suite.AddTestCase(TestCase("Graph lifecycle",
        TestPCGGraphLifecycle,
        "Create/Destroy round-trip; GetNodeCount=0"));
    suite.AddTestCase(TestCase("Add nodes by type name",
        TestPCGAddNodesByType,
        "ReferenceField/NoiseField/FieldScatter add & reflect type name"));
    suite.AddTestCase(TestCase("Node param reflection",
        TestPCGNodeParamReflection,
        "Param count > 0; SetNodeParamFloat valid/invalid name + invalid node"));
    suite.AddTestCase(TestCase("Connect/Disconnect",
        TestPCGConnectDisconnect,
        "Pin connect/disconnect don't crash; node count stable"));
    suite.AddTestCase(TestCase("Remove node",
        TestPCGRemoveNode,
        "Remove decrements; out-of-range index and TypeName lookup are safe"));
    suite.RunAllTests();
}

int main() {
    RunDebugDrawTests();
    RunRenderTargetTests();
    RunPCGTests();
    return 0;
}
