#pragma once

#include "CommonHeaders.h"
#include "Graphics/ShaderIR/ShaderIRBuilder.h"
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
#include <cstring>
#include <string>
#include <unordered_map>

namespace primal::graphics::shader_ir {

struct TranslateError {
    std::string message;
    u32 node_id;
};

struct TranslateResult {
    ShaderIRFunction function;
    bool success{false};
    utl::vector<TranslateError> errors;
};

class MaterialGraphToIR {
public:
    TranslateResult Translate(const material_graph::MaterialGraph& graph) {
        TranslateResult result;
        ShaderIRBuilder builder(result.function);

        input_source_.clear();
        node_output_reg_.clear();
        node_output_type_.clear();
        errors_ = &result.errors;
        next_texture_slot_ = 0;

        // Build connection map: (to_node, to_pin) → (from_node, from_pin)
        for (auto& conn : graph.GetConnections()) {
            InputKey key{conn.to_node, conn.to_pin};
            input_source_[key] = {conn.from_node, conn.from_pin};
        }

        auto order = TopologicalSort(graph);

        u32 n = (u32)graph.GetNodes().size();
        node_output_reg_.resize(n, 0xFFFF);
        node_output_type_.resize(n, 0);

        for (u32 node_id : order) {
            const auto& node = graph.GetNodes()[node_id];
            TranslateNode(node_id, node.get(), builder);
        }

        result.success = result.errors.empty();
        errors_ = nullptr;
        return result;
    }

private:
    struct InputKey {
        u32 node_id;
        u32 pin_index;
        bool operator==(const InputKey& o) const { return node_id == o.node_id && pin_index == o.pin_index; }
    };

    struct InputKeyHash {
        u64 operator()(const InputKey& k) const {
            return ((u64)k.node_id * 1000003ULL) ^ k.pin_index;
        }
    };

    std::unordered_map<InputKey, InputKey, InputKeyHash> input_source_;
    utl::vector<u16> node_output_reg_;
    utl::vector<u8>  node_output_type_;
    utl::vector<TranslateError>* errors_{nullptr};
    u16 next_texture_slot_{0};

    void Error(u32 node_id, const char* msg) {
        if (errors_) errors_->push_back({msg, node_id});
    }

    static utl::vector<u32> TopologicalSort(const material_graph::MaterialGraph& graph) {
        const auto& nodes = graph.GetNodes();
        const auto& conns = graph.GetConnections();
        u32 n = (u32)nodes.size();

        utl::vector<u32> in_degree(n, 0);
        for (auto& c : conns) in_degree[c.to_node]++;

        utl::vector<u32> order;
        order.reserve(n);
        for (u32 i = 0; i < n; i++) {
            if (in_degree[i] == 0) order.push_back(i);
        }

        for (u32 idx = 0; idx < order.size(); idx++) {
            u32 nid = order[idx];
            for (auto& c : conns) {
                if (c.from_node == nid) {
                    if (--in_degree[c.to_node] == 0) {
                        order.push_back(c.to_node);
                    }
                }
            }
        }
        return order;
    }

    // Returns the source register for an input pin, or 0xFFFF if unconnected.
    // If required=true and unconnected, emits an error.
    u16 GetInputReg(u32 node_id, u32 pin_index, bool required = false, const char* pin_name = "") {
        InputKey key{node_id, pin_index};
        auto it = input_source_.find(key);
        if (it == input_source_.end()) {
            if (required) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Required input '%s' is not connected", pin_name);
                Error(node_id, buf);
            }
            return 0xFFFF;
        }
        u16 reg = node_output_reg_[it->second.node_id];
        if (reg == 0xFFFF && required) {
            char buf[128];
            snprintf(buf, sizeof(buf), "Upstream node for input '%s' produced no output", pin_name);
            Error(node_id, buf);
        }
        return reg;
    }

    u8 GetInputType(u32 node_id, u32 pin_index) {
        InputKey key{node_id, pin_index};
        auto it = input_source_.find(key);
        if (it == input_source_.end()) return 0;
        return node_output_type_[it->second.node_id];
    }

    static u8 DT(material_graph::MaterialDataType t) { return static_cast<u8>(t); }

    void TranslateNode(u32 node_id, material_graph::MaterialNode* node, ShaderIRBuilder& builder) {
        const char* type = node->TypeName();
        u16 reg = 0xFFFF;

        if (std::strcmp(type, "ConstantFloat") == 0) {
            auto* cn = static_cast<material_graph::ConstantFloatNode*>(node);
            reg = builder.ConstFloat(cn->value);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float);

        } else if (std::strcmp(type, "ConstantFloat3") == 0) {
            auto* cn = static_cast<material_graph::ConstantFloat3Node*>(node);
            reg = builder.ConstFloat3(cn->value.x, cn->value.y, cn->value.z);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float3);

        } else if (std::strcmp(type, "ConstantFloat4") == 0) {
            auto* cn = static_cast<material_graph::ConstantFloat4Node*>(node);
            reg = builder.ConstFloat4(cn->value.x, cn->value.y, cn->value.z, cn->value.w);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float4);

        } else if (std::strcmp(type, "ConstantTexture") == 0) {
            u16 slot = next_texture_slot_++;
            reg = builder.TextureHandle(slot);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Texture2D);

        } else if (std::strcmp(type, "Time") == 0) {
            reg = builder.LoadTime();
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float);

        } else if (std::strcmp(type, "UV") == 0) {
            reg = builder.LoadUV();
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float2);

        } else if (std::strcmp(type, "Add") == 0) {
            u16 a = GetInputReg(node_id, 0, true, "A");
            u16 b = GetInputReg(node_id, 1, true, "B");
            if (a == 0xFFFF || b == 0xFFFF) return;
            u8 dt = GetInputType(node_id, 0);
            reg = builder.Add(dt, a, b);
            node_output_type_[node_id] = dt;

        } else if (std::strcmp(type, "Multiply") == 0) {
            u16 a = GetInputReg(node_id, 0, true, "A");
            u16 b = GetInputReg(node_id, 1, true, "B");
            if (a == 0xFFFF || b == 0xFFFF) return;
            u8 dt_a = GetInputType(node_id, 0);
            u8 dt_b = GetInputType(node_id, 1);
            u8 dt = (dt_a > dt_b) ? dt_a : dt_b;
            reg = builder.Mul(dt, a, b);
            node_output_type_[node_id] = dt;

        } else if (std::strcmp(type, "Lerp") == 0) {
            u16 a = GetInputReg(node_id, 0, true, "A");
            u16 b = GetInputReg(node_id, 1, true, "B");
            u16 alpha = GetInputReg(node_id, 2, true, "Alpha");
            if (a == 0xFFFF || b == 0xFFFF || alpha == 0xFFFF) return;
            u8 dt = GetInputType(node_id, 0);
            reg = builder.Lerp(dt, a, b, alpha);
            node_output_type_[node_id] = dt;

        } else if (std::strcmp(type, "Clamp") == 0) {
            auto* cn = static_cast<material_graph::ClampNode*>(node);
            u16 val = GetInputReg(node_id, 0, true, "Value");
            if (val == 0xFFFF) return;
            u16 min_reg = builder.ConstFloat(cn->min_val);
            u16 max_reg = builder.ConstFloat(cn->max_val);
            reg = builder.Clamp(DT(material_graph::MaterialDataType::Float), val, min_reg, max_reg);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float);

        } else if (std::strcmp(type, "Pow") == 0) {
            u16 base = GetInputReg(node_id, 0, true, "Base");
            u16 exp = GetInputReg(node_id, 1, true, "Exponent");
            if (base == 0xFFFF || exp == 0xFFFF) return;
            reg = builder.Pow(base, exp);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float);

        } else if (std::strcmp(type, "Saturate") == 0) {
            u16 val = GetInputReg(node_id, 0, true, "Value");
            if (val == 0xFFFF) return;
            reg = builder.Saturate(val);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float);

        } else if (std::strcmp(type, "Fresnel") == 0) {
            auto* fn = static_cast<material_graph::FresnelNode*>(node);
            u16 n = GetInputReg(node_id, 0, true, "N");
            u16 v = GetInputReg(node_id, 1, true, "V");
            if (n == 0xFFFF || v == 0xFFFF) return;
            reg = builder.Fresnel(n, v, fn->power);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float);

        } else if (std::strcmp(type, "NormalBlend") == 0) {
            u16 base = GetInputReg(node_id, 0, true, "Base");
            u16 detail = GetInputReg(node_id, 1, true, "Detail");
            if (base == 0xFFFF || detail == 0xFFFF) return;
            reg = builder.NormalBlend(base, detail);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float3);

        } else if (std::strcmp(type, "ConstantFloat2") == 0) {
            auto* cn = static_cast<material_graph::ConstantFloat2Node*>(node);
            reg = builder.ConstFloat2(cn->value.x, cn->value.y);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float2);

        } else if (std::strcmp(type, "Select") == 0) {
            u16 cond = GetInputReg(node_id, 0, true, "Condition");
            u16 true_val = GetInputReg(node_id, 1, true, "True");
            u16 false_val = GetInputReg(node_id, 2, true, "False");
            if (cond == 0xFFFF || true_val == 0xFFFF || false_val == 0xFFFF) return;
            u8 dt = GetInputType(node_id, 1);
            reg = builder.Select(dt, cond, true_val, false_val);
            node_output_type_[node_id] = dt;

        } else if (std::strcmp(type, "Remap") == 0) {
            auto* rn = static_cast<material_graph::RemapNode*>(node);
            u16 val = GetInputReg(node_id, 0, true, "Value");
            if (val == 0xFFFF) return;
            reg = builder.Remap(val, rn->in_min, rn->in_max, rn->out_min, rn->out_max);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float);

        } else if (std::strcmp(type, "SampleTexture") == 0) {
            u16 tex = GetInputReg(node_id, 0, true, "Texture");
            u16 uv = GetInputReg(node_id, 1, true, "UV");
            if (tex == 0xFFFF || uv == 0xFFFF) return;
            reg = builder.Sample(tex, uv);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float4);

        } else if (std::strcmp(type, "Curve") == 0) {
            auto* cn = static_cast<material_graph::CurveNode*>(node);
            u16 x_reg = GetInputReg(node_id, 0, false, "X");
            if (x_reg == 0xFFFF) {
                // Default to time if X input is unconnected
                x_reg = builder.LoadTime();
            }
            if (cn->curve.point_count < 2) {
                Error(node_id, "Curve needs at least 2 control points");
                return;
            }
            // Flatten control points: [t0, v0, t1, v1, ...]
            utl::vector<f32> flat;
            for (u32 i = 0; i < cn->curve.point_count; i++) {
                flat.push_back(cn->curve.points[i].time);
                flat.push_back(cn->curve.points[i].value);
            }
            reg = builder.CurveEval(x_reg, flat.data(), cn->curve.point_count);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Float);

        } else if (std::strcmp(type, "MaterialOutput") == 0) {
            for (u32 pin = 0; pin < node->inputs.size(); pin++) {
                u16 src = GetInputReg(node_id, pin);
                if (src != 0xFFFF) {
                    builder.StoreOutput(src, (u8)pin);
                }
            }
            return;
        } else {
            Error(node_id, "Unknown node type");
            return;
        }

        node_output_reg_[node_id] = reg;
    }
};

} // namespace primal::graphics::shader_ir
