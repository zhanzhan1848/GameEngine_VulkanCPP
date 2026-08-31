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
#include <algorithm>
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
    // slot i → the asset_path of the ConstantTexture assigned texture
    // binding i. Lets binders import descriptors in exactly the order the
    // emitted texN arguments expect, instead of guessing node visitation.
    utl::vector<std::string> texture_slots;
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
        texture_slots_ = &result.texture_slots;
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

        // Only nodes that contribute to a MaterialOutput participate in the
        // shader. Editors deliberately allow unfinished/disconnected nodes on
        // the canvas; validating those as if they were live made an unrelated
        // empty Add/Curve node disable the whole material preview.
        utl::vector<u8> active(n, 0);
        bool has_material_output = false;
        for (u32 i = 0; i < n; ++i) {
            if (std::strcmp(graph.GetNodes()[i]->TypeName(), "MaterialOutput") == 0) {
                active[i] = 1;
                has_material_output = true;
            }
        }
        if (!has_material_output) {
            // Standalone translator/compiler tests intentionally build graph
            // fragments without a terminal node; retain their prior behavior.
            std::fill(active.begin(), active.end(), static_cast<u8>(1));
        } else {
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto& conn : graph.GetConnections()) {
                    if (active[conn.to_node] && !active[conn.from_node]) {
                        active[conn.from_node] = 1;
                        changed = true;
                    }
                }
            }
        }

        for (u32 node_id : order) {
            const auto& node = graph.GetNodes()[node_id];
            // Preserve IR generation for disconnected leaf/value nodes (UV,
            // Time, constants), which is useful to graph tooling, while
            // ignoring unfinished disconnected operators with required inputs.
            if (!active[node_id] && !node->inputs.empty()) continue;
            TranslateNode(node_id, node.get(), builder);
        }

        result.success = result.errors.empty();
        errors_ = nullptr;
        texture_slots_ = nullptr;
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
    utl::vector<std::string>* texture_slots_{nullptr};
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
            // The world graph-material descriptor layout carries 8 image
            // bindings (ForwardSceneRenderer graph_material_set_layout_); the
            // preview layout is 4 but the translator only needs one cap —
            // graphs beyond it fail translation and the caller falls back to
            // the fixed material path.
            constexpr u16 kMaxTextureSlots = 8;
            if (next_texture_slot_ >= kMaxTextureSlots) {
                Error(node_id, "material uses more than 8 textures");
                return;
            }
            u16 slot = next_texture_slot_++;
            reg = builder.TextureHandle(slot);
            node_output_type_[node_id] = DT(material_graph::MaterialDataType::Texture2D);
            if (texture_slots_) {
                if ((size_t)slot >= texture_slots_->size()) texture_slots_->resize((size_t)slot + 1);
                (*texture_slots_)[slot] = static_cast<material_graph::ConstantTextureNode*>(node)->asset_path;
            }

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
            const u8 f = DT(material_graph::MaterialDataType::Float);
            const u8 f2 = DT(material_graph::MaterialDataType::Float2);
            const u8 f3 = DT(material_graph::MaterialDataType::Float3);
            const u8 f4 = DT(material_graph::MaterialDataType::Float4);
            const bool same_vector_width = dt_a == dt_b && dt_a >= f2 && dt_a <= f4;
            const bool scalar_broadcast = (dt_a == f && dt_b >= f && dt_b <= f4) ||
                                          (dt_b == f && dt_a >= f && dt_a <= f4);
            const bool float3_float4 = (dt_a == f3 && dt_b == f4) ||
                                       (dt_a == f4 && dt_b == f3);
            if (!same_vector_width && !scalar_broadcast && !float3_float4) {
                Error(node_id, "Multiply inputs have incompatible types");
                return;
            }
            // A sampled texture is Float4. Multiplying it by a Float3 tint
            // promotes the tint to Float4(..., 1), preserving texture alpha.
            if (dt_a == f3 && dt_b == f4) a = builder.Float3ToFloat4(a);
            if (dt_a == f4 && dt_b == f3) b = builder.Float3ToFloat4(b);
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
                if (src == 0xFFFF) continue;
                // Pin 0 (BaseColor) is a Float4 output. If the upstream is
                // a Float3 (e.g. ConstantFloat3, a 3-component math
                // expression), pad alpha to 1.0 so the emitted shader
                // matches the Float4 return type. Without this Metal
                // fails to type-check (return type mismatch) and the
                // preview silently falls back to the background
                // gradient. Float4 sources and unconnected pins pass
                // through unchanged.
                if (pin == 0 && GetInputType(node_id, pin) ==
                    DT(material_graph::MaterialDataType::Float3)) {
                    src = builder.Float3ToFloat4(src);
                } else if (pin == 0 && GetInputType(node_id, pin) ==
                    DT(material_graph::MaterialDataType::Float2)) {
                    src = builder.Float2ToFloat4(src);
                }
                builder.StoreOutput(src, (u8)pin);
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
