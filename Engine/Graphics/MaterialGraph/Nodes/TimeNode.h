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
    NodeTypeInfo GetTypeInfo() const override {
        return {"Time", "Time", "Input", false};
    }
    void Execute() override {
        static auto start = std::chrono::steady_clock::now();
        f32 t = std::chrono::duration<f32>(std::chrono::steady_clock::now() - start).count();
        auto* out = CreateOutput<MaterialFloatData>(0);
        out->value = t;
    }
    const MaterialPinDescriptor* GetPinDescriptors(u32& c) const override { c = 1; return kPins; }
private:
    static const MaterialPinDescriptor kPins[];
};

inline const MaterialPinDescriptor TimeNode::kPins[] = {
    {"time", 0, MaterialDataType::Float, false},
};

} // namespace primal::graphics::material_graph
