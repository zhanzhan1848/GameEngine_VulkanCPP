// PCG C-API boundary for editor integration (C# P/Invoke, scripting, etc.)
//
// Exposes 30 EDITOR_INTERFACE functions covering the full PCG graph lifecycle:
//   - Graph creation/destruction (single global instance)
//   - Node CRUD (add/remove/query by type name)
//   - Connection management (connect/disconnect pins)
//   - Parameter reflection + setting (auto-generate UI controls from descriptors)
//   - Pin reflection (auto-generate pin connectors)
//   - Mesh library (graph-level mesh slot registry for MeshAssignNode)
//   - Execution + output querying
//   - Error reporting
//   - JSON serialization/deserialization (with mesh_slots support)
//   - Node type registry (populate editor palette)
//
// Threading: All functions operate on a single global PCGGraph* (g_pcg_graph).
// Caller must synchronize access. Typical usage: call from main thread only.
//
// Null safety: All functions null-check g_pcg_graph and individual arguments.
// Invalid node_id/pin_idx/index return 0/nullptr without crashing.
//
// Symbol visibility: All functions use EDITOR_INTERFACE (extern "C" + visibility default).
// Verify exports with: nm <dylib> | grep PCG
//
// C# interop example:
//   [DllImport("libEngineDLL.dylib")] static extern void PCGCreateGraph();
//   [DllImport("libEngineDLL.dylib")] static extern uint PCGAddNode(string typeName);
//   [DllImport("libEngineDLL.dylib")] static extern uint PCGSetNodeParamFloat(uint nodeId, string name, float value);

#if defined(_MSC_VER)
#include "Common.h"
#include "Graphics/PCG/PCGGraph.h"
#include "Graphics/PCG/PCGSerializer.h"
#include "Graphics/PCG/PCGReflection.h"
#include "Graphics/PCG/Nodes/ReferenceFieldNode.h"
#include "Graphics/PCG/Nodes/NoiseFieldNode.h"
#include "Graphics/PCG/Nodes/FieldScatterNode.h"
#include "Graphics/PCG/Nodes/SDFConstraintNode.h"
#include "Graphics/PCG/Nodes/DensityFilterNode.h"
#include "Graphics/PCG/Nodes/TransformNode.h"
#include "Graphics/PCG/Nodes/MeshAssignNode.h"
#include <cstring>

#pragma comment(lib, "Engine.lib")

#elif defined(__clang__)
#include "Common.h"
#include "Graphics/PCG/PCGGraph.h"
#include "Graphics/PCG/PCGSerializer.h"
#include "Graphics/PCG/PCGReflection.h"
#include "Graphics/PCG/Nodes/ReferenceFieldNode.h"
#include "Graphics/PCG/Nodes/NoiseFieldNode.h"
#include "Graphics/PCG/Nodes/FieldScatterNode.h"
#include "Graphics/PCG/Nodes/SDFConstraintNode.h"
#include "Graphics/PCG/Nodes/DensityFilterNode.h"
#include "Graphics/PCG/Nodes/TransformNode.h"
#include "Graphics/PCG/Nodes/MeshAssignNode.h"
#include <cstring>

#endif

using namespace primal::graphics::pcg;

namespace {
// Single global graph instance. PCGCreateGraph/PCGDestroyGraph manage lifetime.
PCGGraph* g_pcg_graph = nullptr;
}

extern "C" {

// ============================================================================
// Graph Lifecycle
// ============================================================================

// Create a new graph instance. Destroys any existing graph.
EDITOR_INTERFACE void PCGCreateGraph() {
    delete g_pcg_graph;
    g_pcg_graph = new PCGGraph();
}

// Destroy the current graph instance. Safe to call multiple times.
EDITOR_INTERFACE void PCGDestroyGraph() {
    delete g_pcg_graph;
    g_pcg_graph = nullptr;
}

// ============================================================================
// Node CRUD
// ============================================================================

// Create a node by type name and add it to the graph.
// type_name: one of "ReferenceField", "NoiseField", "FieldScatter",
//            "SDFConstraint", "DensityFilter", "Transform", "MeshAssign"
// Returns: node ID (index), or u32(-1) on failure.
EDITOR_INTERFACE u32 PCGAddNode(const char* type_name) {
    if (!g_pcg_graph || !type_name) return u32(-1);
    auto node = PCGSerializer::CreateNode(type_name);
    if (!node) return u32(-1);
    return g_pcg_graph->AddNode(std::move(node));
}

// Remove a node by ID. All connections to/from this node are removed.
// Remaining node IDs shift: if you remove node 1, old node 2 becomes 1, etc.
EDITOR_INTERFACE void PCGRemoveNode(u32 node_id) {
    if (!g_pcg_graph) return;
    g_pcg_graph->RemoveNode(node_id);
}

// Returns the number of nodes in the current graph.
EDITOR_INTERFACE u32 PCGGetNodeCount() {
    return g_pcg_graph ? static_cast<u32>(g_pcg_graph->GetNodes().size()) : 0;
}

// Returns the type name of node at node_id (e.g. "NoiseField"), or nullptr.
// Pointer is valid as long as the node exists.
EDITOR_INTERFACE const char* PCGGetNodeTypeName(u32 node_id) {
    if (!g_pcg_graph || node_id >= g_pcg_graph->GetNodes().size()) return nullptr;
    return g_pcg_graph->GetNodes()[node_id]->TypeName();
}

// ============================================================================
// Connection
// ============================================================================

// Create a directed connection: from_node's output[from_pin] → to_node's input[to_pin].
// Data flows from output to input during PCGExecute().
EDITOR_INTERFACE void PCGConnect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin) {
    if (!g_pcg_graph) return;
    g_pcg_graph->Connect(from_node, from_pin, to_node, to_pin);
}

// Remove a specific connection matching all four identifiers.
EDITOR_INTERFACE void PCGDisconnect(u32 from_node, u32 from_pin, u32 to_node, u32 to_pin) {
    if (!g_pcg_graph) return;
    g_pcg_graph->Disconnect(from_node, from_pin, to_node, to_pin);
}

// ============================================================================
// Parameter Reflection
// ============================================================================

// Returns the number of parameters exposed by the node at node_id.
EDITOR_INTERFACE u32 PCGGetNodeParamCount(u32 node_id) {
    if (!g_pcg_graph || node_id >= g_pcg_graph->GetNodes().size()) return 0;
    u32 count;
    g_pcg_graph->GetNodes()[node_id]->GetParamDescriptors(count);
    return count;
}

// Returns a pointer to the param_idx'th PCGParamDescriptor for the node.
// Use to auto-generate UI controls (slider type, min/max range, enum labels).
// Returns nullptr if node_id or param_idx is out of range.
// Pointer is valid as long as the node exists (points to static data).
EDITOR_INTERFACE const PCGParamDescriptor* PCGGetNodeParamDescriptor(u32 node_id, u32 param_idx) {
    if (!g_pcg_graph || node_id >= g_pcg_graph->GetNodes().size()) return nullptr;
    u32 count;
    auto* descs = g_pcg_graph->GetNodes()[node_id]->GetParamDescriptors(count);
    return (param_idx < count) ? &descs[param_idx] : nullptr;
}

// Set a scalar (Float/UInt/Int/Enum/Bool) parameter by name.
// Returns 1 if the parameter was found and set, 0 otherwise.
EDITOR_INTERFACE u32 PCGSetNodeParamFloat(u32 node_id, const char* name, f32 value) {
    if (!g_pcg_graph || node_id >= g_pcg_graph->GetNodes().size() || !name) return 0;
    return g_pcg_graph->GetNodes()[node_id]->SetParamByName(name, value) ? 1u : 0u;
}

// Set a Vec3 parameter by name (e.g. "bounds_min", "scale_max").
// Returns 1 if the parameter was found and set, 0 otherwise.
EDITOR_INTERFACE u32 PCGSetNodeParamVec3(u32 node_id, const char* name, f32 x, f32 y, f32 z) {
    if (!g_pcg_graph || node_id >= g_pcg_graph->GetNodes().size() || !name) return 0;
    return g_pcg_graph->GetNodes()[node_id]->SetParamByName(name, primal::math::v3{x, y, z}) ? 1u : 0u;
}

// Set a float array parameter by name (e.g. MeshAssignNode "weights").
// Returns 1 if the parameter was found and set, 0 otherwise.
EDITOR_INTERFACE u32 PCGSetNodeParamArray(u32 node_id, const char* name, const f32* values, u32 count) {
    if (!g_pcg_graph || node_id >= g_pcg_graph->GetNodes().size() || !name) return 0;
    return g_pcg_graph->GetNodes()[node_id]->SetParamArrayByName(name, values, count) ? 1u : 0u;
}

// ============================================================================
// Pin Reflection
// ============================================================================

// Returns the number of pins (input + output) exposed by the node.
EDITOR_INTERFACE u32 PCGGetNodePinCount(u32 node_id) {
    if (!g_pcg_graph || node_id >= g_pcg_graph->GetNodes().size()) return 0;
    u32 count;
    g_pcg_graph->GetNodes()[node_id]->GetPinDescriptors(count);
    return count;
}

// Returns a pointer to the pin_idx'th PCGPinDescriptor.
// is_input field distinguishes input vs output pins.
// Returns nullptr if node_id or pin_idx is out of range.
EDITOR_INTERFACE const PCGPinDescriptor* PCGGetNodePinDescriptor(u32 node_id, u32 pin_idx) {
    if (!g_pcg_graph || node_id >= g_pcg_graph->GetNodes().size()) return nullptr;
    u32 count;
    auto* descs = g_pcg_graph->GetNodes()[node_id]->GetPinDescriptors(count);
    return (pin_idx < count) ? &descs[pin_idx] : nullptr;
}

// ============================================================================
// Mesh Library
// ============================================================================

// Add a mesh slot to the graph-level mesh library.
// path: asset path (e.g. "Content/Props/Tree.model")
// name: display name (e.g. "Oak Tree")
// Returns: slot index, or u32(-1) if no graph exists.
EDITOR_INTERFACE u32 PCGAddMeshSlot(const char* path, const char* name) {
    if (!g_pcg_graph) return u32(-1);
    return g_pcg_graph->AddMeshSlot(path ? path : "", name ? name : "");
}

// Remove a mesh slot by index. Indices above shift down.
EDITOR_INTERFACE void PCGRemoveMeshSlot(u32 index) {
    if (!g_pcg_graph) return;
    g_pcg_graph->RemoveMeshSlot(index);
}

// Returns the number of mesh slots in the library.
EDITOR_INTERFACE u32 PCGGetMeshSlotCount() {
    return g_pcg_graph ? g_pcg_graph->GetMeshSlotCount() : 0;
}

// Returns a pointer to the mesh slot at index, or nullptr if out of range.
// PCGMeshSlot has .path and .name fields (C strings).
EDITOR_INTERFACE const PCGMeshSlot* PCGGetMeshSlot(u32 index) {
    if (!g_pcg_graph || index >= g_pcg_graph->GetMeshSlotCount()) return nullptr;
    return &g_pcg_graph->GetMeshSlot(index);
}

// ============================================================================
// Execution + Output
// ============================================================================

// Execute all nodes in topological order. Propagates data between connected pins.
// Check PCGGetErrorCount() after execution for any issues.
EDITOR_INTERFACE void PCGExecute() {
    if (!g_pcg_graph) return;
    g_pcg_graph->Execute();
}

// Returns the number of output points from the given node, or 0 if none.
EDITOR_INTERFACE u32 PCGGetOutputPointCount(u32 node_id) {
    if (!g_pcg_graph) return 0;
    auto* pts = g_pcg_graph->GetOutputPoints(node_id);
    return pts ? pts->count : 0;
}

// ============================================================================
// Error Reporting
// ============================================================================

// Returns the number of errors recorded during the last PCGExecute().
EDITOR_INTERFACE u32 PCGGetErrorCount() {
    return g_pcg_graph ? static_cast<u32>(g_pcg_graph->GetErrors().size()) : 0;
}

// Returns the error message for error index, or nullptr.
// Pointer is valid until the next PCGExecute() or PCGClearErrors().
EDITOR_INTERFACE const char* PCGGetErrorMessage(u32 index) {
    if (!g_pcg_graph || index >= g_pcg_graph->GetErrors().size()) return nullptr;
    return g_pcg_graph->GetErrors()[index].message.c_str();
}

// Returns the node ID associated with the error, or u32(-1) if unknown.
EDITOR_INTERFACE u32 PCGGetErrorNodeId(u32 index) {
    if (!g_pcg_graph || index >= g_pcg_graph->GetErrors().size()) return u32(-1);
    return g_pcg_graph->GetErrors()[index].node_id;
}

// Clear all recorded errors.
EDITOR_INTERFACE void PCGClearErrors() {
    if (!g_pcg_graph) return;
    g_pcg_graph->ClearErrors();
}

// ============================================================================
// Serialization
// ============================================================================

// Serialize the current graph (including mesh_slots) to JSON.
// If buf is nullptr or buf_size is 0, returns the required buffer size.
// Otherwise writes null-terminated JSON to buf, returns total length including NUL.
EDITOR_INTERFACE u32 PCGSerializeGraph(char* buf, u32 buf_size) {
    if (!g_pcg_graph) return 0;
    auto json = PCGSerializer::Serialize(*g_pcg_graph);
    if (!buf || buf_size == 0) return static_cast<u32>(json.size()) + 1;
    u32 copy_len = static_cast<u32>(json.size()) < buf_size - 1
                   ? static_cast<u32>(json.size()) : buf_size - 1;
    std::memcpy(buf, json.c_str(), copy_len);
    buf[copy_len] = '\0';
    return static_cast<u32>(json.size()) + 1;
}

// Deserialize a JSON string into the current graph. Replaces all existing
// nodes, connections, and mesh slots. Returns 1 on success, 0 on failure.
EDITOR_INTERFACE u32 PCGDeserializeGraph(const char* json) {
    if (!g_pcg_graph || !json) return 0;
    return PCGSerializer::DeserializeIntoGraph(std::string(json), *g_pcg_graph) ? 1u : 0u;
}

// ============================================================================
// Node Type Registry (palette)
// ============================================================================

// Returns the number of registered node types (currently 7).
// Use to populate an editor palette: iterate 0..count-1, call GetRegisteredNodeTypeName.
EDITOR_INTERFACE u32 PCGGetRegisteredNodeTypeCount() {
    return PCGSerializer::GetRegisteredNodeTypeCount();
}

// Returns the type name at the given index (e.g. "NoiseField" at index 1).
// Returns nullptr if index is out of range. Order is fixed:
//   0=ReferenceField, 1=NoiseField, 2=FieldScatter,
//   3=SDFConstraint, 4=DensityFilter, 5=Transform, 6=MeshAssign
EDITOR_INTERFACE const char* PCGGetRegisteredNodeTypeName(u32 index) {
    return PCGSerializer::GetRegisteredNodeTypeName(index);
}

} // extern "C"
