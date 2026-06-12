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
    void Execute() override {
        auto* out = CreateOutput<MaterialFloat2Data>(0);
        out->value = default_uv;
    }
};

} // namespace primal::graphics::material_graph
