// TestMaterialReflection: unit tests for P0.2+P0.3+P0.4 node type reflection.
// Tests:
//   1. GetRegisteredMaterialNodeTypeCount() == 20
//   2. Category counts: Constants=5, Input=2, Math=10, Texture=1, Utility=1, Output=1
//   3. MaterialOutput is_terminal == 1
//   4. ConstantFloat4: 0 input pins, 1 output pin (data_type=Float4)
//   5. MaterialOutput: 8 input pins
//   6. Param set/get round-trip: ConstantFloat4 value

#include "CommonHeaders.h"
#include "Graphics/MaterialGraph/MaterialGraph.h"
#include "Graphics/MaterialGraph/MaterialGraphSerializer.h"
#include "Graphics/MaterialGraph/MaterialNode.h"
#include "Graphics/MaterialGraph/Nodes/ConstantNode.h"
#include "Graphics/MaterialGraph/Nodes/MaterialOutputNode.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace primal;
using namespace primal::graphics::material_graph;

// --- C ABI declarations (we test both engine-level and C ABI) ---
// These are declared extern "C" in the DLL, but we just declare them here
// since the test doesn't link against EngineDLL.
// We test engine-level reflection directly, which is the source of truth.

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(cond, msg) do { \
    if (cond) { \
        ++g_tests_passed; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        ++g_tests_failed; \
        printf("  [FAIL] %s\n", msg); \
    } \
} while(0)

static void TestNodeTypeCount() {
    printf("\n=== Test 1: Registered Node Type Count ===\n");
    u32 count = MaterialGraphSerializer::GetRegisteredNodeTypeCount();
    TEST(count == 20, "GetRegisteredNodeTypeCount() == 20");
}

static void TestCategoryCounts() {
    printf("\n=== Test 2: Category Counts ===\n");

    // Count nodes per category by creating each node and reading GetTypeInfo()
    int constants = 0, input = 0, math = 0, texture = 0, utility = 0, output = 0;

    for (u32 i = 0; i < MaterialGraphSerializer::GetRegisteredNodeTypeCount(); ++i) {
        auto info = MaterialGraphSerializer::GetRegisteredNodeTypeInfo(i);
        std::string cat(info.category);
        if (cat == "Constants") ++constants;
        else if (cat == "Input") ++input;
        else if (cat == "Math") ++math;
        else if (cat == "Texture") ++texture;
        else if (cat == "Utility") ++utility;
        else if (cat == "Output") ++output;
    }

    printf("    Constants=%d (expect 5)\n", constants);
    printf("    Input=%d (expect 2)\n", input);
    printf("    Math=%d (expect 10)\n", math);
    printf("    Texture=%d (expect 1)\n", texture);
    printf("    Utility=%d (expect 1)\n", utility);
    printf("    Output=%d (expect 1)\n", output);

    TEST(constants == 5, "Constants category has 5 nodes");
    TEST(input == 2, "Input category has 2 nodes");
    TEST(math == 10, "Math category has 10 nodes");
    TEST(texture == 1, "Texture category has 1 node");
    TEST(utility == 1, "Utility category has 1 node");
    TEST(output == 1, "Output category has 1 node");
}

static void TestTerminalFlag() {
    printf("\n=== Test 3: MaterialOutput is_terminal ===\n");

    bool found_terminal = false;
    int terminal_count = 0;

    for (u32 i = 0; i < MaterialGraphSerializer::GetRegisteredNodeTypeCount(); ++i) {
        auto info = MaterialGraphSerializer::GetRegisteredNodeTypeInfo(i);
        if (info.is_terminal) {
            ++terminal_count;
            if (std::string(info.type_name) == "MaterialOutput") {
                found_terminal = true;
            }
        }
    }

    TEST(found_terminal, "MaterialOutput has is_terminal == true");
    TEST(terminal_count == 1, "Exactly 1 terminal node type (MaterialOutput)");
}

static void TestConstantFloat4Pins() {
    printf("\n=== Test 4: ConstantFloat4 Pin Structure ===\n");

    auto node = MaterialGraphSerializer::CreateNode("ConstantFloat4");
    TEST(node != nullptr, "CreateNode(\"ConstantFloat4\") succeeds");

    if (!node) return;

    u32 pin_count = 0;
    const MaterialPinDescriptor* pins = node->GetPinDescriptors(pin_count);
    TEST(pin_count == 1, "ConstantFloat4 has 1 pin descriptor");

    if (pin_count > 0 && pins) {
        TEST(pins[0].is_input == false, "ConstantFloat4 pin 0 is output (is_input=false)");
        TEST(static_cast<u32>(pins[0].data_type) == static_cast<u32>(MaterialDataType::Float4),
             "ConstantFloat4 pin 0 data_type == Float4");
    }

    // Verify node's runtime pins match
    TEST(node->inputs.size() == 0, "ConstantFloat4 has 0 input pins at runtime");
    TEST(node->outputs.size() == 1, "ConstantFloat4 has 1 output pin at runtime");
}

static void TestMaterialOutputPins() {
    printf("\n=== Test 5: MaterialOutput Pin Structure ===\n");

    auto node = MaterialGraphSerializer::CreateNode("MaterialOutput");
    TEST(node != nullptr, "CreateNode(\"MaterialOutput\") succeeds");

    if (!node) return;

    u32 pin_count = 0;
    const MaterialPinDescriptor* pins = node->GetPinDescriptors(pin_count);
    TEST(pin_count == 8, "MaterialOutput has 8 pin descriptors");

    if (pin_count >= 8 && pins) {
        // Verify all are inputs
        bool all_inputs = true;
        for (u32 i = 0; i < pin_count; ++i) {
            if (!pins[i].is_input) { all_inputs = false; break; }
        }
        TEST(all_inputs, "All MaterialOutput pins are inputs");

        // Check specific pin data types
        TEST(static_cast<u32>(pins[0].data_type) == static_cast<u32>(MaterialDataType::Float4),
             "Pin 0 (BaseColor) is Float4");
        TEST(static_cast<u32>(pins[1].data_type) == static_cast<u32>(MaterialDataType::Float),
             "Pin 1 (Roughness) is Float");
        TEST(static_cast<u32>(pins[4].data_type) == static_cast<u32>(MaterialDataType::Texture2D),
             "Pin 4 (AlbedoTex) is Texture2D");
    }

    // Runtime: 8 input pins, 0 output
    TEST(node->inputs.size() == 8, "MaterialOutput has 8 input pins at runtime");
    TEST(node->outputs.size() == 0, "MaterialOutput has 0 output pins at runtime");
}

static void TestParamRoundTrip() {
    printf("\n=== Test 6: Param Set/Get Round-Trip (ConstantFloat4) ===\n");

    // Create a graph, add ConstantFloat4
    MaterialGraph graph;
    auto node = MaterialGraphSerializer::CreateNode("ConstantFloat4");
    TEST(node != nullptr, "CreateNode(\"ConstantFloat4\") for round-trip");

    if (!node) return;

    ConstantFloat4Node* raw_ptr = static_cast<ConstantFloat4Node*>(node.get());

    u32 node_id = graph.AddNode(std::move(node));
    TEST(node_id == 0, "Node added at index 0");

    // Set value to [0.85, 0.35, 0.15, 1.0]
    math::v4 set_val{0.85f, 0.35f, 0.15f, 1.0f};
    bool set_ok = graph.GetNodes()[node_id]->SetParamByName("value", set_val);
    TEST(set_ok, "SetParamByName(\"value\", [0.85, 0.35, 0.15, 1.0]) returns true");

    // Read back via param descriptor + offset (same pattern as serializer)
    u32 param_count = 0;
    const MaterialParamDescriptor* descs = graph.GetNodes()[node_id]->GetParamDescriptors(param_count);
    TEST(param_count == 1, "ConstantFloat4 has 1 param descriptor");

    if (param_count > 0 && descs) {
        TEST(std::strcmp(descs[0].name, "value") == 0, "Param 0 name is \"value\"");
        TEST(static_cast<u32>(descs[0].type) == static_cast<u32>(MaterialParamType::Float4),
             "Param 0 type is Float4");

        // Read via offset
        const auto* base = reinterpret_cast<const char*>(graph.GetNodes()[node_id].get());
        const auto* val_ptr = reinterpret_cast<const math::v4*>(base + descs[0].offset);

        bool matches = (std::abs(val_ptr->x - 0.85f) < 1e-5f &&
                        std::abs(val_ptr->y - 0.35f) < 1e-5f &&
                        std::abs(val_ptr->z - 0.15f) < 1e-5f &&
                        std::abs(val_ptr->w - 1.0f) < 1e-5f);
        TEST(matches, "Read-back value matches [0.85, 0.35, 0.15, 1.0]");

        // Also check the raw member directly
        bool raw_match = (std::abs(raw_ptr->value.x - 0.85f) < 1e-5f &&
                          std::abs(raw_ptr->value.y - 0.35f) < 1e-5f &&
                          std::abs(raw_ptr->value.z - 0.15f) < 1e-5f &&
                          std::abs(raw_ptr->value.w - 1.0f) < 1e-5f);
        TEST(raw_match, "Raw member value matches [0.85, 0.35, 0.15, 1.0]");
    }
}

static void TestAllTypeInfo() {
    printf("\n=== Test 7: All 20 NodeTypeInfo Sanity ===\n");

    int total = 0;
    for (u32 i = 0; i < MaterialGraphSerializer::GetRegisteredNodeTypeCount(); ++i) {
        auto info = MaterialGraphSerializer::GetRegisteredNodeTypeInfo(i);
        bool valid = (info.type_name != nullptr && info.type_name[0] != '\0' &&
                      info.display_name != nullptr && info.display_name[0] != '\0' &&
                      info.category != nullptr && info.category[0] != '\0');
        if (valid) {
            ++total;
            printf("    [%2u] %-18s | %-18s | %s%s\n", i,
                   info.type_name, info.display_name, info.category,
                   info.is_terminal ? " (terminal)" : "");
        } else {
            printf("    [%2u] INVALID TYPE INFO\n", i);
        }
    }
    TEST(total == 20, "All 20 nodes have valid type info");
}

int main() {
    printf("========================================\n");
    printf("  TestMaterialReflection\n");
    printf("========================================\n");

    TestNodeTypeCount();
    TestCategoryCounts();
    TestTerminalFlag();
    TestConstantFloat4Pins();
    TestMaterialOutputPins();
    TestParamRoundTrip();
    TestAllTypeInfo();

    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", g_tests_passed, g_tests_failed);
    printf("========================================\n");

    return g_tests_failed > 0 ? 1 : 0;
}
