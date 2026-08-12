#pragma once

#include "Graphics/MaterialGraph/MaterialNode.h"
#include "Graphics/Material/ShaderTechnique.h"
#include <cstring>

namespace primal::graphics::material_graph {

class MaterialOutputNode : public MaterialNode {
public:
    MaterialOutputNode() {
        inputs.resize(8);
        // Pin 0: BaseColor (Float4)
        inputs[0].expected_type = MaterialDataType::Float4; inputs[0].index = 0;
        // Pin 1: Roughness (Float)
        inputs[1].expected_type = MaterialDataType::Float; inputs[1].index = 1;
        // Pin 2: Metallic (Float)
        inputs[2].expected_type = MaterialDataType::Float; inputs[2].index = 2;
        // Pin 3: AlphaCutoff (Float)
        inputs[3].expected_type = MaterialDataType::Float; inputs[3].index = 3;
        // Pin 4: AlbedoTex (Texture2D)
        inputs[4].expected_type = MaterialDataType::Texture2D; inputs[4].index = 4;
        // Pin 5: NormalTex (Texture2D)
        inputs[5].expected_type = MaterialDataType::Texture2D; inputs[5].index = 5;
        // Pin 6: ORMTex (Texture2D)
        inputs[6].expected_type = MaterialDataType::Texture2D; inputs[6].index = 6;
        // Pin 7: Technique (Float, cast to u32)
        inputs[7].expected_type = MaterialDataType::Float; inputs[7].index = 7;
        // No outputs — this is a terminal node
    }
    const char* TypeName() const override { return "MaterialOutput"; }
    NodeTypeInfo GetTypeInfo() const override {
        return {"MaterialOutput", "Material Output", "Output", true};
    }
    void Execute() override {
        // No-op: data is propagated to input pins by the graph before Execute().
        // The bridge reads input pins directly after graph execution.
    }

    const MaterialPinDescriptor* GetPinDescriptors(u32& c) const override { c = 8; return kPins; }

private:
    static const MaterialPinDescriptor kPins[];
};

inline const MaterialPinDescriptor MaterialOutputNode::kPins[] = {
    {"BaseColor",   0, MaterialDataType::Float4,    true},
    {"Roughness",   1, MaterialDataType::Float,     true},
    {"Metallic",    2, MaterialDataType::Float,     true},
    {"AlphaCutoff", 3, MaterialDataType::Float,     true},
    {"AlbedoTex",   4, MaterialDataType::Texture2D, true},
    {"NormalTex",   5, MaterialDataType::Texture2D, true},
    {"ORMTex",      6, MaterialDataType::Texture2D, true},
    {"Technique",   7, MaterialDataType::Float,     true},
};

} // namespace primal::graphics::material_graph
