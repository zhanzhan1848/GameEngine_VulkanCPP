#pragma once

#include "Graphics/PCG/PCGNode.h"
#include <cstdlib>
#include <cmath>

namespace primal::graphics::pcg {

// Applies random per-point scale, rotation, and position jitter to a point set.
// Reads existing point positions and writes ScaleX/Y/Z attributes.
//
// Pin layout:
//   Inputs:  [0] PointSet — input points
//   Outputs: [0] PointSet — same points with updated Scale attributes and jittered positions
//
// Parameters:
//   scale_min/max     — random scale range per axis. Each axis gets an independent
//                        uniform random value in [min, max].
//   rotation_range    — maximum rotation angle in radians (default 2*PI = full range).
//                        Phase 1: stored but not applied to output matrices.
//   position_jitter   — maximum horizontal displacement added to each point's x/z.
//                        Set to 0 (default) for no jitter.
//   seed              — random seed for reproducible transforms
class TransformNode : public PCGNode {
public:
    math::v3 scale_min{0.8f, 0.8f, 0.8f};
    math::v3 scale_max{1.2f, 1.2f, 1.2f};
    f32 rotation_range{6.2832f}; // 2*PI
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

            if (position_jitter > 0.0f) {
                f32 jx = (std::rand() / f32(RAND_MAX) - 0.5f) * position_jitter;
                f32 jz = (std::rand() / f32(RAND_MAX) - 0.5f) * position_jitter;
                out->positions[i].x += jx;
                out->positions[i].z += jz;
            }
        }
    }
};

} // namespace primal::graphics::pcg
