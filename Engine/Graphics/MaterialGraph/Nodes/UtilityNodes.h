#pragma once

#include "Graphics/MaterialGraph/MaterialNode.h"
#include <cmath>
#include <cstring>

namespace primal::graphics::material_graph {

// Fresnel: pow(1 - max(dot(N, V), 0), power)
class FresnelNode : public MaterialNode {
public:
    f32 power{5.0f};

    FresnelNode() {
        inputs.resize(2);
        inputs[0].expected_type = MaterialDataType::Float3; inputs[0].index = 0;
        inputs[1].expected_type = MaterialDataType::Float3; inputs[1].index = 1;
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float; outputs[0].index = 0;
    }
    const char* TypeName() const override { return "Fresnel"; }
    void Execute() override {
        auto* n = inputs[0].AsFloat3();
        auto* v = inputs[1].AsFloat3();
        if (n && v) {
            f32 dot = n->value.x * v->value.x + n->value.y * v->value.y + n->value.z * v->value.z;
            if (dot < 0.0f) dot = 0.0f;
            if (dot > 1.0f) dot = 1.0f;
            auto* out = CreateOutput<MaterialFloatData>(0);
            out->value = std::pow(1.0f - dot, power);
        }
    }

    const MaterialParamDescriptor* GetParamDescriptors(u32& c) const override { c = 1; return kParams; }
    bool SetParamByName(const char* n, f32 v) override {
        if (std::strcmp(n, "power") == 0) { power = v; return true; }
        return false;
    }
private:
    static const MaterialParamDescriptor kParams[];
};

inline const MaterialParamDescriptor FresnelNode::kParams[] = {
    {"power", "Fresnel", MaterialParamType::Float, {0.0f, 20.0f, 0.1f},
     MAT_OFFSETOF(FresnelNode, power), sizeof(power), nullptr},
};

// NormalBlend: UDN blend formula — normalize(Base.xy + Detail.xy, Base.z * Detail.z)
class NormalBlendNode : public MaterialNode {
public:
    NormalBlendNode() {
        inputs.resize(2);
        inputs[0].expected_type = MaterialDataType::Float3; inputs[0].index = 0;
        inputs[1].expected_type = MaterialDataType::Float3; inputs[1].index = 1;
        outputs.resize(1);
        outputs[0].expected_type = MaterialDataType::Float3; outputs[0].index = 0;
    }
    const char* TypeName() const override { return "NormalBlend"; }
    void Execute() override {
        auto* base = inputs[0].AsFloat3();
        auto* detail = inputs[1].AsFloat3();
        if (base && detail) {
            f32 x = base->value.x + detail->value.x;
            f32 y = base->value.y + detail->value.y;
            f32 z = base->value.z * detail->value.z;
            f32 len = std::sqrt(x * x + y * y + z * z);
            if (len > 0.0f) { x /= len; y /= len; z /= len; }
            auto* out = CreateOutput<MaterialFloat3Data>(0);
            out->value = {x, y, z};
        }
    }
};

} // namespace primal::graphics::material_graph
