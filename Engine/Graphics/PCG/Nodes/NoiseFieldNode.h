#pragma once

#include "Graphics/PCG/PCGNode.h"

namespace primal::graphics::pcg {

// Outputs a procedural noise field using FBM (Fractal Brownian Motion) Simplex noise.
// Typically wired to FieldScatterNode as a density input to create natural variation.
//
// Pin layout:
//   Outputs: [0] Field — PCGNoiseField with FBM sampling
//   Inputs:  (none)
//
// Parameters:
//   frequency   — spatial frequency of the base noise (lower = smoother patterns)
//   octaves     — number of noise layers summed (more = finer detail, higher cost)
//   lacunarity  — frequency multiplier between octaves (typically 2.0)
//   persistence — amplitude decay between octaves (typically 0.5)
//   seed        — permutation table seed for reproducible results
//
// Example: frequency=0.05, octaves=4 produces smooth rolling hills suitable
// for vegetation scatter density.
class NoiseFieldNode : public PCGNode {
public:
    f32 frequency{0.02f};
    f32 lacunarity{2.0f};
    f32 persistence{0.5f};
    u32 octaves{6};
    u32 seed{0};

    NoiseFieldNode() {
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::Field;
    }

    const char* TypeName() const override { return "NoiseField"; }

    void Execute() override {
        auto* field = CreateOutput<PCGNoiseField>(0);
        field->frequency = frequency;
        field->lacunarity = lacunarity;
        field->persistence = persistence;
        field->octaves = octaves;
        field->seed = seed;
    }
};

} // namespace primal::graphics::pcg
