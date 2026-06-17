#pragma once

#include "Graphics/MaterialGraph/MaterialNode.h"
#include <cstring>

namespace primal::graphics::material_graph {

class SelectNode : public MaterialNode {
public:
    SelectNode() {
        inputs.resize(3);
        inputs[0].expected_type = MaterialDataType::Bool;
        inputs[0].index = 0;
        inputs[1].expected_type = MaterialDataType::Float;
        inputs[1].index = 1;
        inputs[2].expected_type = MaterialDataType::Float;
        inputs[2].index = 2;

        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "Select"; }
    NodeTypeInfo GetTypeInfo() const override {
        return {"Select", "Select", "Math", false};
    }
    void Execute() override {
        auto* cond = inputs[0].AsBool();
        if (!cond) return;
        MaterialPin& src_pin = cond->value ? inputs[1] : inputs[2];
        if (!src_pin.data) return;

        switch (src_pin.data->type) {
        case MaterialDataType::Float: {
            auto* src = src_pin.AsFloat();
            if (src) { auto* out = CreateOutput<MaterialFloatData>(0); out->value = src->value; }
            break;
        }
        case MaterialDataType::Float3: {
            auto* src = src_pin.AsFloat3();
            if (src) { auto* out = CreateOutput<MaterialFloat3Data>(0); out->value = src->value; }
            break;
        }
        case MaterialDataType::Float4: {
            auto* src = src_pin.AsFloat4();
            if (src) { auto* out = CreateOutput<MaterialFloat4Data>(0); out->value = src->value; }
            break;
        }
        default: break;
        }
    }

    const MaterialPinDescriptor* GetPinDescriptors(u32& c) const override { c = 3; return kPins; }

private:
    static const MaterialPinDescriptor kPins[];
};

inline const MaterialPinDescriptor SelectNode::kPins[] = {
    {"condition",   0, MaterialDataType::Bool,  true},
    {"true_value",  1, MaterialDataType::Float, true},
    {"false_value", 2, MaterialDataType::Float, true},
};

} // namespace primal::graphics::material_graph
