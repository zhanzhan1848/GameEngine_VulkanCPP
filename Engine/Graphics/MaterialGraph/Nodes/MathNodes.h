#pragma once

#include "Graphics/MaterialGraph/MaterialNode.h"
#include <algorithm>
#include <cmath>

namespace primal::graphics::material_graph {

// Add: A + B (supports Float, Float3, Float4)
class AddNode : public MaterialNode {
public:
    AddNode() {
        inputs.resize(2);
        inputs[0].expected_type = MaterialDataType::Float;
        inputs[0].index = 0;
        inputs[1].expected_type = MaterialDataType::Float;
        inputs[1].index = 1;
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "Add"; }
    void Execute() override {
        if (!inputs[0].data || !inputs[1].data) return;
        auto ta = inputs[0].data->type;
        if (ta == MaterialDataType::Float3) {
            auto* a = inputs[0].AsFloat3();
            auto* b = inputs[1].AsFloat3();
            if (a && b) {
                auto* out = CreateOutput<MaterialFloat3Data>(0);
                out->value = {a->value.x + b->value.x, a->value.y + b->value.y, a->value.z + b->value.z};
            }
        } else if (ta == MaterialDataType::Float4) {
            auto* a = inputs[0].AsFloat4();
            auto* b = inputs[1].AsFloat4();
            if (a && b) {
                auto* out = CreateOutput<MaterialFloat4Data>(0);
                out->value = {a->value.x + b->value.x, a->value.y + b->value.y, a->value.z + b->value.z, a->value.w + b->value.w};
            }
        } else {
            auto* a = inputs[0].AsFloat();
            auto* b = inputs[1].AsFloat();
            if (a && b) {
                auto* out = CreateOutput<MaterialFloatData>(0);
                out->value = a->value + b->value;
            }
        }
    }
};

// Multiply: A * B (supports Float * Float, Float * Float3, Float * Float4)
class MultiplyNode : public MaterialNode {
public:
    MultiplyNode() {
        inputs.resize(2);
        inputs[0].expected_type = MaterialDataType::Float;
        inputs[0].index = 0;
        inputs[1].expected_type = MaterialDataType::Float;
        inputs[1].index = 1;
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "Multiply"; }
    void Execute() override {
        if (!inputs[0].data || !inputs[1].data) return;
        // Float * Float3/Float4: scalar * vector
        if (inputs[0].data->type == MaterialDataType::Float && inputs[1].data->type == MaterialDataType::Float3) {
            auto* s = inputs[0].AsFloat();
            auto* v = inputs[1].AsFloat3();
            if (s && v) {
                auto* out = CreateOutput<MaterialFloat3Data>(0);
                out->value = {s->value * v->value.x, s->value * v->value.y, s->value * v->value.z};
            }
        } else if (inputs[0].data->type == MaterialDataType::Float3 && inputs[1].data->type == MaterialDataType::Float) {
            auto* v = inputs[0].AsFloat3();
            auto* s = inputs[1].AsFloat();
            if (v && s) {
                auto* out = CreateOutput<MaterialFloat3Data>(0);
                out->value = {s->value * v->value.x, s->value * v->value.y, s->value * v->value.z};
            }
        } else if (inputs[0].data->type == MaterialDataType::Float && inputs[1].data->type == MaterialDataType::Float4) {
            auto* s = inputs[0].AsFloat();
            auto* v = inputs[1].AsFloat4();
            if (s && v) {
                auto* out = CreateOutput<MaterialFloat4Data>(0);
                out->value = {s->value * v->value.x, s->value * v->value.y, s->value * v->value.z, s->value * v->value.w};
            }
        } else if (inputs[0].data->type == MaterialDataType::Float4 && inputs[1].data->type == MaterialDataType::Float) {
            auto* v = inputs[0].AsFloat4();
            auto* s = inputs[1].AsFloat();
            if (v && s) {
                auto* out = CreateOutput<MaterialFloat4Data>(0);
                out->value = {s->value * v->value.x, s->value * v->value.y, s->value * v->value.z, s->value * v->value.w};
            }
        } else {
            auto* a = inputs[0].AsFloat();
            auto* b = inputs[1].AsFloat();
            if (a && b) {
                auto* out = CreateOutput<MaterialFloatData>(0);
                out->value = a->value * b->value;
            }
        }
    }
};

// Lerp: A + (B - A) * Alpha
class LerpNode : public MaterialNode {
public:
    LerpNode() {
        inputs.resize(3);
        inputs[0].expected_type = MaterialDataType::Float; inputs[0].index = 0;
        inputs[1].expected_type = MaterialDataType::Float; inputs[1].index = 1;
        inputs[2].expected_type = MaterialDataType::Float; inputs[2].index = 2;
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float; outputs[0].index = 0;
    }
    const char* TypeName() const override { return "Lerp"; }
    void Execute() override {
        if (!inputs[0].data || !inputs[1].data || !inputs[2].data) return;
        auto* alpha = inputs[2].AsFloat();
        if (!alpha) return;
        f32 t = alpha->value;

        if (inputs[0].data->type == MaterialDataType::Float3) {
            auto* a = inputs[0].AsFloat3();
            auto* b = inputs[1].AsFloat3();
            if (a && b) {
                auto* out = CreateOutput<MaterialFloat3Data>(0);
                out->value = {
                    a->value.x + (b->value.x - a->value.x) * t,
                    a->value.y + (b->value.y - a->value.y) * t,
                    a->value.z + (b->value.z - a->value.z) * t
                };
            }
        } else if (inputs[0].data->type == MaterialDataType::Float4) {
            auto* a = inputs[0].AsFloat4();
            auto* b = inputs[1].AsFloat4();
            if (a && b) {
                auto* out = CreateOutput<MaterialFloat4Data>(0);
                out->value = {
                    a->value.x + (b->value.x - a->value.x) * t,
                    a->value.y + (b->value.y - a->value.y) * t,
                    a->value.z + (b->value.z - a->value.z) * t,
                    a->value.w + (b->value.w - a->value.w) * t
                };
            }
        } else {
            auto* a = inputs[0].AsFloat();
            auto* b = inputs[1].AsFloat();
            if (a && b) {
                auto* out = CreateOutput<MaterialFloatData>(0);
                out->value = a->value + (b->value - a->value) * t;
            }
        }
    }
};

// Clamp: clamp(Value, Min, Max)
class ClampNode : public MaterialNode {
public:
    f32 min_val{0.0f};
    f32 max_val{1.0f};

    ClampNode() {
        inputs.resize(1);
        inputs[0].expected_type = MaterialDataType::Float; inputs[0].index = 0;
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float; outputs[0].index = 0;
    }
    const char* TypeName() const override { return "Clamp"; }
    void Execute() override {
        if (!inputs[0].data) return;
        auto* v = inputs[0].AsFloat();
        if (v) {
            auto* out = CreateOutput<MaterialFloatData>(0);
            out->value = std::clamp(v->value, min_val, max_val);
        }
    }
    const MaterialParamDescriptor* GetParamDescriptors(u32& c) const override { c = 2; return kParams; }
    bool SetParamByName(const char* n, f32 v) override {
        if (std::strcmp(n, "min_val") == 0) { min_val = v; return true; }
        if (std::strcmp(n, "max_val") == 0) { max_val = v; return true; }
        return false;
    }
private:
    static const MaterialParamDescriptor kParams[];
};

inline const MaterialParamDescriptor ClampNode::kParams[] = {
    {"min_val", "Clamp", MaterialParamType::Float, {-1e6f, 1e6f, 0.01f},
     MAT_OFFSETOF(ClampNode, min_val), sizeof(min_val), nullptr},
    {"max_val", "Clamp", MaterialParamType::Float, {-1e6f, 1e6f, 0.01f},
     MAT_OFFSETOF(ClampNode, max_val), sizeof(max_val), nullptr},
};

// Pow: pow(Base, Exponent)
class PowNode : public MaterialNode {
public:
    PowNode() {
        inputs.resize(2);
        inputs[0].expected_type = MaterialDataType::Float; inputs[0].index = 0;
        inputs[1].expected_type = MaterialDataType::Float; inputs[1].index = 1;
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float; outputs[0].index = 0;
    }
    const char* TypeName() const override { return "Pow"; }
    void Execute() override {
        auto* base = inputs[0].AsFloat();
        auto* exp = inputs[1].AsFloat();
        if (base && exp) {
            auto* out = CreateOutput<MaterialFloatData>(0);
            out->value = std::pow(base->value, exp->value);
        }
    }
};

// Saturate: clamp(Value, 0, 1)
class SaturateNode : public MaterialNode {
public:
    SaturateNode() {
        inputs.resize(1);
        inputs[0].expected_type = MaterialDataType::Float; inputs[0].index = 0;
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float; outputs[0].index = 0;
    }
    const char* TypeName() const override { return "Saturate"; }
    void Execute() override {
        auto* v = inputs[0].AsFloat();
        if (v) {
            auto* out = CreateOutput<MaterialFloatData>(0);
            out->value = std::clamp(v->value, 0.0f, 1.0f);
        }
    }
};

} // namespace primal::graphics::material_graph
