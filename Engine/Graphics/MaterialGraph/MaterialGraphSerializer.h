#pragma once

#include "Graphics/MaterialGraph/MaterialGraph.h"
#include "Graphics/MaterialGraph/Nodes/ConstantNode.h"
#include "Graphics/MaterialGraph/Nodes/TimeNode.h"
#include "Graphics/MaterialGraph/Nodes/UVNode.h"
#include "Graphics/MaterialGraph/Nodes/MathNodes.h"
#include "Graphics/MaterialGraph/Nodes/FlowNodes.h"
#include "Graphics/MaterialGraph/Nodes/TextureNodes.h"
#include "Graphics/MaterialGraph/Nodes/CurveNode.h"
#include "Graphics/MaterialGraph/Nodes/UtilityNodes.h"
#include "Graphics/MaterialGraph/Nodes/MaterialOutputNode.h"
#include <sstream>
#include <string>

namespace primal::graphics::material_graph {

class MaterialGraphSerializer {
public:
    static std::string Serialize(const MaterialGraph& graph) {
        std::string json = "{\"nodes\":[";
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

    static std::string SerializeNode(const MaterialNode& node) {
        std::ostringstream ss;
        ss << "\"type\": \"" << node.TypeName() << "\"";
        std::string params = SerializeParams(node);
        if (!params.empty()) {
            ss << ", \"params\": {" << params << "}";
        }
        return ss.str();
    }

    static std::unique_ptr<MaterialNode> CreateNode(const char* type_name) {
        std::string t(type_name);
        if (t == "ConstantFloat")     return std::make_unique<ConstantFloatNode>();
        if (t == "ConstantFloat2")    return std::make_unique<ConstantFloat2Node>();
        if (t == "ConstantFloat3")    return std::make_unique<ConstantFloat3Node>();
        if (t == "ConstantFloat4")    return std::make_unique<ConstantFloat4Node>();
        if (t == "ConstantTexture")   return std::make_unique<ConstantTextureNode>();
        if (t == "Time")              return std::make_unique<TimeNode>();
        if (t == "UV")                return std::make_unique<UVNode>();
        if (t == "Add")               return std::make_unique<AddNode>();
        if (t == "Multiply")          return std::make_unique<MultiplyNode>();
        if (t == "Lerp")              return std::make_unique<LerpNode>();
        if (t == "Clamp")             return std::make_unique<ClampNode>();
        if (t == "Pow")               return std::make_unique<PowNode>();
        if (t == "Saturate")          return std::make_unique<SaturateNode>();
        if (t == "Fresnel")           return std::make_unique<FresnelNode>();
        if (t == "NormalBlend")       return std::make_unique<NormalBlendNode>();
        if (t == "Select")            return std::make_unique<SelectNode>();
        if (t == "Remap")             return std::make_unique<RemapNode>();
        if (t == "SampleTexture")     return std::make_unique<SampleTextureNode>();
        if (t == "Curve")             return std::make_unique<CurveNode>();
        if (t == "MaterialOutput")    return std::make_unique<MaterialOutputNode>();
        return nullptr;
    }

    static u32 GetRegisteredNodeTypeCount() { return 20; }

    static const char* GetRegisteredNodeTypeName(u32 index) {
        static const char* kNames[] = {
            "ConstantFloat", "ConstantFloat2", "ConstantFloat3", "ConstantFloat4", "ConstantTexture",
            "Time", "UV",
            "Add", "Multiply", "Lerp", "Clamp", "Pow", "Saturate",
            "Fresnel", "NormalBlend",
            "Select", "Remap", "SampleTexture", "Curve",
            "MaterialOutput"
        };
        return index < 20 ? kNames[index] : nullptr;
    }

    static NodeTypeInfo GetRegisteredNodeTypeInfo(u32 index) {
        if (index >= 20) {
            return {"", "", "", false};
        }
        auto node = CreateNode(GetRegisteredNodeTypeName(index));
        if (!node) {
            return {"", "", "", false};
        }
        return node->GetTypeInfo();
    }

    static bool DeserializeIntoGraph(const std::string& json, MaterialGraph& graph) {
        graph.Clear();

        auto nodes_start = json.find("\"nodes\"");
        if (nodes_start == std::string::npos) return false;

        auto nodes_arr_start = json.find('[', nodes_start);
        auto nodes_arr_end = FindMatchingBracket(json, nodes_arr_start);
        if (nodes_arr_start == std::string::npos || nodes_arr_end == std::string::npos) return false;

        std::string nodes_str = json.substr(nodes_arr_start, nodes_arr_end - nodes_arr_start + 1);
        std::vector<std::unique_ptr<MaterialNode>> temp_nodes;
        ParseNodesArray(nodes_str, temp_nodes);
        for (auto& n : temp_nodes) {
            graph.AddNode(std::move(n));
        }

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

private:
    static std::string SerializeParams(const MaterialNode& node) {
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
            case MaterialParamType::Float: {
                f32 val = *reinterpret_cast<const f32*>(base + d.offset);
                ss << "\"" << d.name << "\": " << val;
                break;
            }
            case MaterialParamType::Bool: {
                bool val = *reinterpret_cast<const bool*>(base + d.offset);
                ss << "\"" << d.name << "\": " << (val ? "true" : "false");
                break;
            }
            case MaterialParamType::Float3: {
                auto* v = reinterpret_cast<const math::v3*>(base + d.offset);
                ss << "\"" << d.name << "\": [" << v->x << ", " << v->y << ", " << v->z << "]";
                break;
            }
            case MaterialParamType::Float4: {
                auto* v = reinterpret_cast<const math::v4*>(base + d.offset);
                ss << "\"" << d.name << "\": [" << v->x << ", " << v->y << ", " << v->z << ", " << v->w << "]";
                break;
            }
            case MaterialParamType::Texture: {
                std::string val;
                if (std::strcmp(node.TypeName(), "ConstantTexture") == 0) {
                    val = static_cast<const ConstantTextureNode&>(node).asset_path;
                }
                ss << "\"" << d.name << "\": \"" << val << "\"";
                break;
            }
            case MaterialParamType::Curve: {
                if (std::strcmp(node.TypeName(), "Curve") == 0) {
                    auto& cn = static_cast<const CurveNode&>(node);
                    ss << "\"" << d.name << "\": [[";
                    for (u32 i = 0; i < cn.curve.point_count; i++) {
                        if (i > 0) ss << "],[";
                        ss << cn.curve.points[i].time << "," << cn.curve.points[i].value;
                    }
                    ss << "]]";
                }
                break;
            }
            default: break;
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

    static void ParseNodesArray(const std::string& arr,
                                 std::vector<std::unique_ptr<MaterialNode>>& out_nodes) {
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

    static void ApplyParams(MaterialNode* node, const std::string& params) {
        size_t pos = 0;
        while (pos < params.size()) {
            while (pos < params.size() && (params[pos] == ' ' || params[pos] == '\n' || params[pos] == '\r' || params[pos] == '\t'))
                pos++;
            if (pos >= params.size()) break;

            if (params[pos] != '"') { pos++; continue; }
            auto q1 = pos;
            auto q2 = params.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string key = params.substr(q1 + 1, q2 - q1 - 1);

            auto colon = params.find(':', q2);
            if (colon == std::string::npos) break;

            size_t val_start = colon + 1;
            while (val_start < params.size() && params[val_start] == ' ')
                val_start++;
            if (val_start >= params.size()) break;

            if (params[val_start] == '[') {
                auto arr_end = FindMatchingBracket(params, val_start);
                if (arr_end == std::string::npos) break;
                std::string arr_str = params.substr(val_start + 1, arr_end - val_start - 1);

                // Check for nested array (curve points): [[t0,v0],[t1,v1],...]
                if (key == "curve_points" && std::strcmp(node->TypeName(), "Curve") == 0) {
                    auto* cn = static_cast<CurveNode*>(node);
                    cn->curve.point_count = 0;
                    size_t cp = 0;
                    while (cp < arr_str.size() && cn->curve.point_count < CurveData::MAX_POINTS) {
                        auto inner_start = arr_str.find('[', cp);
                        if (inner_start == std::string::npos) break;
                        auto inner_end = FindMatchingBracket(arr_str, inner_start);
                        if (inner_end == std::string::npos) break;
                        std::string inner = arr_str.substr(inner_start + 1, inner_end - inner_start - 1);
                        auto vals = ParseFloatArray(inner);
                        if (vals.size() >= 2) {
                            cn->curve.points[cn->curve.point_count].time = vals[0];
                            cn->curve.points[cn->curve.point_count].value = vals[1];
                            cn->curve.point_count++;
                        }
                        cp = inner_end + 1;
                    }
                } else {
                    auto arr_vals = ParseFloatArray(arr_str);
                    if (arr_vals.size() == 4) {
                        node->SetParamByName(key.c_str(), math::v4{arr_vals[0], arr_vals[1], arr_vals[2], arr_vals[3]});
                    } else if (arr_vals.size() == 3) {
                        node->SetParamByName(key.c_str(), math::v3{arr_vals[0], arr_vals[1], arr_vals[2]});
                    } else if (arr_vals.size() == 1) {
                        node->SetParamByName(key.c_str(), arr_vals[0]);
                    }
                }

                pos = arr_end + 1;
                auto comma = params.find(',', pos);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            } else if (params[val_start] == '"') {
                auto q_end = params.find('"', val_start + 1);
                if (q_end == std::string::npos) break;
                std::string str_val = params.substr(val_start + 1, q_end - val_start - 1);
                node->SetParamByName(key.c_str(), str_val.c_str());
                pos = q_end + 1;
                auto comma = params.find(',', pos);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            } else {
                std::string val_str;
                for (size_t i = val_start; i < params.size(); ++i) {
                    char c = params[i];
                    if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == 'E' || c == '+')
                        val_str += c;
                    else if (!val_str.empty()) break;
                }

                if (!val_str.empty()) {
                    f32 value = std::stof(val_str);
                    node->SetParamByName(key.c_str(), value);
                }

                auto comma = params.find(',', val_start);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
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

} // namespace primal::graphics::material_graph
