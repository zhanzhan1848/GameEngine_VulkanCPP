#pragma once

#include "Graphics/PCG/PCGNode.h"
#include "Graphics/PCG/Nodes/ScatterContext.h"
#include <cstring>

namespace primal::graphics::pcg {

class DensityFilterNode : public PCGNode {
public:
    f32 min_density{0.0f};
    f32 max_density{1.0f};

    DensityFilterNode() {
        inputs.resize(1);
        inputs[0].expected_type = PCGDataType::PointSet;
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::PointSet;
    }

    const char* TypeName() const override { return "DensityFilter"; }

    void Execute() override {
        auto* src = inputs[0].AsPointSet();
        if (!src) return;

        auto* out = CreateOutput<PCGPointSet>(0);
        out->Init(src->count, src->attr_stride);
        out->positions = src->positions;
        out->attrs = src->attrs;

        ScatterContext::ApplyDensityFilter(*out, min_density, max_density);
    }

    const PCGParamDescriptor* GetParamDescriptors(u32& out_count) const override {
        out_count = kParamCount; return kParams;
    }
    const PCGPinDescriptor* GetPinDescriptors(u32& out_count) const override {
        out_count = kPinCount; return kPins;
    }
    bool SetParamByName(const char* n, f32 v) override {
        if (std::strcmp(n, "min_density") == 0) { min_density = v; return true; }
        if (std::strcmp(n, "max_density") == 0) { max_density = v; return true; }
        return false;
    }

private:
    static constexpr u32 kParamCount = 2;
    static constexpr u32 kPinCount = 2;
    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor kPins[];
};

inline const PCGParamDescriptor DensityFilterNode::kParams[] = {
    {"min_density", "Filter", PCGParamType::Float, {0.0f,1.0f,0.01f}, PCG_OFFSETOF(DensityFilterNode, min_density), sizeof(min_density), nullptr},
    {"max_density", "Filter", PCGParamType::Float, {0.0f,1.0f,0.01f}, PCG_OFFSETOF(DensityFilterNode, max_density), sizeof(max_density), nullptr},
};
inline const PCGPinDescriptor DensityFilterNode::kPins[] = {
    {"points", 0, PCGDataType::PointSet, true},
    {"points", 0, PCGDataType::PointSet, false},
};

} // namespace primal::graphics::pcg
