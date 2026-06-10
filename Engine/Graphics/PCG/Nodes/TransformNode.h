#pragma once

#include "Graphics/PCG/PCGNode.h"
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace primal::graphics::pcg {

class TransformNode : public PCGNode {
public:
    math::v3 scale_min{0.8f, 0.8f, 0.8f};
    math::v3 scale_max{1.2f, 1.2f, 1.2f};
    f32 rotation_range{6.2832f};
    f32 position_jitter{0.0f};
    u32 seed{0};

    TransformNode() {
        inputs.resize(1);
        inputs[0].expected_type = PCGDataType::PointSet;
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::PointSet;
    }

    const char* TypeName() const override { return "Transform"; }

    void Execute() override {
        auto* src = inputs[0].AsPointSet();
        if (!src) return;

        auto* out = CreateOutput<PCGPointSet>(0);
        out->Init(src->count, src->attr_stride);
        out->positions = src->positions;
        out->attrs = src->attrs;

        std::srand(seed);
        for (u32 i = 0; i < out->count; ++i) {
            f32 rx = std::rand() / f32(RAND_MAX);
            f32 ry = std::rand() / f32(RAND_MAX);
            f32 rz = std::rand() / f32(RAND_MAX);

            out->SetAttr(i, PCGAttr::ScaleX, scale_min.x + rx * (scale_max.x - scale_min.x));
            out->SetAttr(i, PCGAttr::ScaleY, scale_min.y + ry * (scale_max.y - scale_min.y));
            out->SetAttr(i, PCGAttr::ScaleZ, scale_min.z + rz * (scale_max.z - scale_min.z));

            if (rotation_range > 0.0f) {
                f32 rot_angle = (std::rand() / f32(RAND_MAX)) * rotation_range;
                out->SetAttr(i, PCGAttr::RotationY, rot_angle);
            }

            if (position_jitter > 0.0f) {
                f32 jx = (std::rand() / f32(RAND_MAX) - 0.5f) * position_jitter;
                f32 jz = (std::rand() / f32(RAND_MAX) - 0.5f) * position_jitter;
                out->positions[i].x += jx;
                out->positions[i].z += jz;
            }
        }
    }

    const PCGParamDescriptor* GetParamDescriptors(u32& out_count) const override {
        out_count = kParamCount; return kParams;
    }
    const PCGPinDescriptor* GetPinDescriptors(u32& out_count) const override {
        out_count = kPinCount; return kPins;
    }
    bool SetParamByName(const char* n, f32 v) override {
        if (std::strcmp(n, "rotation_range") == 0)  { rotation_range = v; return true; }
        if (std::strcmp(n, "position_jitter") == 0) { position_jitter = v; return true; }
        if (std::strcmp(n, "seed") == 0) { seed = static_cast<u32>(v); return true; }
        return false;
    }
    bool SetParamByName(const char* n, math::v3 v) override {
        if (std::strcmp(n, "scale_min") == 0) { scale_min = v; return true; }
        if (std::strcmp(n, "scale_max") == 0) { scale_max = v; return true; }
        return false;
    }

private:
    static constexpr u32 kParamCount = 5;
    static constexpr u32 kPinCount = 2;
    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor kPins[];
};

inline const PCGParamDescriptor TransformNode::kParams[] = {
    {"scale_min",       "Transform", PCGParamType::Vec3,  {0.0f,10.0f,0.01f},  PCG_OFFSETOF(TransformNode, scale_min),       sizeof(scale_min), nullptr},
    {"scale_max",       "Transform", PCGParamType::Vec3,  {0.0f,10.0f,0.01f},  PCG_OFFSETOF(TransformNode, scale_max),       sizeof(scale_max), nullptr},
    {"rotation_range",  "Transform", PCGParamType::Float, {0.0f,6.2832f,0.01f}, PCG_OFFSETOF(TransformNode, rotation_range), sizeof(rotation_range), nullptr},
    {"position_jitter", "Transform", PCGParamType::Float, {0.0f,10.0f,0.01f},  PCG_OFFSETOF(TransformNode, position_jitter),sizeof(position_jitter), nullptr},
    {"seed",            "Transform", PCGParamType::UInt,  {0,9999,1},           PCG_OFFSETOF(TransformNode, seed),            sizeof(seed), nullptr},
};
inline const PCGPinDescriptor TransformNode::kPins[] = {
    {"points", 0, PCGDataType::PointSet, true},
    {"points", 0, PCGDataType::PointSet, false},
};

} // namespace primal::graphics::pcg
