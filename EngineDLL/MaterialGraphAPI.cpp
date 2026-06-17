// MaterialGraph C API: thin wrapper around material_graph::MaterialGraph for P/Invoke callers.
// graph_id is 1-based (0 is the error sentinel). All functions are no-throw.

#include "Common.h"
#include "CommonHeaders.h"
#include "../Engine/Graphics/MaterialGraph/MaterialGraph.h"
#include "../Engine/Graphics/MaterialGraph/MaterialGraphSerializer.h"
#include "../Engine/Graphics/MaterialGraph/MaterialGraphBridge.h"
#include "../Engine/Components/Material.h"

#include <iostream>
#include <new>
#include <fstream>
#include <sstream>
#include <cstring>

using namespace primal;
using namespace primal::graphics;

// --- C-facing structs ---

struct material_graph_connection {
    u32 from_node, from_pin, to_node, to_pin;
};

struct material_init_info_c {
    u32 technique;
    f32 base_color[4];
    f32 roughness, metallic, alpha_cutoff;
    u64 albedo_texture, normal_texture, orm_texture;
};

namespace {

// 1-based slot map; index 0 is reserved as invalid.
utl::vector<material_graph::MaterialGraph*> graphs;

u32 slot_to_index(u32 graph_id) {
    if (graph_id == 0 || graph_id > graphs.size()) return UINT32_MAX;
    material_graph::MaterialGraph* g = graphs[graph_id - 1];
    return g ? (graph_id - 1) : UINT32_MAX;
}

} // anonymous namespace

// Public accessor for sibling API files (MaterialGraphReflectionAPI.cpp etc.)
// Declared in MaterialGraphSlotMap.h. Returns nullptr if graph_id is invalid.
material_graph::MaterialGraph* GetMaterialGraphById(u32 graph_id) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return nullptr;
    return graphs[idx];
}

extern "C" {

// --- Lifecycle ---

EDITOR_INTERFACE u32 CreateMaterialGraph() {
    auto* graph = new (std::nothrow) material_graph::MaterialGraph();
    if (!graph) {
        std::cerr << "[MaterialGraphAPI] CreateMaterialGraph: allocation failed\n";
        return 0;
    }
    graphs.push_back(graph);
    return static_cast<u32>(graphs.size());  // 1-based id
}

EDITOR_INTERFACE void DestroyMaterialGraph(u32 graph_id) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return;
    delete graphs[idx];
    graphs[idx] = nullptr;
}

EDITOR_INTERFACE void DestroyAllMaterialGraphs() {
    for (auto*& g : graphs) {
        if (g) {
            delete g;
            g = nullptr;
        }
    }
    graphs.clear();
}

EDITOR_INTERFACE u32 CloneMaterialGraph(u32 src_graph_id) {
    u32 src_idx = slot_to_index(src_graph_id);
    if (src_idx == UINT32_MAX) return 0;

    auto* src = graphs[src_idx];
    auto* dst = new (std::nothrow) material_graph::MaterialGraph();
    if (!dst) return 0;

    // Serialize source, deserialize into clone.
    std::string json = material_graph::MaterialGraphSerializer::Serialize(*src);
    if (!material_graph::MaterialGraphSerializer::DeserializeIntoGraph(json, *dst)) {
        delete dst;
        return 0;
    }

    graphs.push_back(dst);
    return static_cast<u32>(graphs.size());
}

// --- Node CRUD ---

EDITOR_INTERFACE u32 GraphAddNode(u32 graph_id, const char* type_name) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX || !type_name) return 0;

    auto node = material_graph::MaterialGraphSerializer::CreateNode(type_name);
    if (!node) {
        std::cerr << "[MaterialGraphAPI] GraphAddNode: unknown type \"" << type_name << "\"\n";
        return 0;
    }
    // AddNode returns the 0-based node_id; shift to 1-based so 0 = failure sentinel.
    u32 node_id = graphs[idx]->AddNode(std::move(node));
    return node_id + 1;
}

EDITOR_INTERFACE u32 GraphRemoveNode(u32 graph_id, u32 node_id) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return 1;  // error

    // node_id is 1-based from API; convert back to 0-based internal index.
    if (node_id == 0) return 1;
    u32 internal_id = node_id - 1;
    graphs[idx]->RemoveNode(internal_id);
    return 0;
}

EDITOR_INTERFACE u32 GraphGetNodeCount(u32 graph_id) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return 0;
    return static_cast<u32>(graphs[idx]->GetNodes().size());
}

EDITOR_INTERFACE u32 GraphGetNodeIdByIndex(u32 graph_id, u32 index) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return 0;
    const auto& nodes = graphs[idx]->GetNodes();
    if (index >= nodes.size()) return 0;
    // Node id is its vector index; shift to 1-based.
    return index + 1;
}

EDITOR_INTERFACE u32 GraphNodeGetTypeId(u32 graph_id, u32 node_id,
                                         char* out_buf, u64 buf_size) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX || !out_buf || buf_size == 0) return 1;

    if (node_id == 0) return 1;
    u32 internal_id = node_id - 1;
    const auto& nodes = graphs[idx]->GetNodes();
    if (internal_id >= nodes.size()) return 1;

    const char* name = nodes[internal_id]->TypeName();
    std::strncpy(out_buf, name, buf_size - 1);
    out_buf[buf_size - 1] = '\0';
    return 0;
}

// --- Connection CRUD ---

EDITOR_INTERFACE u32 GraphConnect(u32 graph_id, u32 from_node, u32 from_pin,
                                    u32 to_node, u32 to_pin) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return 1;

    if (from_node == 0 || to_node == 0) return 1;
    // Convert 1-based API ids to 0-based internal ids.
    graphs[idx]->Connect(from_node - 1, from_pin, to_node - 1, to_pin);
    return 0;
}

EDITOR_INTERFACE u32 GraphDisconnect(u32 graph_id, u32 from_node, u32 from_pin,
                                       u32 to_node, u32 to_pin) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return 1;

    if (from_node == 0 || to_node == 0) return 1;
    graphs[idx]->Disconnect(from_node - 1, from_pin, to_node - 1, to_pin);
    return 0;
}

EDITOR_INTERFACE u32 GraphGetConnectionCount(u32 graph_id) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return 0;
    return static_cast<u32>(graphs[idx]->GetConnections().size());
}

EDITOR_INTERFACE u32 GraphGetConnectionByIndex(u32 graph_id, u32 index,
                                                 material_graph_connection* out) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX || !out) return 1;

    const auto& conns = graphs[idx]->GetConnections();
    if (index >= conns.size()) return 1;

    const auto& c = conns[index];
    // Internal ids are 0-based; shift to 1-based for the API.
    out->from_node = c.from_node + 1;
    out->from_pin  = c.from_pin;
    out->to_node   = c.to_node + 1;
    out->to_pin    = c.to_pin;
    return 0;
}

// --- Bulk / Execute / Errors ---

EDITOR_INTERFACE void GraphClear(u32 graph_id) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return;
    graphs[idx]->Clear();
}

EDITOR_INTERFACE u32 GraphExecute(u32 graph_id) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return 0;
    graphs[idx]->Execute();
    return static_cast<u32>(graphs[idx]->GetErrors().size());
}

EDITOR_INTERFACE u32 GraphGetErrorCount(u32 graph_id) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return 0;
    return static_cast<u32>(graphs[idx]->GetErrors().size());
}

EDITOR_INTERFACE u32 GraphGetErrorByIndex(u32 graph_id, u32 index,
                                            u32* out_node_id,
                                            char* out_msg, u64 buf_size) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return 1;

    const auto& errors = graphs[idx]->GetErrors();
    if (index >= errors.size()) return 1;

    const auto& err = errors[index];
    if (out_node_id) {
        // Internal node_id is 0-based; shift to 1-based.
        *out_node_id = err.node_id + 1;
    }
    if (out_msg && buf_size > 0) {
        std::strncpy(out_msg, err.message.c_str(), buf_size - 1);
        out_msg[buf_size - 1] = '\0';
    }
    return 0;
}

EDITOR_INTERFACE void GraphClearErrors(u32 graph_id) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return;
    graphs[idx]->ClearErrors();
}

// --- JSON / File IO ---

EDITOR_INTERFACE u64 GraphGetJsonSize(u32 graph_id) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX) return 0;
    std::string json = material_graph::MaterialGraphSerializer::Serialize(*graphs[idx]);
    return static_cast<u64>(json.size() + 1);  // include null terminator
}

EDITOR_INTERFACE u32 GraphGetJson(u32 graph_id, char* out, u64 buf_size) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX || !out || buf_size == 0) return 1;

    std::string json = material_graph::MaterialGraphSerializer::Serialize(*graphs[idx]);
    u64 needed = json.size() + 1;
    if (buf_size < needed) return 1;

    std::memcpy(out, json.c_str(), needed);
    return 0;
}

EDITOR_INTERFACE u32 GraphLoadFromJson(u32 graph_id, const char* json) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX || !json) return 1;

    if (!material_graph::MaterialGraphSerializer::DeserializeIntoGraph(
            std::string(json), *graphs[idx])) {
        return 1;
    }
    return 0;
}

EDITOR_INTERFACE u32 LoadMaterialGraphFromFile(const char* path) {
    if (!path) return 0;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[MaterialGraphAPI] LoadMaterialGraphFromFile: cannot open \"" << path << "\"\n";
        return 0;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    std::string json = ss.str();

    auto* graph = new (std::nothrow) material_graph::MaterialGraph();
    if (!graph) return 0;

    if (!material_graph::MaterialGraphSerializer::DeserializeIntoGraph(json, *graph)) {
        delete graph;
        return 0;
    }

    graphs.push_back(graph);
    return static_cast<u32>(graphs.size());
}

EDITOR_INTERFACE u32 SaveMaterialGraphToFile(u32 graph_id, const char* path) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX || !path) return 1;

    std::string json = material_graph::MaterialGraphSerializer::Serialize(*graphs[idx]);

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[MaterialGraphAPI] SaveMaterialGraphToFile: cannot open \"" << path << "\"\n";
        return 1;
    }
    file.write(json.data(), static_cast<std::streamsize>(json.size()));
    return 0;
}

// --- Bridge: graph -> material init_info ---

EDITOR_INTERFACE u32 GraphEvaluateToMaterialInfo(u32 graph_id,
                                                  material_init_info_c* out) {
    u32 idx = slot_to_index(graph_id);
    if (idx == UINT32_MAX || !out) return 1;

    auto result = material_graph::MaterialGraphBridge::Evaluate(*graphs[idx]);
    if (!result.valid) return 1;

    out->technique = static_cast<u32>(result.info.technique);
    out->base_color[0] = result.info.base_color[0];
    out->base_color[1] = result.info.base_color[1];
    out->base_color[2] = result.info.base_color[2];
    out->base_color[3] = result.info.base_color[3];
    out->roughness = result.info.roughness;
    out->metallic = result.info.metallic;
    out->alpha_cutoff = result.info.alpha_cutoff;
    out->albedo_texture  = static_cast<u64>(result.info.albedo_texture);
    out->normal_texture  = static_cast<u64>(result.info.normal_texture);
    out->orm_texture     = static_cast<u64>(result.info.orm_texture);
    return 0;
}

} // extern "C"
