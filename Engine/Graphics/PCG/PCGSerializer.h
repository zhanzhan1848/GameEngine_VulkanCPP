#pragma once

#include "Graphics/PCG/PCGGraph.h"
#include "Graphics/PCG/Nodes/ReferenceFieldNode.h"
#include "Graphics/PCG/Nodes/NoiseFieldNode.h"
#include "Graphics/PCG/Nodes/FieldScatterNode.h"
#include "Graphics/PCG/Nodes/SDFConstraintNode.h"
#include "Graphics/PCG/Nodes/DensityFilterNode.h"
#include "Graphics/PCG/Nodes/TransformNode.h"
#include "Graphics/PCG/Nodes/MeshAssignNode.h"
#include "Graphics/PCG/Nodes/RasterizedFieldNode.h"
#include "Graphics/PCG/Nodes/ScatterOnGeometryNode.h"
#include "Graphics/PCG/Nodes/CurveAlignNode.h"
#include "Graphics/PCG/Nodes/SurfaceScatterNode.h"
#include "Graphics/PCG/Nodes/MarchingCubesNode.h"
#include <sstream>
#include <string>

namespace primal::graphics::pcg {

// Serialize/deserialize PCGGraph to/from JSON format.
// Lightweight, no external JSON library dependency.
//
// JSON format (Phase 2.5+):
// {
//   "mesh_slots": [                              // optional, graph-level mesh library
//     {"path": "Content/Props/Tree.model", "name": "Oak Tree"},
//     {"path": "Content/Props/Rock.model", "name": "Granite"}
//   ],
//   "nodes": [
//     {"type": "ReferenceField", "params": {"voxel_size": 0.5}},
//     {"type": "NoiseField", "params": {"frequency": 0.05, "octaves": 4, "seed": 123}},
//     {"type": "FieldScatter", "params": {"target_count": 1000, "bounds_min": [-50,0,-50]}},
//     {"type": "MeshAssign", "params": {"weights": [0.7, 0.3]}}
//   ],
//   "connections": [
//     {"from_node": 1, "from_pin": 0, "to_node": 2, "to_pin": 0}
//   ]
// }
//
// Vec3 params serialized as "name": [x, y, z]
// Float arrays (weights) serialized as "name": [w0, w1, ...]
// Parameter serialization is descriptor-driven: iterates each node's
// GetParamDescriptors() and reads values via offsetof, so new nodes
// are automatically supported without modifying this file.
class PCGSerializer {
public:
    // Serialize a full graph to JSON string.
    static std::string Serialize(const PCGGraph& graph) {
        std::string json = "{";

        // Mesh slots
        auto slot_count = graph.GetMeshSlotCount();
        if (slot_count > 0) {
            json += "\"mesh_slots\":[";
            for (u32 i = 0; i < slot_count; ++i) {
                if (i > 0) json += ",";
                const auto& slot = graph.GetMeshSlot(i);
                json += "{\"path\":\"" + EscapeString(slot.path)
                     + "\",\"name\":\"" + EscapeString(slot.name) + "\"}";
            }
            json += "],";
        }

        json += "\"nodes\":[";
        auto& nodes = graph.GetNodes();
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (i > 0) json += ",";
            json += "{" + SerializeNode(*nodes[i]) + "}";
        }
        json += "],\"connections\":[";
        auto& conns = graph.GetConnections();
        for (size_t i = 0; i < conns.size(); ++i) {
            if (i > 0) json += ",";
            auto& c = conns[i];
            json += "{\"from_node\":" + std::to_string(c.from_node)
                 + ",\"from_pin\":" + std::to_string(c.from_pin)
                 + ",\"to_node\":" + std::to_string(c.to_node)
                 + ",\"to_pin\":" + std::to_string(c.to_pin) + "}";
        }
        json += "]}";
        return json;
    }

    // Serialize node parameters to a JSON-like string fragment.
    // Returns the content between { } for a single node.
    static std::string SerializeNode(const PCGNode& node) {
        std::ostringstream ss;
        ss << "\"type\": \"" << node.TypeName() << "\"";

        std::string params = SerializeParams(node);
        if (!params.empty()) {
            ss << ", \"params\": {" << params << "}";
        }
        return ss.str();
    }

    // Create a node from its type name. Returns nullptr for unknown types.
    static std::unique_ptr<PCGNode> CreateNode(const char* type_name) {
        std::string t(type_name);
        if (t == "ReferenceField") return std::make_unique<ReferenceFieldNode>();
        if (t == "NoiseField")     return std::make_unique<NoiseFieldNode>();
        if (t == "FieldScatter")   return std::make_unique<FieldScatterNode>();
        if (t == "SDFConstraint")  return std::make_unique<SDFConstraintNode>();
        if (t == "DensityFilter")  return std::make_unique<DensityFilterNode>();
        if (t == "Transform")      return std::make_unique<TransformNode>();
        if (t == "MeshAssign")     return std::make_unique<MeshAssignNode>();
        if (t == "RasterizedField") return std::make_unique<RasterizedFieldNode>();
        if (t == "ScatterOnGeometry") return std::make_unique<ScatterOnGeometryNode>();
        if (t == "CurveAlign")       return std::make_unique<CurveAlignNode>();
        if (t == "SurfaceScatter")  return std::make_unique<SurfaceScatterNode>();
        if (t == "MarchingCubes")   return std::make_unique<MarchingCubesNode>();
        return nullptr;
    }

    // Apply a named float parameter to a node (delegates to node's virtual method).
    static bool SetParam(PCGNode* node, const char* name, f32 value) {
        return node->SetParamByName(name, value);
    }

    // Apply a named vec3 parameter to a node.
    static bool SetParamVec3(PCGNode* node, const char* name, math::v3 value) {
        return node->SetParamByName(name, value);
    }

    // Apply a float array parameter to a node.
    static bool SetParamArray(PCGNode* node, const char* name, const std::vector<f32>& values) {
        return node->SetParamArrayByName(name, values.data(), static_cast<u32>(values.size()));
    }

    // Deserialize a complete JSON graph into an existing PCGGraph object.
    // Clears the graph first, then populates mesh_slots, nodes, and connections.
    // Returns true if at least nodes were parsed, false on parse failure.
    // This is the recommended deserialization API — handles the full Phase 2.5 format.
    static bool DeserializeIntoGraph(const std::string& json, PCGGraph& graph) {
        graph.Clear();

        // Parse mesh_slots
        auto ms_start = json.find("\"mesh_slots\"");
        if (ms_start != std::string::npos) {
            auto arr_start = json.find('[', ms_start);
            auto arr_end = FindMatchingBracket(json, arr_start);
            if (arr_start != std::string::npos && arr_end != std::string::npos) {
                std::string arr = json.substr(arr_start, arr_end - arr_start + 1);
                size_t pos = 1;
                while (pos < arr.size()) {
                    auto obj_start = arr.find('{', pos);
                    if (obj_start == std::string::npos) break;
                    auto obj_end = FindMatchingBrace(arr, obj_start);
                    if (obj_end == std::string::npos) break;
                    std::string obj = arr.substr(obj_start + 1, obj_end - obj_start - 1);
                    std::string path = ExtractStringValue(obj, "path");
                    std::string name = ExtractStringValue(obj, "name");
                    graph.AddMeshSlot(path.c_str(), name.c_str());
                    pos = obj_end + 1;
                }
            }
        }

        // Parse nodes
        auto nodes_start = json.find("\"nodes\"");
        if (nodes_start == std::string::npos) return false;

        auto nodes_arr_start = json.find('[', nodes_start);
        auto nodes_arr_end = FindMatchingBracket(json, nodes_arr_start);
        if (nodes_arr_start == std::string::npos || nodes_arr_end == std::string::npos) return false;

        std::string nodes_str = json.substr(nodes_arr_start, nodes_arr_end - nodes_arr_start + 1);
        std::vector<std::unique_ptr<PCGNode>> temp_nodes;
        ParseNodesArray(nodes_str, temp_nodes);
        for (auto& n : temp_nodes) {
            graph.AddNode(std::move(n));
        }

        // Parse connections
        auto conns_start = json.find("\"connections\"");
        if (conns_start != std::string::npos) {
            auto conns_arr_start = json.find('[', conns_start);
            auto conns_arr_end = FindMatchingBracket(json, conns_arr_start);
            if (conns_arr_start != std::string::npos && conns_arr_end != std::string::npos) {
                std::string conns_str = json.substr(conns_arr_start, conns_arr_end - conns_arr_start + 1);
                std::vector<std::array<u32, 4>> temp_conns;
                ParseConnectionsArray(conns_str, temp_conns);
                for (auto& c : temp_conns) {
                    graph.Connect(c[0], c[1], c[2], c[3]);
                }
            }
        }

        return true;
    }

    // Deserialize a JSON graph definition (legacy API — does not handle mesh_slots).
    // Outputs raw nodes and connection arrays for manual graph assembly.
    // Prefer DeserializeIntoGraph() for new code.
    static bool DeserializeGraph(const std::string& json,
                                  std::vector<std::unique_ptr<PCGNode>>& out_nodes,
                                  std::vector<std::array<u32, 4>>& out_connections) {
        auto nodes_start = json.find("\"nodes\"");
        auto conns_start = json.find("\"connections\"");

        if (nodes_start == std::string::npos) return false;

        auto nodes_arr_start = json.find('[', nodes_start);
        auto nodes_arr_end = FindMatchingBracket(json, nodes_arr_start);
        if (nodes_arr_start == std::string::npos || nodes_arr_end == std::string::npos) return false;

        std::string nodes_str = json.substr(nodes_arr_start, nodes_arr_end - nodes_arr_start + 1);
        ParseNodesArray(nodes_str, out_nodes);

        if (conns_start != std::string::npos) {
            auto conns_arr_start = json.find('[', conns_start);
            auto conns_arr_end = FindMatchingBracket(json, conns_arr_start);
            if (conns_arr_start != std::string::npos && conns_arr_end != std::string::npos) {
                std::string conns_str = json.substr(conns_arr_start, conns_arr_end - conns_arr_start + 1);
                ParseConnectionsArray(conns_str, out_connections);
            }
        }

        return !out_nodes.empty();
    }

    // --- Node Type Registry (for editor palette) ---
    // Returns the number and names of all registered node types.
    // Use to populate an editor node palette: iterate 0..count-1, display names,
    // create nodes via CreateNode().

    static u32 GetRegisteredNodeTypeCount() { return 12; }

    // Returns the type name at index (fixed order), or nullptr if out of range.
    // Order: 0=ReferenceField, 1=NoiseField, 2=FieldScatter,
    //        3=SDFConstraint, 4=DensityFilter, 5=Transform, 6=MeshAssign,
    //        7=RasterizedField, 8=ScatterOnGeometry, 9=CurveAlign,
    //        10=SurfaceScatter, 11=MarchingCubes
    static const char* GetRegisteredNodeTypeName(u32 index) {
        static const char* kNames[] = {
            "ReferenceField", "NoiseField", "FieldScatter",
            "SDFConstraint", "DensityFilter", "Transform", "MeshAssign",
            "RasterizedField", "ScatterOnGeometry", "CurveAlign", "SurfaceScatter",
            "MarchingCubes"
        };
        return index < 12 ? kNames[index] : nullptr;
    }

private:
    static std::string EscapeString(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out;
    }
    static std::string SerializeParams(const PCGNode& node) {
        std::ostringstream ss;
        u32 count;
        auto* descs = node.GetParamDescriptors(count);
        if (!descs || count == 0) return "";

        bool first = true;
        for (u32 i = 0; i < count; ++i) {
            const auto& d = descs[i];
            if (!first) ss << ", ";
            first = false;

            const auto* base = reinterpret_cast<const char*>(&node);

            switch (d.type) {
            case PCGParamType::Float: {
                f32 val = *reinterpret_cast<const f32*>(base + d.offset);
                ss << "\"" << d.name << "\": " << val;
                break;
            }
            case PCGParamType::UInt:
            case PCGParamType::Int: {
                u32 val = *reinterpret_cast<const u32*>(base + d.offset);
                ss << "\"" << d.name << "\": " << static_cast<f32>(val);
                break;
            }
            case PCGParamType::Enum: {
                // Enums may be u8 or u32 — read based on stored size
                f32 val;
                if (d.size == 1) val = static_cast<f32>(*reinterpret_cast<const u8*>(base + d.offset));
                else val = static_cast<f32>(*reinterpret_cast<const u32*>(base + d.offset));
                ss << "\"" << d.name << "\": " << val;
                break;
            }
            case PCGParamType::Bool: {
                bool val = *reinterpret_cast<const bool*>(base + d.offset);
                ss << "\"" << d.name << "\": " << (val ? "true" : "false");
                break;
            }
            case PCGParamType::Vec3: {
                auto* v = reinterpret_cast<const math::v3*>(base + d.offset);
                ss << "\"" << d.name << "\": [" << v->x << ", " << v->y << ", " << v->z << "]";
                break;
            }
            case PCGParamType::FloatArray:
                // FloatArray is stored in std::vector, not at a fixed offset.
                // Special handling per node type.
                if (strcmp(node.TypeName(), "MeshAssign") == 0 && strcmp(d.name, "weights") == 0) {
                    const auto& weights = static_cast<const MeshAssignNode&>(node).weights;
                    ss << "\"" << d.name << "\": [";
                    for (size_t j = 0; j < weights.size(); ++j) {
                        if (j > 0) ss << ", ";
                        ss << weights[j];
                    }
                    ss << "]";
                }
                break;
            }
        }
        return ss.str();
    }

    static size_t FindMatchingBracket(const std::string& s, size_t start) {
        if (start >= s.size() || s[start] != '[') return std::string::npos;
        int depth = 0;
        for (size_t i = start; i < s.size(); ++i) {
            if (s[i] == '[') depth++;
            else if (s[i] == ']') { depth--; if (depth == 0) return i; }
        }
        return std::string::npos;
    }

    static size_t FindMatchingBrace(const std::string& s, size_t start) {
        if (start >= s.size() || s[start] != '{') return std::string::npos;
        int depth = 0;
        for (size_t i = start; i < s.size(); ++i) {
            if (s[i] == '{') depth++;
            else if (s[i] == '}') { depth--; if (depth == 0) return i; }
        }
        return std::string::npos;
    }

    static void ParseNodesArray(const std::string& arr,
                                 std::vector<std::unique_ptr<PCGNode>>& out_nodes) {
        size_t pos = 1;
        while (pos < arr.size()) {
            auto obj_start = arr.find('{', pos);
            if (obj_start == std::string::npos) break;
            auto obj_end = FindMatchingBrace(arr, obj_start);
            if (obj_end == std::string::npos) break;

            std::string obj = arr.substr(obj_start + 1, obj_end - obj_start - 1);
            std::string type = ExtractStringValue(obj, "type");

            auto node = CreateNode(type.c_str());
            if (node) {
                auto params_start = obj.find("\"params\"");
                if (params_start != std::string::npos) {
                    auto p_open = obj.find('{', params_start);
                    auto p_close = FindMatchingBrace(obj, p_open);
                    if (p_open != std::string::npos && p_close != std::string::npos) {
                        std::string params = obj.substr(p_open + 1, p_close - p_open - 1);
                        ApplyParams(node.get(), params);
                    }
                }
                out_nodes.push_back(std::move(node));
            }

            pos = obj_end + 1;
        }
    }

    static void ParseConnectionsArray(const std::string& arr,
                                        std::vector<std::array<u32, 4>>& out_connections) {
        size_t pos = 1;
        while (pos < arr.size()) {
            auto obj_start = arr.find('{', pos);
            if (obj_start == std::string::npos) break;
            auto obj_end = FindMatchingBrace(arr, obj_start);
            if (obj_end == std::string::npos) break;

            std::string obj = arr.substr(obj_start + 1, obj_end - obj_start - 1);
            u32 from_node = ExtractIntValue(obj, "from_node");
            u32 from_pin  = ExtractIntValue(obj, "from_pin");
            u32 to_node   = ExtractIntValue(obj, "to_node");
            u32 to_pin    = ExtractIntValue(obj, "to_pin");
            out_connections.push_back({from_node, from_pin, to_node, to_pin});

            pos = obj_end + 1;
        }
    }

    static std::string ExtractStringValue(const std::string& obj, const std::string& key) {
        auto pos = obj.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        auto colon = obj.find(':', pos);
        if (colon == std::string::npos) return "";
        auto q1 = obj.find('"', colon);
        if (q1 == std::string::npos) return "";
        auto q2 = obj.find('"', q1 + 1);
        if (q2 == std::string::npos) return "";
        return obj.substr(q1 + 1, q2 - q1 - 1);
    }

    static u32 ExtractIntValue(const std::string& obj, const std::string& key) {
        auto pos = obj.find("\"" + key + "\"");
        if (pos == std::string::npos) return 0;
        auto colon = obj.find(':', pos);
        if (colon == std::string::npos) return 0;
        std::string val;
        for (size_t i = colon + 1; i < obj.size(); ++i) {
            char c = obj[i];
            if ((c >= '0' && c <= '9') || c == '-') val += c;
            else if (!val.empty()) break;
        }
        return val.empty() ? 0 : static_cast<u32>(std::stoi(val));
    }

    static void ApplyParams(PCGNode* node, const std::string& params) {
        size_t pos = 0;
        while (pos < params.size()) {
            // Skip whitespace
            while (pos < params.size() && (params[pos] == ' ' || params[pos] == '\n' || params[pos] == '\r' || params[pos] == '\t'))
                pos++;
            if (pos >= params.size()) break;

            // Expect key in quotes
            if (params[pos] != '"') { pos++; continue; }
            auto q1 = pos;
            auto q2 = params.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string key = params.substr(q1 + 1, q2 - q1 - 1);

            auto colon = params.find(':', q2);
            if (colon == std::string::npos) break;

            size_t val_start = colon + 1;
            // Skip whitespace
            while (val_start < params.size() && params[val_start] == ' ')
                val_start++;

            if (val_start >= params.size()) break;

            // Check if value is an array [...]
            if (params[val_start] == '[') {
                auto arr_end = FindMatchingBracket(params, val_start);
                if (arr_end == std::string::npos) break;
                std::string arr_str = params.substr(val_start + 1, arr_end - val_start - 1);
                auto arr_vals = ParseFloatArray(arr_str);

                // Try vec3 first (for 3-element arrays that match a known vec3 param)
                bool applied = false;
                if (arr_vals.size() == 3) {
                    applied = SetParamVec3(node, key.c_str(), {arr_vals[0], arr_vals[1], arr_vals[2]});
                }
                // If vec3 didn't match, try as generic float array
                if (!applied && !arr_vals.empty()) {
                    applied = SetParamArray(node, key.c_str(), arr_vals);
                }

                pos = arr_end + 1;
                auto comma = params.find(',', pos);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            } else {
                // Scalar float value
                std::string val_str;
                for (size_t i = val_start; i < params.size(); ++i) {
                    char c = params[i];
                    if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == 'E' || c == '+')
                        val_str += c;
                    else if (!val_str.empty()) break;
                }

                if (!val_str.empty()) {
                    f32 value = std::stof(val_str);
                    SetParam(node, key.c_str(), value);
                }

                auto comma = params.find(',', val_start);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
    }

    // Parse "x, y, z" as a v3. Returns {true, v3} if 3 values found.
    static std::pair<bool, math::v3> ParseVec3(const std::string& s) {
        auto vals = ParseFloatArray(s);
        if (vals.size() == 3) {
            return {true, math::v3{vals[0], vals[1], vals[2]}};
        }
        return {false, math::v3{}};
    }

    static std::vector<f32> ParseFloatArray(const std::string& s) {
        std::vector<f32> result;
        size_t pos = 0;
        while (pos < s.size()) {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == ',' || s[pos] == '\t'))
                pos++;
            if (pos >= s.size()) break;

            std::string val_str;
            for (size_t i = pos; i < s.size(); ++i) {
                char c = s[i];
                if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == 'E' || c == '+')
                    val_str += c;
                else break;
            }

            if (!val_str.empty()) {
                result.push_back(std::stof(val_str));
                pos += val_str.size();
            } else {
                pos++;
            }
        }
        return result;
    }
};

} // namespace primal::graphics::pcg
