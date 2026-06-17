// MaterialGraphReflectionAPI: C ABI for node type registry reflection + per-node pin/param queries.
// Separate from MaterialGraphAPI.cpp to avoid merge conflicts with parallel work (Agent 1).
// Cross-module graph lookup uses GetMaterialGraphById() declared in MaterialGraphSlotMap.h
// and implemented in MaterialGraphAPI.cpp (Agent 1's file).
//
// Convention: graph_id and node_id are 1-based (0 = invalid sentinel), matching
// MaterialGraphAPI.cpp's convention. Internally we convert to 0-based vector indices.

#include "Common.h"
#include "CommonHeaders.h"

#include "../Engine/Graphics/MaterialGraph/MaterialGraph.h"
#include "../Engine/Graphics/MaterialGraph/MaterialNode.h"
#include "../Engine/Graphics/MaterialGraph/MaterialGraphSerializer.h"
#include "../Engine/Graphics/MaterialGraph/MaterialGraphTypes.h"
#include "../Engine/Graphics/MaterialGraph/MaterialGraphReflection.h"
#include "MaterialGraphSlotMap.h"

#include <cstring>
#include <iostream>
#include <new>

using namespace primal;
using namespace primal::graphics::material_graph;

// ============================================================================
// C struct types returned to C# (mirrors engine structs in a POD-friendly way)
// ============================================================================

typedef struct material_pin_desc_c {
    u32 data_type;     // MaterialDataType enum value
    u32 is_input;
} material_pin_desc_c;

typedef struct material_param_desc_c {
    u32 type;          // MaterialParamType enum value
    f32 min_val, max_val, step;
    u32 offset, size;
    u32 enum_name_count;
} material_param_desc_c;

// ============================================================================
// Helpers
// ============================================================================

// Convert 1-based API node_id to 0-based internal index. Returns UINT32_MAX if invalid.
static inline u32 node_id_to_index(u32 node_id, size_t node_count) {
    if (node_id == 0) return UINT32_MAX;
    u32 idx = node_id - 1;
    return (idx < node_count) ? idx : UINT32_MAX;
}

// Resolve graph from the shared slot map.
static inline MaterialGraph* resolve_graph(u32 graph_id) {
    return GetMaterialGraphById(graph_id);
}

extern "C" {

// --- Registry reflection (no graph_id needed) ---

EDITOR_INTERFACE u32 GetRegisteredMaterialNodeTypeCount() {
    return MaterialGraphSerializer::GetRegisteredNodeTypeCount();
}

EDITOR_INTERFACE u32 GetRegisteredMaterialNodeTypeInfo(u32 index,
                                                        char* out_type_name,    u64 name_buf,
                                                        char* out_display_name, u64 disp_buf,
                                                        char* out_category,     u64 cat_buf,
                                                        u32* out_is_terminal) {
    if (index >= MaterialGraphSerializer::GetRegisteredNodeTypeCount()) return 0;

    auto info = MaterialGraphSerializer::GetRegisteredNodeTypeInfo(index);

    if (out_type_name && name_buf > 0) {
        std::strncpy(out_type_name, info.type_name, name_buf - 1);
        out_type_name[name_buf - 1] = '\0';
    }
    if (out_display_name && disp_buf > 0) {
        std::strncpy(out_display_name, info.display_name, disp_buf - 1);
        out_display_name[disp_buf - 1] = '\0';
    }
    if (out_category && cat_buf > 0) {
        std::strncpy(out_category, info.category, cat_buf - 1);
        out_category[cat_buf - 1] = '\0';
    }
    if (out_is_terminal) {
        *out_is_terminal = info.is_terminal ? 1u : 0u;
    }
    return 1u;
}

// --- Per-node pin reflection ---

EDITOR_INTERFACE u32 GraphNodeGetPinCount(u32 graph_id, u32 node_id) {
    auto* graph = resolve_graph(graph_id);
    if (!graph) return 0;
    auto& nodes = graph->GetNodes();
    u32 idx = node_id_to_index(node_id, nodes.size());
    if (idx == UINT32_MAX) return 0;
    u32 count = 0;
    nodes[idx]->GetPinDescriptors(count);
    return count;
}

EDITOR_INTERFACE u32 GraphNodeGetPinDescriptor(u32 graph_id, u32 node_id, u32 pin_index,
                                                material_pin_desc_c* out,
                                                char* out_name, u64 buf_size) {
    auto* graph = resolve_graph(graph_id);
    if (!graph || !out) return 0;
    auto& nodes = graph->GetNodes();
    u32 idx = node_id_to_index(node_id, nodes.size());
    if (idx == UINT32_MAX) return 0;

    u32 count = 0;
    auto* pins = nodes[idx]->GetPinDescriptors(count);
    if (pin_index >= count || !pins) return 0;

    const auto& p = pins[pin_index];
    out->data_type = static_cast<u32>(p.data_type);
    out->is_input  = p.is_input ? 1u : 0u;

    if (out_name && buf_size > 0) {
        std::strncpy(out_name, p.name, buf_size - 1);
        out_name[buf_size - 1] = '\0';
    }
    return 1u;
}

// --- Per-node param reflection ---

EDITOR_INTERFACE u32 GraphNodeGetParamCount(u32 graph_id, u32 node_id) {
    auto* graph = resolve_graph(graph_id);
    if (!graph) return 0;
    auto& nodes = graph->GetNodes();
    u32 idx = node_id_to_index(node_id, nodes.size());
    if (idx == UINT32_MAX) return 0;
    u32 count = 0;
    nodes[idx]->GetParamDescriptors(count);
    return count;
}

EDITOR_INTERFACE u32 GraphNodeGetParamDescriptor(u32 graph_id, u32 node_id, u32 param_index,
                                                  material_param_desc_c* out,
                                                  char* out_name,  u64 name_buf,
                                                  char* out_group, u64 group_buf) {
    auto* graph = resolve_graph(graph_id);
    if (!graph || !out) return 0;
    auto& nodes = graph->GetNodes();
    u32 idx = node_id_to_index(node_id, nodes.size());
    if (idx == UINT32_MAX) return 0;

    u32 count = 0;
    auto* params = nodes[idx]->GetParamDescriptors(count);
    if (param_index >= count || !params) return 0;

    const auto& d = params[param_index];
    out->type           = static_cast<u32>(d.type);
    out->min_val        = d.range.min_val;
    out->max_val        = d.range.max_val;
    out->step           = d.range.step;
    out->offset         = d.offset;
    out->size           = d.size;
    out->enum_name_count = 0; // Enum names not yet supported in this version

    if (out_name && name_buf > 0) {
        std::strncpy(out_name, d.name, name_buf - 1);
        out_name[name_buf - 1] = '\0';
    }
    if (out_group && group_buf > 0) {
        std::strncpy(out_group, d.group, group_buf - 1);
        out_group[group_buf - 1] = '\0';
    }
    return 1u;
}

EDITOR_INTERFACE u32 GraphNodeGetParamEnumName(u32 graph_id, u32 node_id,
                                                u32 param_index, u32 enum_index,
                                                char* out_buf, u64 buf_size) {
    (void)graph_id; (void)node_id; (void)param_index; (void)enum_index;
    (void)out_buf; (void)buf_size;
    // Enum params not yet used by any node type. Return 0 = not supported.
    return 0;
}

// --- Per-node param get/set ---

EDITOR_INTERFACE u32 GraphNodeSetParamFloat(u32 g, u32 n, const char* name, f32 v) {
    auto* graph = resolve_graph(g);
    if (!graph || !name) return 0;
    auto& nodes = graph->GetNodes();
    u32 idx = node_id_to_index(n, nodes.size());
    if (idx == UINT32_MAX) return 0;
    return nodes[idx]->SetParamByName(name, v) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GraphNodeSetParamFloat2(u32 g, u32 n, const char* name, const f32* v) {
    auto* graph = resolve_graph(g);
    if (!graph || !name || !v) return 0;
    auto& nodes = graph->GetNodes();
    u32 idx = node_id_to_index(n, nodes.size());
    if (idx == UINT32_MAX) return 0;
    // No v2 overload exists on MaterialNode. Float2 params (ConstantFloat2Node)
    // don't have a SetParamByName override, so this will return 0 for them.
    // We use v3 with z=0 as a best-effort fallback.
    math::v3 val{v[0], v[1], 0.0f};
    return nodes[idx]->SetParamByName(name, val) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GraphNodeSetParamFloat3(u32 g, u32 n, const char* name, const f32* v) {
    auto* graph = resolve_graph(g);
    if (!graph || !name || !v) return 0;
    auto& nodes = graph->GetNodes();
    u32 idx = node_id_to_index(n, nodes.size());
    if (idx == UINT32_MAX) return 0;
    math::v3 val{v[0], v[1], v[2]};
    return nodes[idx]->SetParamByName(name, val) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GraphNodeSetParamFloat4(u32 g, u32 n, const char* name, const f32* v) {
    auto* graph = resolve_graph(g);
    if (!graph || !name || !v) return 0;
    auto& nodes = graph->GetNodes();
    u32 idx = node_id_to_index(n, nodes.size());
    if (idx == UINT32_MAX) return 0;
    math::v4 val{v[0], v[1], v[2], v[3]};
    return nodes[idx]->SetParamByName(name, val) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GraphNodeSetParamString(u32 g, u32 n, const char* name, const char* v) {
    auto* graph = resolve_graph(g);
    if (!graph || !name || !v) return 0;
    auto& nodes = graph->GetNodes();
    u32 idx = node_id_to_index(n, nodes.size());
    if (idx == UINT32_MAX) return 0;
    return nodes[idx]->SetParamByName(name, v) ? 1u : 0u;
}

EDITOR_INTERFACE u32 GraphNodeGetParamFloat(u32 g, u32 n, const char* name, f32* out) {
    auto* graph = resolve_graph(g);
    if (!graph || !name || !out) return 0;
    auto& nodes = graph->GetNodes();
    u32 idx = node_id_to_index(n, nodes.size());
    if (idx == UINT32_MAX) return 0;

    u32 count = 0;
    auto* descs = nodes[idx]->GetParamDescriptors(count);
    for (u32 i = 0; i < count; ++i) {
        if (std::strcmp(descs[i].name, name) == 0) {
            if (descs[i].type != MaterialParamType::Float) return 0;
            const auto* base = reinterpret_cast<const char*>(nodes[idx].get());
            const auto* ptr = reinterpret_cast<const f32*>(base + descs[i].offset);
            *out = *ptr;
            return 1u;
        }
    }
    return 0;
}

EDITOR_INTERFACE u32 GraphNodeGetParamFloat4(u32 g, u32 n, const char* name, f32* out) {
    auto* graph = resolve_graph(g);
    if (!graph || !name || !out) return 0;
    auto& nodes = graph->GetNodes();
    u32 idx = node_id_to_index(n, nodes.size());
    if (idx == UINT32_MAX) return 0;

    u32 count = 0;
    auto* descs = nodes[idx]->GetParamDescriptors(count);
    for (u32 i = 0; i < count; ++i) {
        if (std::strcmp(descs[i].name, name) == 0) {
            if (descs[i].type != MaterialParamType::Float4) return 0;
            const auto* base = reinterpret_cast<const char*>(nodes[idx].get());
            const auto* ptr = reinterpret_cast<const f32*>(base + descs[i].offset);
            out[0] = ptr[0]; out[1] = ptr[1]; out[2] = ptr[2]; out[3] = ptr[3];
            return 1u;
        }
    }
    return 0;
}

EDITOR_INTERFACE u32 GraphNodeGetParamString(u32 g, u32 n, const char* name,
                                             char* out, u64 buf_size) {
    auto* graph = resolve_graph(g);
    if (!graph || !name || !out) return 0;
    auto& nodes = graph->GetNodes();
    u32 idx = node_id_to_index(n, nodes.size());
    if (idx == UINT32_MAX) return 0;

    // Only ConstantTextureNode has a string param ("asset_path").
    // std::string member offset via reflection is unreliable; cast directly by type.
    if (std::strcmp(nodes[idx]->TypeName(), "ConstantTexture") == 0) {
        auto* tex_node = static_cast<ConstantTextureNode*>(nodes[idx].get());
        if (std::strcmp(name, "asset_path") == 0) {
            std::strncpy(out, tex_node->asset_path.c_str(), buf_size - 1);
            out[buf_size - 1] = '\0';
            return 1u;
        }
    }
    return 0;
}

} // extern "C"
