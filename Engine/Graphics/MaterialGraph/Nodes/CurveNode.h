#pragma once

#include "Graphics/MaterialGraph/MaterialNode.h"
#include "Graphics/MaterialGraph/CurveData.h"
#include <cstring>

namespace primal::graphics::material_graph {

class CurveNode : public MaterialNode {
public:
    CurveData curve;

    CurveNode() {
        inputs.resize(1);
        inputs[0].expected_type = MaterialDataType::Float;
        inputs[0].index = 0;

        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "Curve"; }
    NodeTypeInfo GetTypeInfo() const override {
        return {"Curve", "Curve", "Utility", false};
    }
    void Execute() override {
        f32 x = 0.0f;
        if (inputs[0].data) {
            auto* inp = inputs[0].AsFloat();
            if (inp) x = inp->value;
        }
        auto* out = CreateOutput<MaterialFloatData>(0);
        out->value = curve.Evaluate(x);
    }

    const MaterialParamDescriptor* GetParamDescriptors(u32& c) const override { c = 1; return kParams; }
    const MaterialPinDescriptor* GetPinDescriptors(u32& c) const override { c = 1; return kPins; }

private:
    static const MaterialParamDescriptor kParams[];
    static const MaterialPinDescriptor kPins[];
};

inline const MaterialParamDescriptor CurveNode::kParams[] = {
    {"curve_points", "Curve", MaterialParamType::Curve, {},
     MAT_OFFSETOF(CurveNode, curve), sizeof(CurveData), nullptr},
};
inline const MaterialPinDescriptor CurveNode::kPins[] = {
    {"x", 0, MaterialDataType::Float, true},
};

} // namespace primal::graphics::material_graph
