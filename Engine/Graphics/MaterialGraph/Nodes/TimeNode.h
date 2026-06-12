#pragma once

#include "Graphics/MaterialGraph/MaterialNode.h"
#include <chrono>

namespace primal::graphics::material_graph {

class TimeNode : public MaterialNode {
public:
    TimeNode() {
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "Time"; }
    void Execute() override {
        static auto start = std::chrono::steady_clock::now();
        f32 t = std::chrono::duration<f32>(std::chrono::steady_clock::now() - start).count();
        auto* out = CreateOutput<MaterialFloatData>(0);
        out->value = t;
    }
};

} // namespace primal::graphics::material_graph
