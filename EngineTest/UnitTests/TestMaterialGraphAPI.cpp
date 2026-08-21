// Unit tests for MaterialGraphAPI (P0.1 + P0.5 C API wrapper).
// Tests the 24 C ABI functions exposed by EngineDLL/MaterialGraphAPI.cpp.
// Since the API uses a static slot map inside the DLL, these tests must
// link against EngineDLL (not call dlopen). On macOS the dylib exports
// the symbols via EDITOR_INTERFACE (visibility("default")).

#include "TestFramework.h"
#include "CommonHeaders.h"
#include <filesystem>
#include <iostream>
#include <cstring>
#include <cmath>

// --- C-facing struct declarations (must match MaterialGraphAPI.cpp) ---

struct material_graph_connection {
    u32 from_node, from_pin, to_node, to_pin;
};

struct material_init_info_c {
    u32 technique;
    f32 base_color[4];
    f32 roughness, metallic, alpha_cutoff;
    u64 albedo_texture, normal_texture, orm_texture;
};

// --- extern "C" prototypes mirroring EDITOR_INTERFACE exports ---

extern "C" {
    u32  CreateMaterialGraph();
    void DestroyMaterialGraph(u32 graph_id);
    void DestroyAllMaterialGraphs();
    u32  CloneMaterialGraph(u32 src_graph_id);

    u32  GraphAddNode(u32 graph_id, const char* type_name);
    u32  GraphRemoveNode(u32 graph_id, u32 node_id);
    u32  GraphGetNodeCount(u32 graph_id);
    u32  GraphGetNodeIdByIndex(u32 graph_id, u32 index);
    u32  GraphNodeGetTypeId(u32 graph_id, u32 node_id, char* out_buf, u64 buf_size);

    u32  GraphConnect(u32 graph_id, u32 from_node, u32 from_pin, u32 to_node, u32 to_pin);
    u32  GraphDisconnect(u32 graph_id, u32 from_node, u32 from_pin, u32 to_node, u32 to_pin);
    u32  GraphGetConnectionCount(u32 graph_id);
    u32  GraphGetConnectionByIndex(u32 graph_id, u32 index, material_graph_connection* out);

    void GraphClear(u32 graph_id);
    u32  GraphExecute(u32 graph_id);
    u32  GraphGetErrorCount(u32 graph_id);
    u32  GraphGetErrorByIndex(u32 graph_id, u32 index, u32* out_node_id, char* out_msg, u64 buf_size);
    void GraphClearErrors(u32 graph_id);

    u64  GraphGetJsonSize(u32 graph_id);
    u32  GraphGetJson(u32 graph_id, char* out, u64 buf_size);
    u32  GraphLoadFromJson(u32 graph_id, const char* json);
    u32  LoadMaterialGraphFromFile(const char* path);
    u32  SaveMaterialGraphToFile(u32 graph_id, const char* path);

    u32  GraphEvaluateToMaterialInfo(u32 graph_id, material_init_info_c* out);
}

using namespace Engine::Test;

// --- Test 1: Create / Destroy balance (no crash) ---

TestResult TestCreateDestroy() {
    u32 g = CreateMaterialGraph();
    TEST_ASSERT(g != 0, "CreateMaterialGraph should return non-zero id");
    DestroyMaterialGraph(g);
    // Double-destroy of same id is a no-op (slot is already nullptr).
    DestroyMaterialGraph(g);
    return TestResult::Passed;
}

// --- Test 2: AddNode + GetNodeCount ---

TestResult TestAddNode() {
    u32 g = CreateMaterialGraph();
    TEST_ASSERT(g != 0, "Create failed");

    u32 n1 = GraphAddNode(g, "ConstantFloat4");
    TEST_ASSERT(n1 != 0, "GraphAddNode should return 1-based node id");

    u32 count = GraphGetNodeCount(g);
    TEST_ASSERT_EQ(1u, count, "Node count should be 1 after one AddNode");

    u32 n2 = GraphAddNode(g, "MaterialOutput");
    TEST_ASSERT(n2 != 0, "Second AddNode failed");
    TEST_ASSERT(n2 != n1, "Node ids should differ");

    count = GraphGetNodeCount(g);
    TEST_ASSERT_EQ(2u, count, "Node count should be 2");

    DestroyMaterialGraph(g);
    return TestResult::Passed;
}

// --- Test 3: Connect + GetConnectionCount ---

TestResult TestConnect() {
    u32 g = CreateMaterialGraph();
    TEST_ASSERT(g != 0, "Create failed");

    u32 n1 = GraphAddNode(g, "ConstantFloat4");  // node 1
    u32 n2 = GraphAddNode(g, "MaterialOutput");  // node 2
    TEST_ASSERT(n1 != 0 && n2 != 0, "AddNode failed");

    u32 rc = GraphConnect(g, n1, 0, n2, 0);  // Float4 output -> BaseColor input
    TEST_ASSERT_EQ(0u, rc, "GraphConnect should return 0 on success");

    u32 conn_count = GraphGetConnectionCount(g);
    TEST_ASSERT_EQ(1u, conn_count, "Connection count should be 1");

    // Verify connection data via GetConnectionByIndex
    material_graph_connection c{};
    u32 rc2 = GraphGetConnectionByIndex(g, 0, &c);
    TEST_ASSERT_EQ(0u, rc2, "GetConnectionByIndex should succeed");
    TEST_ASSERT_EQ(n1, c.from_node, "from_node mismatch");
    TEST_ASSERT_EQ(0u, c.from_pin, "from_pin mismatch");
    TEST_ASSERT_EQ(n2, c.to_node, "to_node mismatch");
    TEST_ASSERT_EQ(0u, c.to_pin, "to_pin mismatch");

    DestroyMaterialGraph(g);
    return TestResult::Passed;
}

// --- Test 4: Execute on a minimal valid graph ---

TestResult TestExecute() {
    u32 g = CreateMaterialGraph();
    TEST_ASSERT(g != 0, "Create failed");

    u32 n1 = GraphAddNode(g, "ConstantFloat4");
    u32 n2 = GraphAddNode(g, "MaterialOutput");
    TEST_ASSERT(n1 != 0 && n2 != 0, "AddNode failed");

    GraphConnect(g, n1, 0, n2, 0);

    u32 error_count = GraphExecute(g);
    TEST_ASSERT_EQ(0u, error_count, "Execute should report 0 errors for valid graph");

    DestroyMaterialGraph(g);
    return TestResult::Passed;
}

// --- Test 5: JSON round-trip ---

TestResult TestJsonRoundTrip() {
    u32 g1 = CreateMaterialGraph();
    TEST_ASSERT(g1 != 0, "Create g1 failed");

    GraphAddNode(g1, "ConstantFloat4");
    GraphAddNode(g1, "MaterialOutput");
    GraphConnect(g1, 1, 0, 2, 0);

    u64 json_size = GraphGetJsonSize(g1);
    TEST_ASSERT(json_size > 0, "Json size should be > 0");

    char* buf = new char[json_size];
    u32 rc = GraphGetJson(g1, buf, json_size);
    TEST_ASSERT_EQ(0u, rc, "GraphGetJson should succeed");

    // Load JSON into a fresh graph.
    u32 g2 = CreateMaterialGraph();
    TEST_ASSERT(g2 != 0, "Create g2 failed");

    rc = GraphLoadFromJson(g2, buf);
    TEST_ASSERT_EQ(0u, rc, "GraphLoadFromJson should succeed");

    u32 count = GraphGetNodeCount(g2);
    TEST_ASSERT_EQ(2u, count, "Round-tripped graph should have 2 nodes");

    u32 conn_count = GraphGetConnectionCount(g2);
    TEST_ASSERT_EQ(1u, conn_count, "Round-tripped graph should have 1 connection");

    delete[] buf;
    DestroyMaterialGraph(g1);
    DestroyMaterialGraph(g2);
    return TestResult::Passed;
}

// --- Test 6: GraphEvaluateToMaterialInfo ---

TestResult TestEvaluateToMaterialInfo() {
    u32 g = CreateMaterialGraph();
    TEST_ASSERT(g != 0, "Create failed");

    // ConstantFloat4 default value is (0,0,0,1).
    u32 n1 = GraphAddNode(g, "ConstantFloat4");
    u32 n2 = GraphAddNode(g, "MaterialOutput");
    TEST_ASSERT(n1 != 0 && n2 != 0, "AddNode failed");

    GraphConnect(g, n1, 0, n2, 0);

    material_init_info_c info{};
    u32 rc = GraphEvaluateToMaterialInfo(g, &info);
    TEST_ASSERT_EQ(0u, rc, "Evaluate should succeed on valid graph");

    // ConstantFloat4 default = {0, 0, 0, 1}.
    TEST_ASSERT_FLOAT_EQ(0.0f, info.base_color[0], 0.001f, "base_color[0]");
    TEST_ASSERT_FLOAT_EQ(0.0f, info.base_color[1], 0.001f, "base_color[1]");
    TEST_ASSERT_FLOAT_EQ(0.0f, info.base_color[2], 0.001f, "base_color[2]");
    TEST_ASSERT_FLOAT_EQ(1.0f, info.base_color[3], 0.001f, "base_color[3]");

    DestroyMaterialGraph(g);
    return TestResult::Passed;
}

// --- Test 7: Invalid graph_id ---

TestResult TestInvalidGraphId() {
    const u32 invalid = 9999;

    // All functions should handle invalid id gracefully.
    TEST_ASSERT_EQ(0u, GraphGetNodeCount(invalid), "Invalid GetNodeCount");
    TEST_ASSERT_EQ(0u, GraphAddNode(invalid, "ConstantFloat4"), "Invalid AddNode");
    TEST_ASSERT_EQ(1u, GraphRemoveNode(invalid, 1), "Invalid RemoveNode");
    TEST_ASSERT_EQ(1u, GraphConnect(invalid, 1, 0, 2, 0), "Invalid Connect");
    TEST_ASSERT_EQ(0u, GraphGetConnectionCount(invalid), "Invalid GetConnectionCount");

    material_graph_connection c{};
    TEST_ASSERT_EQ(1u, GraphGetConnectionByIndex(invalid, 0, &c), "Invalid GetConnectionByIndex");

    char buf[64];
    TEST_ASSERT_EQ(1u, GraphNodeGetTypeId(invalid, 1, buf, sizeof(buf)), "Invalid GetNodeTypeId");
    TEST_ASSERT_EQ(0u, GraphExecute(invalid), "Invalid Execute");
    TEST_ASSERT_EQ(0u, GraphGetErrorCount(invalid), "Invalid GetErrorCount");

    // No-throw on void functions.
    GraphClear(invalid);
    GraphClearErrors(invalid);
    DestroyMaterialGraph(invalid);

    return TestResult::Passed;
}

// --- Test 8: File IO round-trip ---

TestResult TestFileIO() {
    u32 g1 = CreateMaterialGraph();
    TEST_ASSERT(g1 != 0, "Create failed");

    GraphAddNode(g1, "ConstantFloat4");
    GraphAddNode(g1, "MaterialOutput");

    // "/tmp" 是 POSIX 路径——Windows 上 fopen("/tmp/...") 解析到当前盘符根的
    // C:\tmp(不存在)→ 保存失败。改用系统临时目录的可移植路径。
    const std::string path =
        (std::filesystem::temp_directory_path() / "test_material_graph_api.json")
            .string();
    u32 rc = SaveMaterialGraphToFile(g1, path.c_str());
    TEST_ASSERT_EQ(0u, rc, "Save should succeed");

    u32 g2 = LoadMaterialGraphFromFile(path.c_str());
    TEST_ASSERT(g2 != 0, "Load should return valid id");

    u32 count = GraphGetNodeCount(g2);
    TEST_ASSERT_EQ(2u, count, "Loaded graph should have 2 nodes");

    DestroyMaterialGraph(g1);
    DestroyMaterialGraph(g2);
    return TestResult::Passed;
}

// --- Test 9: Clone ---

TestResult TestClone() {
    u32 g1 = CreateMaterialGraph();
    TEST_ASSERT(g1 != 0, "Create failed");

    GraphAddNode(g1, "ConstantFloat4");
    GraphAddNode(g1, "MaterialOutput");

    u32 g2 = CloneMaterialGraph(g1);
    TEST_ASSERT(g2 != 0, "Clone should return valid id");
    TEST_ASSERT(g1 != g2, "Clone id should differ from source");

    u32 count = GraphGetNodeCount(g2);
    TEST_ASSERT_EQ(2u, count, "Cloned graph should have 2 nodes");

    DestroyMaterialGraph(g1);
    DestroyMaterialGraph(g2);
    return TestResult::Passed;
}

// --- Test 10: NodeGetTypeId ---

TestResult TestNodeGetTypeId() {
    u32 g = CreateMaterialGraph();
    TEST_ASSERT(g != 0, "Create failed");

    u32 n1 = GraphAddNode(g, "ConstantFloat4");
    char buf[64] = {};
    u32 rc = GraphNodeGetTypeId(g, n1, buf, sizeof(buf));
    TEST_ASSERT_EQ(0u, rc, "GetTypeId should succeed");
    TEST_ASSERT_STR_EQ("ConstantFloat4", buf, "Type name mismatch");

    DestroyMaterialGraph(g);
    return TestResult::Passed;
}

// --- Test 11: RemoveNode ---

TestResult TestRemoveNode() {
    u32 g = CreateMaterialGraph();
    TEST_ASSERT(g != 0, "Create failed");

    GraphAddNode(g, "ConstantFloat4");
    GraphAddNode(g, "MaterialOutput");
    TEST_ASSERT_EQ(2u, GraphGetNodeCount(g), "Should have 2 nodes");

    u32 rc = GraphRemoveNode(g, 1);  // remove first node (1-based)
    TEST_ASSERT_EQ(0u, rc, "RemoveNode should succeed");
    TEST_ASSERT_EQ(1u, GraphGetNodeCount(g), "Should have 1 node after remove");

    DestroyMaterialGraph(g);
    return TestResult::Passed;
}

// --- Test 12: Disconnect ---

TestResult TestDisconnect() {
    u32 g = CreateMaterialGraph();
    TEST_ASSERT(g != 0, "Create failed");

    u32 n1 = GraphAddNode(g, "ConstantFloat4");
    u32 n2 = GraphAddNode(g, "MaterialOutput");
    GraphConnect(g, n1, 0, n2, 0);
    TEST_ASSERT_EQ(1u, GraphGetConnectionCount(g), "Should have 1 connection");

    u32 rc = GraphDisconnect(g, n1, 0, n2, 0);
    TEST_ASSERT_EQ(0u, rc, "Disconnect should succeed");
    TEST_ASSERT_EQ(0u, GraphGetConnectionCount(g), "Should have 0 connections");

    DestroyMaterialGraph(g);
    return TestResult::Passed;
}

int main() {
    TestSuite suite("MaterialGraphAPI");

    TEST_CASE(suite, "CreateDestroy",       TestCreateDestroy);
    TEST_CASE(suite, "AddNode",             TestAddNode);
    TEST_CASE(suite, "Connect",             TestConnect);
    TEST_CASE(suite, "Execute",             TestExecute);
    TEST_CASE(suite, "JsonRoundTrip",       TestJsonRoundTrip);
    TEST_CASE(suite, "EvaluateToMaterialInfo", TestEvaluateToMaterialInfo);
    TEST_CASE(suite, "InvalidGraphId",      TestInvalidGraphId);
    TEST_CASE(suite, "FileIO",              TestFileIO);
    TEST_CASE(suite, "Clone",               TestClone);
    TEST_CASE(suite, "NodeGetTypeId",       TestNodeGetTypeId);
    TEST_CASE(suite, "RemoveNode",          TestRemoveNode);
    TEST_CASE(suite, "Disconnect",          TestDisconnect);

    TestStats stats = suite.RunAllTests();
    return (stats.failedTests == 0) ? 0 : 1;
}
