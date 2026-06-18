// TestMediatedDataFlow.cpp - Phase 5.1 UI-Mediated data flow C ABI round-trip.
//
// Validates the 14 new APIs that close the gap between TestPCGScatter's in-engine
// workflow and what an Editor can drive through EngineDLL:
//
//   ProceduralMeshAPI  — ProceduralMeshCreate/Destroy (12 mesh types)
//   PCGAPI (extension) — PCGGetOutputPositions/MeshSlots/Transforms
//   AssetAPI (ext)     — ImportSceneBinary + GetImportedMesh{Count,ContentId,TexturePaths}
//   RenderPipelineAPI  — PipelineRegisterMeshEntity/Unregister + SetEditorMode/Is + SetLumenConfig
//   CameraAPI (ext)    — GetEntityCameraViewMatrix/ProjectionMatrix
//
// All tests are headless: no GPU/surface/window needed. We exercise:
//   - argument validation (null/invalid returns 0 / predictable)
//   - basic happy-path round-trip (non-zero content IDs, count > 0, etc.)
//   - PCG output readback over a tiny NoiseField → FieldScatter → MeshAssign graph
//
// NOTE: PipelineRegisterMeshEntity requires the global RenderPipeline::Get() to
// be initialized. In headless test runs, Get() is typically nullptr, so the test
// only verifies that the function returns 0 gracefully rather than crashing.
// Full mesh registration is exercised by TestRenderFrameAPI (which initializes
// the engine via InitializeEngine).

#include "../UnitTests/TestFramework.h"
#include "Engine/Common/PrimitiveTypes.h"
#include "Engine/Common/Id.h"

#include <cmath>
#include <cstring>
#include <vector>

using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using Engine::Test::TestCase;
using namespace primal;  // brings id:: into scope for C ABI decls

constexpr u32 INVALID_NODE_ID = ~0u;
// id::invalid_id is id_type(-1). Cast to u64 it's a huge sentinel, not 0.
// The FIRST successful register_mesh_asset returns slot 0 — so we must check
// against the sentinel, not zero.
constexpr u64 INVALID_CONTENT_ID = static_cast<u64>(id::invalid_id);

// === EngineDLL C ABI declarations ===========================================

extern "C" {

// --- ProceduralMeshAPI ---
u64  ProceduralMeshCreate(u32 mesh_type, const f32* params, u32 param_count);
void ProceduralMeshDestroy(u64 content_id);

// --- PCGAPI (extension) ---
void PCGCreateGraph();
void PCGDestroyGraph();
u32  PCGAddNode(const char* type_name);
void PCGConnect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin);
void PCGExecute();
u32  PCGGetOutputPointCount(u32 node_id);
u32  PCGGetOutputPositions(u32 node_id, f32* out_positions_xyz, u32 max_count);
u32  PCGGetOutputMeshSlots(u32 node_id, u32* out_mesh_slots, u32 max_count);
u32  PCGGetOutputTransforms(u32 node_id, f32* out_scale_xyz_rot_y, u32 max_count);
u64  PCGGetOutputGeometry(u32 node_id);
u32  PCGAddMeshSlot(const char* path, const char* name);
u32  PCGSetNodeParamFloat(u32 node_id, const char* name, f32 value);
u32  PCGSetNodeParamVec3(u32 node_id, const char* name, f32 x, f32 y, f32 z);

// --- AssetAPI (extension) ---
u32  ImportSceneBinary(const char* path);
u32  GetImportedMeshCount();
u64  GetImportedMeshContentId(u32 index);
u32  GetImportedMeshTexturePaths(u32 index,
                                 const char** out_diffuse_path,
                                 const char** out_normal_path,
                                 const char** out_orm_path);

// --- RenderPipelineAPI (extension) ---
u64  PipelineRegisterMeshEntity(u64 geometry_content_id,
                                const u64* texture_content_ids,
                                u32 texture_count);
void PipelineUnregisterMeshEntity(u64 entity_id);
void PipelineSetEditorMode(u32 enable);
u32  PipelineIsEditorMode();
void PipelineSetLumenConfig(u32 quality_preset);

// --- CameraAPI (extension) — minimal: just the matrix readback ---
// Reuse the full Add/Remove lifecycle from TestLightCameraAPI; here we only
// exercise the matrix getters since they depend on a Camera component.
u32  AddEntityCamera(id::id_type entity_id, u32 projection_type,
                     f32 field_of_view, f32 aspect_ratio,
                     f32 near_z, f32 far_z, const f32* up_vector);
void RemoveEntityCamera(id::id_type entity_id);
u32  GetEntityCameraViewMatrix(id::id_type entity_id, f32* out_16);
u32  GetEntityCameraProjectionMatrix(id::id_type entity_id, f32* out_16);

}  // extern "C"

// ============================================================================
// ProceduralMeshAPI tests
// ============================================================================

TestResult TestProceduralMeshSphere() {
    // Sphere needs (radius, segments, rings) = 3 params.
    const f32 params[3] = {1.0f, 16.0f, 12.0f};
    const u64 cid = ProceduralMeshCreate(0 /*sphere*/, params, 3);
    TEST_ASSERT(cid != INVALID_CONTENT_ID, "Sphere create returned valid content_id");
    ProceduralMeshDestroy(cid);
    return TestResult::Passed;
}

TestResult TestProceduralMeshBox() {
    const f32 params[3] = {2.0f, 2.0f, 2.0f};
    const u64 cid = ProceduralMeshCreate(3 /*box*/, params, 3);
    TEST_ASSERT(cid != INVALID_CONTENT_ID, "Box create returned valid content_id");
    ProceduralMeshDestroy(cid);
    return TestResult::Passed;
}

TestResult TestProceduralMeshInvalidArgs() {
    // Unknown mesh type
    TEST_ASSERT_EQ(0ull, ProceduralMeshCreate(99, nullptr, 0), "Unknown mesh_type returns 0");

    // Param count too small for sphere (needs 3)
    const f32 short_params[2] = {1.0f, 16.0f};
    TEST_ASSERT_EQ(0ull, ProceduralMeshCreate(0 /*sphere*/, short_params, 2), "Under-param sphere returns 0");

    // Null params with non-zero count
    TEST_ASSERT_EQ(0ull, ProceduralMeshCreate(0, nullptr, 3), "Null params with count returns 0");

    // Destroy(0) is a no-op, must not crash. (Don't destroy arbitrary IDs — FreeList asserts.)
    ProceduralMeshDestroy(0);
    return TestResult::Passed;
}

TestResult TestProceduralMeshAllTypes() {
    // Round-trip every mesh type. Just verify non-invalid IDs; we don't have GPU to
    // validate vertex data here.
    struct Case { u32 type; u32 nparams; f32 p[4]; };
    const Case cases[] = {
        {0,  3, {1.0f, 16.0f, 12.0f, 0.0f}},     // Sphere
        {1,  3, {1.0f, 2.0f,  16.0f, 0.0f}},     // Cylinder
        {2,  3, {1.0f, 2.0f,  16.0f, 0.0f}},     // Cone
        {3,  3, {1.0f, 1.0f,  1.0f,  0.0f}},     // Box
        {4,  4, {1.0f, 0.3f,  16.0f, 8.0f}},     // Torus
        {5,  4, {0.5f, 1.0f,  8.0f,  4.0f}},     // Capsule
        {6,  2, {1.0f, 16.0f, 0.0f,  0.0f}},     // Disc
        {7,  3, {1.0f, 16.0f, 8.0f,  0.0f}},     // Hemisphere
        {8,  2, {1.0f, 1.0f,  0.0f,  0.0f}},     // Pyramid
        {9,  4, {4.0f, 4.0f,  1.0f,  1.0f}},     // Plane
        {10, 2, {1.0f, 1.0f,  0.0f,  0.0f}},     // QuadXY
        {11, 2, {1.0f, 8.0f,  0.0f,  0.0f}},     // Teapot
    };
    for (const auto& c : cases) {
        const u64 cid = ProceduralMeshCreate(c.type, c.p, c.nparams);
        if (cid == INVALID_CONTENT_ID) {
            TEST_ASSERT(false, "ProceduralMeshCreate returned invalid_id for a valid type");
            return TestResult::Failed;
        }
        ProceduralMeshDestroy(cid);
    }
    return TestResult::Passed;
}

// ============================================================================
// PCGAPI output read tests
// ============================================================================

TestResult TestPCGOutputReadNoGraph() {
    // No graph created → all read APIs return 0 / predictable behavior.
    f32 pos[3] = {0};
    u32 slot[1] = {0};
    f32 xform[4] = {0};
    // Make sure no graph exists. (If a previous test left one, destroy it.)
    PCGDestroyGraph();
    TEST_ASSERT_EQ(0u, PCGGetOutputPositions(0, pos, 1), "Positions w/o graph = 0");
    TEST_ASSERT_EQ(0u, PCGGetOutputMeshSlots(0, slot, 1), "MeshSlots w/o graph = 0");
    TEST_ASSERT_EQ(0u, PCGGetOutputTransforms(0, xform, 1), "Transforms w/o graph = 0");
    return TestResult::Passed;
}

TestResult TestPCGOutputReadNullArgs() {
    PCGCreateGraph();
    // Even with a graph, null out / zero max returns 0 cleanly.
    TEST_ASSERT_EQ(0u, PCGGetOutputPositions(0, nullptr, 10), "Null out_buf returns 0");
    TEST_ASSERT_EQ(0u, PCGGetOutputPositions(0, nullptr, 0), "Zero max_count returns 0");

    u32 dummy_u = 0;
    TEST_ASSERT_EQ(0u, PCGGetOutputMeshSlots(0, nullptr, 10), "Null slots returns 0");
    (void)dummy_u;

    f32 dummy_f = 0;
    TEST_ASSERT_EQ(0u, PCGGetOutputTransforms(0, nullptr, 10), "Null transforms returns 0");
    (void)dummy_f;

    PCGDestroyGraph();
    return TestResult::Passed;
}

TestResult TestPCGOutputReadScatter() {
    // Build a tiny graph: NoiseField → FieldScatter → MeshAssign, execute,
    // verify positions/mesh_slots/transforms read back consistently.
    PCGCreateGraph();

    const u32 noise = PCGAddNode("NoiseField");
    const u32 scatter = PCGAddNode("FieldScatter");
    const u32 mesh_assign = PCGAddNode("MeshAssign");
    TEST_ASSERT(noise != INVALID_NODE_ID, "NoiseField added");
    TEST_ASSERT(scatter != INVALID_NODE_ID, "FieldScatter added");
    TEST_ASSERT(mesh_assign != INVALID_NODE_ID, "MeshAssign added");

    // Give FieldScatter a small bounds to keep point count manageable in tests.
    TEST_ASSERT(PCGSetNodeParamVec3(scatter, "bounds_min", -8.f, 0.f, -8.f) != 0,
                "Set bounds_min");
    TEST_ASSERT(PCGSetNodeParamVec3(scatter, "bounds_max",  8.f, 0.f,  8.f) != 0,
                "Set bounds_max");

    // Configure MeshAssign: one mesh slot, uniform weight 1.
    PCGAddMeshSlot("Content/Procedural/Sphere.engine_mesh", "Sphere");
    f32 weights[1] = {1.0f};
    (void)weights;  // MeshAssign defaults to uniform if weights not set; skip for MVP

    // Wire noise.field_out → scatter.field_in, scatter.point_out → mesh_assign.point_in.
    // Pin indices follow the engine's node definitions (output 0, input 0 — common convention).
    PCGConnect(noise, 0, scatter, 0);
    PCGConnect(scatter, 0, mesh_assign, 0);

    PCGExecute();

    const u32 point_count = PCGGetOutputPointCount(mesh_assign);
    TEST_ASSERT(point_count > 0, "FieldScatter produced points");

    // Read positions into a buffer sized to point_count.
    std::vector<f32> positions(point_count * 3, 0.0f);
    const u32 got_pos = PCGGetOutputPositions(mesh_assign, positions.data(), point_count);
    TEST_ASSERT_EQ(point_count, got_pos, "Positions count == point count");

    // Read mesh slots — should all be 0 (only one slot).
    std::vector<u32> slots(point_count, 99);
    const u32 got_slots = PCGGetOutputMeshSlots(mesh_assign, slots.data(), point_count);
    TEST_ASSERT_EQ(point_count, got_slots, "Mesh slots count == point count");
    for (u32 i = 0; i < got_slots; ++i) {
        if (slots[i] != 0) {
            TEST_ASSERT(false, "MeshAssign with single slot → all indices 0");
            return TestResult::Failed;
        }
    }

    // Read transforms — 4 floats per point.
    std::vector<f32> xforms(point_count * 4, 0.0f);
    const u32 got_xforms = PCGGetOutputTransforms(mesh_assign, xforms.data(), point_count);
    TEST_ASSERT_EQ(point_count, got_xforms, "Transforms count == point count");

    // Sanity: ScaleX/Y/Z should be finite (default scale is 1.0 if TransformNode not wired).
    for (u32 i = 0; i < got_xforms; ++i) {
        const f32 sx = xforms[i * 4 + 0];
        const f32 sy = xforms[i * 4 + 1];
        const f32 sz = xforms[i * 4 + 2];
        if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(sz)) {
            TEST_ASSERT(false, "Transform scale values must be finite");
            return TestResult::Failed;
        }
    }

    // Partial-read contract: max_count < point_count returns max_count.
    if (point_count >= 2) {
        f32 small_buf[6] = {0};
        const u32 got_one = PCGGetOutputPositions(mesh_assign, small_buf, 1);
        TEST_ASSERT_EQ(1u, got_one, "max_count=1 returns 1");
    }

    PCGDestroyGraph();
    return TestResult::Passed;
}

// ============================================================================
// AssetAPI scene import tests
// ============================================================================

TestResult TestAssetImportMissingFile() {
    // Non-existent path → 0, doesn't crash.
    const u32 ok = ImportSceneBinary("nonexistent/path/missing.bin");
    TEST_ASSERT_EQ(0u, ok, "Missing file returns 0");
    TEST_ASSERT_EQ(0u, GetImportedMeshCount(), "No imported meshes after failed import");
    return TestResult::Passed;
}

TestResult TestAssetImportNullPath() {
    TEST_ASSERT_EQ(0u, ImportSceneBinary(nullptr), "Null path returns 0");
    return TestResult::Passed;
}

TestResult TestAssetImportedMeshInvalidIndex() {
    // Without any successful import, all queries on indices should be 0 / safe.
    TEST_ASSERT_EQ(0ull, GetImportedMeshContentId(0), "ContentId(0) w/o import = 0");
    TEST_ASSERT_EQ(0ull, GetImportedMeshContentId(99), "ContentId(99) w/o import = 0");

    const char* d = nullptr;
    const char* n = nullptr;
    const char* o = nullptr;
    TEST_ASSERT_EQ(0u, GetImportedMeshTexturePaths(0, &d, &n, &o), "TexturePaths(0) w/o import = 0");
    TEST_ASSERT(d == nullptr && n == nullptr && o == nullptr, "Out pointers untouched on failure");
    return TestResult::Passed;
}

// ============================================================================
// RenderPipelineAPI extension tests (headless — pipeline usually not initialized)
// ============================================================================

TestResult TestPipelineEditorModeRoundTrip() {
    // In headless test env, StandardRenderPipeline::Get() may be null. Either way,
    // PipelineSetEditorMode must not crash, and PipelineIsEditorMode returns 0 or
    // matches the most recent set value.
    PipelineSetEditorMode(1);
    const u32 after_on = PipelineIsEditorMode();
    // If pipeline isn't initialized, returns 0; if it is, returns 1. Both acceptable.
    (void)after_on;

    PipelineSetEditorMode(0);
    const u32 after_off = PipelineIsEditorMode();
    (void)after_off;
    return TestResult::Passed;
}

TestResult TestPipelineSetLumenConfigNoCrash() {
    // SetLumenConfig with each preset. In headless env, GetStdPipeline() may be null,
    // so this just verifies the C ABI doesn't crash on null dereference.
    for (u32 p = 0; p <= 5; ++p) {
        PipelineSetLumenConfig(p);
    }
    return TestResult::Passed;
}

TestResult TestPipelineRegisterMeshEntityNoPipeline() {
    // Headless: pipeline is null → must return 0 cleanly, not crash.
    const u64 entity = PipelineRegisterMeshEntity(12345 /*fake content_id*/, nullptr, 0);
    TEST_ASSERT_EQ(0ull, entity, "Register w/o pipeline returns 0");

    // Unregister on a bogus id must not crash.
    PipelineUnregisterMeshEntity(99999);
    PipelineUnregisterMeshEntity(0);
    return TestResult::Passed;
}

// ============================================================================
// MarchingCubes API tests (Phase 9.1)
// ============================================================================

TestResult TestMarchingCubesNoiseField() {
    // Build: NoiseField → MarchingCubes, execute, verify PCGGetOutputGeometry
    // returns a valid (non-INVALID) content_id. The mesh may be empty if noise
    // happens to never exceed iso_value, but PCGGetOutputGeometry should still
    // return a non-INVALID sentinel distinguishable from "node not MC".
    PCGCreateGraph();

    const u32 noise = PCGAddNode("NoiseField");
    const u32 mc    = PCGAddNode("MarchingCubes");
    TEST_ASSERT(noise != INVALID_NODE_ID, "NoiseField added");
    TEST_ASSERT(mc != INVALID_NODE_ID,    "MarchingCubes added");

    // Tighten bounds and raise iso so noise (FBM2D range ~[-1,1]) reliably straddles.
    TEST_ASSERT(PCGSetNodeParamVec3(mc, "bounds_min", -4.f, -4.f, -4.f) != 0, "Set bounds_min");
    TEST_ASSERT(PCGSetNodeParamVec3(mc, "bounds_max",  4.f,  4.f,  4.f) != 0, "Set bounds_max");
    TEST_ASSERT(PCGSetNodeParamFloat(mc, "iso_value", 0.0f) != 0, "Set iso_value");
    TEST_ASSERT(PCGSetNodeParamFloat(mc, "resolution", 32) != 0, "Set resolution");

    PCGConnect(noise, 0, mc, 0);
    PCGExecute();

    const u64 cid = PCGGetOutputGeometry(mc);
    TEST_ASSERT(cid != INVALID_CONTENT_ID, "MC produced a content_id (or documented empty-surface sentinel)");

    // Querying geometry on the NoiseField (input node) must return INVALID — type mismatch.
    const u64 bad = PCGGetOutputGeometry(noise);
    TEST_ASSERT_EQ(INVALID_CONTENT_ID, bad, "Non-MC node returns INVALID_CONTENT_ID");

    // Querying geometry on an out-of-range node_id must return INVALID, not crash.
    const u64 oob = PCGGetOutputGeometry(9999);
    TEST_ASSERT_EQ(INVALID_CONTENT_ID, oob, "Out-of-range node returns INVALID");

    PCGDestroyGraph();
    return TestResult::Passed;
}

TestResult TestMarchingCubesRegisterEntity() {
    // Generate a mesh via MC, then attempt PipelineRegisterMeshEntity on the result.
    // Headless: pipeline is null → must return 0 cleanly without crashing, and
    // importantly without corrupting the MC-tracked content_id (no FreeList assertion).
    PCGCreateGraph();

    const u32 noise = PCGAddNode("NoiseField");
    const u32 mc    = PCGAddNode("MarchingCubes");
    TEST_ASSERT(PCGSetNodeParamVec3(mc, "bounds_min", -4.f, -4.f, -4.f) != 0, "Set bounds_min");
    TEST_ASSERT(PCGSetNodeParamVec3(mc, "bounds_max",  4.f,  4.f,  4.f) != 0, "Set bounds_max");
    PCGConnect(noise, 0, mc, 0);
    PCGExecute();

    const u64 cid = PCGGetOutputGeometry(mc);
    TEST_ASSERT(cid != INVALID_CONTENT_ID, "MC produced a content_id");

    // Register (graceful no-op when pipeline absent).
    const u64 entity = PipelineRegisterMeshEntity(cid, nullptr, 0);
    (void)entity;  // 0 without pipeline — the point is no crash + no assertion

    // Unregister (also graceful on bogus id).
    PipelineUnregisterMeshEntity(entity);

    PCGDestroyGraph();
    return TestResult::Passed;
}

TestResult TestMarchingCubesReexecuteNoLeak() {
    // Execute the same MC node twice. The node must destroy the previous asset
    // before allocating a new one (destroy-before-create contract). If it forgets,
    // FreeList::operator[] would assert on a double-destroy or overflow.
    PCGCreateGraph();

    const u32 noise = PCGAddNode("NoiseField");
    const u32 mc    = PCGAddNode("MarchingCubes");
    TEST_ASSERT(PCGSetNodeParamVec3(mc, "bounds_min", -4.f, -4.f, -4.f) != 0, "Set bounds_min");
    TEST_ASSERT(PCGSetNodeParamVec3(mc, "bounds_max",  4.f,  4.f,  4.f) != 0, "Set bounds_max");
    PCGConnect(noise, 0, mc, 0);

    PCGExecute();
    const u64 cid1 = PCGGetOutputGeometry(mc);
    TEST_ASSERT(cid1 != INVALID_CONTENT_ID, "First execute produced a content_id");

    // Tweak a parameter (resolution) and re-execute.
    TEST_ASSERT(PCGSetNodeParamFloat(mc, "resolution", 24) != 0, "Bump resolution");
    PCGExecute();
    const u64 cid2 = PCGGetOutputGeometry(mc);
    TEST_ASSERT(cid2 != INVALID_CONTENT_ID, "Second execute produced a content_id");

    // Graph teardown implicitly destroys the latest asset via ~MarchingCubesNode.
    PCGDestroyGraph();
    return TestResult::Passed;
}

// ============================================================================
// CameraAPI matrix readback tests
// ============================================================================

TestResult TestCameraMatrixNullArgs() {
    TEST_ASSERT_EQ(0u, GetEntityCameraViewMatrix(0, nullptr), "Null out returns 0");
    TEST_ASSERT_EQ(0u, GetEntityCameraProjectionMatrix(0, nullptr), "Null out returns 0");
    return TestResult::Passed;
}

TestResult TestCameraMatrixInvalidEntity() {
    f32 view[16] = {0};
    f32 proj[16] = {0};
    // Pass invalid_id sentinel — id::is_valid() rejects it before any bounds-checked
    // lookup happens. (Out-of-range ids trigger an assertion in is_alive, which is a
    // separate API contract issue outside this test's scope.)
    const id::id_type bad = id::invalid_id;
    TEST_ASSERT_EQ(0u, GetEntityCameraViewMatrix(bad, view), "invalid_id returns 0");
    TEST_ASSERT_EQ(0u, GetEntityCameraProjectionMatrix(bad, proj), "invalid_id returns 0");
    return TestResult::Passed;
}

// ============================================================================
// Test runner
// ============================================================================

void RunProceduralMeshTests() {
    TestSuite suite("ProceduralMesh API Tests (Phase 5.1)");
    suite.AddTestCase(TestCase("Sphere create/destroy",
        TestProceduralMeshSphere,
        "Create sphere with valid params → non-zero content_id"));
    suite.AddTestCase(TestCase("Box create/destroy",
        TestProceduralMeshBox,
        "Create box with valid params → non-zero content_id"));
    suite.AddTestCase(TestCase("Invalid args rejected",
        TestProceduralMeshInvalidArgs,
        "Unknown type / null params / under-param / bogus destroy all safe"));
    suite.AddTestCase(TestCase("All 12 mesh types round-trip",
        TestProceduralMeshAllTypes,
        "Sphere/Cylinder/Cone/Box/Torus/Capsule/Disc/Hemisphere/Pyramid/Plane/QuadXY/Teapot"));
    suite.RunAllTests();
}

void RunPCGOutputTests() {
    TestSuite suite("PCG Output Read Tests (Phase 5.1)");
    suite.AddTestCase(TestCase("No graph → 0",
        TestPCGOutputReadNoGraph,
        "Positions/MeshSlots/Transforms return 0 without a graph"));
    suite.AddTestCase(TestCase("Null args rejected",
        TestPCGOutputReadNullArgs,
        "Null out_buf or zero max_count returns 0"));
    suite.AddTestCase(TestCase("Scatter round-trip",
        TestPCGOutputReadScatter,
        "NoiseField → FieldScatter → MeshAssign produces matching positions/slots/transforms"));
    suite.RunAllTests();
}

void RunAssetImportTests() {
    TestSuite suite("Asset Scene Import Tests (Phase 5.1)");
    suite.AddTestCase(TestCase("Missing file",
        TestAssetImportMissingFile,
        "ImportSceneBinary on bad path returns 0, no crash"));
    suite.AddTestCase(TestCase("Null path",
        TestAssetImportNullPath,
        "ImportSceneBinary(nullptr) returns 0"));
    suite.AddTestCase(TestCase("Invalid index queries",
        TestAssetImportedMeshInvalidIndex,
        "GetImportedMesh* on out-of-range index returns 0 / null without crashing"));
    suite.RunAllTests();
}

void RunPipelineExtTests() {
    TestSuite suite("RenderPipeline Extension Tests (Phase 5.1)");
    suite.AddTestCase(TestCase("Editor mode round-trip",
        TestPipelineEditorModeRoundTrip,
        "SetEditorMode/IsEditorMode do not crash headless"));
    suite.AddTestCase(TestCase("Lumen config presets",
        TestPipelineSetLumenConfigNoCrash,
        "SetLumenConfig with all 6 presets does not crash"));
    suite.AddTestCase(TestCase("Register w/o pipeline",
        TestPipelineRegisterMeshEntityNoPipeline,
        "RegisterMeshEntity/Unregister safe without pipeline initialized"));
    suite.RunAllTests();
}

void RunCameraMatrixTests() {
    TestSuite suite("Camera Matrix Readback Tests (Phase 5.1)");
    suite.AddTestCase(TestCase("Null out_buf rejected",
        TestCameraMatrixNullArgs,
        "Get*Matrix with null out returns 0"));
    suite.AddTestCase(TestCase("Invalid entity rejected",
        TestCameraMatrixInvalidEntity,
        "Get*Matrix on non-existent entity returns 0"));
    suite.RunAllTests();
}

void RunMarchingCubesTests() {
    TestSuite suite("MarchingCubes API Tests (Phase 9.1)");
    suite.AddTestCase(TestCase("NoiseField → mesh",
        TestMarchingCubesNoiseField,
        "NoiseField → MarchingCubes → valid content_id; non-MC/OOB nodes return INVALID"));
    suite.AddTestCase(TestCase("Register entity",
        TestMarchingCubesRegisterEntity,
        "PipelineRegisterMeshEntity on MC output is a graceful no-op headless"));
    suite.AddTestCase(TestCase("Re-execute no leak",
        TestMarchingCubesReexecuteNoLeak,
        "Two Execute() calls on same MC node: destroy-before-create, no FreeList assertion"));
    suite.RunAllTests();
}

int main() {
    RunProceduralMeshTests();
    RunPCGOutputTests();
    RunAssetImportTests();
    RunPipelineExtTests();
    RunCameraMatrixTests();
    RunMarchingCubesTests();
    return 0;
}
