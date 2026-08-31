#pragma once

#include "Graphics/MaterialGraph/MaterialNode.h"

namespace primal::graphics::material_graph {

class UVNode : public MaterialNode {
public:
    math::v2 default_uv{0.0f, 0.0f};

    UVNode() {
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float2;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "UV"; }
    NodeTypeInfo GetTypeInfo() const override {
        return {"UV", "UV", "Input", false};
    }
    void Execute() override {
        auto* out = CreateOutput<MaterialFloat2Data>(0);
        out->value = default_uv;
    }
    const MaterialPinDescriptor* GetPinDescriptors(u32& c) const override { c = 1; return kPins; }
private:
    static const MaterialPinDescriptor kPins[];
};

inline const MaterialPinDescriptor UVNode::kPins[] = {
    {"uv", 0, MaterialDataType::Float2, false},
};

} // namespace primal::graphics::material_graph
