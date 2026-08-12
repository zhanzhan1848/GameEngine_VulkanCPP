#pragma once

#include "Graphics/PCG/PCGNode.h"
#include <cstdlib>
#include <numeric>
#include <cstring>

namespace primal::graphics::pcg {

class MeshAssignNode : public PCGNode {
public:
    std::vector<f32> weights{1.0f};
    std::vector<u32> techniques; // per-slot ShaderTechnique value (default: all Opaque=0)

    MeshAssignNode() {
        inputs.resize(1);
        inputs[0].expected_type = PCGDataType::PointSet;
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::PointSet;
    }

    const char* TypeName() const override { return "MeshAssign"; }

    void Execute() override {
        auto* src = inputs[0].AsPointSet();
        if (!src) return;

        auto* out = CreateOutput<PCGPointSet>(0);
        out->Init(src->count, src->attr_stride);
        out->positions = src->positions;
        out->attrs = src->attrs;

        if (weights.empty()) return;

        std::vector<f32> cumulative(weights.size());
        std::partial_sum(weights.begin(), weights.end(), cumulative.begin());
        f32 total = cumulative.back();
        if (total <= 0.0f) return;

        std::srand(12345);
        for (u32 i = 0; i < out->count; ++i) {
            f32 r = (std::rand() / f32(RAND_MAX)) * total;
            u32 slot = 0;
            for (; slot < cumulative.size() - 1; ++slot) {
                if (r < cumulative[slot]) break;
            }
            out->SetAttr(i, PCGAttr::MeshIndex, static_cast<f32>(slot));
            u32 tech = (slot < techniques.size()) ? techniques[slot] : 0;
            out->SetAttr(i, PCGAttr::TechniqueIndex, static_cast<f32>(tech));
        }
    }

    const PCGParamDescriptor* GetParamDescriptors(u32& out_count) const override {
        out_count = kParamCount; return kParams;
    }
    const PCGPinDescriptor* GetPinDescriptors(u32& out_count) const override {
        out_count = kPinCount; return kPins;
    }
    bool SetParamArrayByName(const char* n, const f32* values, u32 count) override {
        if (std::strcmp(n, "weights") == 0) {
            weights = std::vector<f32>(values, values + count);
            return true;
        }
        if (std::strcmp(n, "techniques") == 0) {
            techniques.clear();
            techniques.reserve(count);
            for (u32 i = 0; i < count; ++i)
                techniques.push_back(static_cast<u32>(values[i]));
            return true;
        }
        return false;
    }

private:
    static constexpr u32 kParamCount = 2;
    static constexpr u32 kPinCount = 2;
    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor kPins[];
};

inline const PCGParamDescriptor MeshAssignNode::kParams[] = {
    {"weights", "Mesh", PCGParamType::FloatArray, {0.0f,1.0f,0.01f}, 0, 0, nullptr},
    {"techniques", "Mesh", PCGParamType::FloatArray, {0.0f,6.0f,1.0f}, 0, 0, nullptr},
};
inline const PCGPinDescriptor MeshAssignNode::kPins[] = {
    {"points", 0, PCGDataType::PointSet, true},
    {"points", 0, PCGDataType::PointSet, false},
};

} // namespace primal::graphics::pcg
