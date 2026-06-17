#pragma once

#include "Graphics/MaterialGraph/MaterialNode.h"
#include <cstring>

namespace primal::graphics::material_graph {

class SampleTextureNode : public MaterialNode {
public:
    SampleTextureNode() {
        inputs.resize(2);
        inputs[0].expected_type = MaterialDataType::Texture2D;
        inputs[0].index = 0;
        inputs[1].expected_type = MaterialDataType::Float2;
        inputs[1].index = 1;

        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float4;
        outputs[0].index = 0;
    }
    const char* TypeName() const override { return "SampleTexture"; }
    NodeTypeInfo GetTypeInfo() const override {
        return {"SampleTexture", "Sample Texture", "Texture", false};
    }
    void Execute() override {
        auto* out = CreateOutput<MaterialFloat4Data>(0);
        out->value = math::v4{1.0f, 1.0f, 1.0f, 1.0f};
    }

    const MaterialPinDescriptor* GetPinDescriptors(u32& c) const override { c = 2; return kPins; }

private:
    static const MaterialPinDescriptor kPins[];
};

inline const MaterialPinDescriptor SampleTextureNode::kPins[] = {
    {"texture", 0, MaterialDataType::Texture2D, true},
    {"uv", 1, MaterialDataType::Float2, true},
};

} // namespace primal::graphics::material_graph
