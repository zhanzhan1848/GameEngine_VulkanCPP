#pragma once

#include "Graphics/PCG/PCGNode.h"
#include <cstring>

namespace primal::graphics::pcg {

// Outputs a procedural noise field. Supports Simplex (FBM), Worley (cellular), and Ridged variants.
// Typically wired to FieldScatterNode as a density input to create natural variation.
//
// Pin layout:
//   Outputs: [0] Field — PCGNoiseField with selected noise type
//   Inputs:  (none)
//
// Parameters:
//   noise_type  — Simplex (default), Worley, or Ridged noise
//   frequency   — spatial frequency of the base noise (lower = smoother patterns)
//   octaves     — number of noise layers summed (more = finer detail, higher cost)
//   lacunarity  — frequency multiplier between octaves (typically 2.0)
//   persistence — amplitude decay between octaves (typically 0.5)
//   seed        — permutation table seed for reproducible results
//
// Noise types:
//   Simplex — FBM-layered gradient noise, output ~[-1,1]. Good for organic density variation.
//   Worley  — Cellular (Voronoi) distance to nearest feature point, output ~[0,1.1].
//             Creates island/patch patterns.
//   Ridged  — Sharp ridge patterns from inverted abs(Simplex), output [0,1]. Good for
//             mountain-like or vein structures.
class NoiseFieldNode : public PCGNode {
public:
    PCGNoiseField::NoiseType noise_type{PCGNoiseField::NoiseType::Simplex};
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
        field->noise_type = noise_type;
        field->frequency = frequency;
        field->lacunarity = lacunarity;
        field->persistence = persistence;
        field->octaves = octaves;
        field->seed = seed;
    }

    // --- Reflection ---
    const PCGParamDescriptor* GetParamDescriptors(u32& out_count) const override {
        out_count = kParamCount;
        return kParams;
    }
    const PCGPinDescriptor* GetPinDescriptors(u32& out_count) const override {
        out_count = kPinCount;
        return kPins;
    }
    bool SetParamByName(const char* name, f32 value) override {
        if (std::strcmp(name, "frequency") == 0)   { frequency = value; return true; }
        if (std::strcmp(name, "lacunarity") == 0)  { lacunarity = value; return true; }
        if (std::strcmp(name, "persistence") == 0) { persistence = value; return true; }
        if (std::strcmp(name, "octaves") == 0)     { octaves = static_cast<u32>(value); return true; }
        if (std::strcmp(name, "seed") == 0)        { seed = static_cast<u32>(value); return true; }
        if (std::strcmp(name, "noise_type") == 0)  { noise_type = static_cast<PCGNoiseField::NoiseType>(static_cast<int>(value)); return true; }
        return false;
    }

private:
    static constexpr u32 kParamCount = 6;
    static constexpr u32 kPinCount = 1;
    static const PCGParamDescriptor kParams[];
    static const PCGPinDescriptor kPins[];
};

inline const PCGParamDescriptor NoiseFieldNode::kParams[] = {
    {"noise_type",  "Noise", PCGParamType::Enum,   {0,2,1},               PCG_OFFSETOF(NoiseFieldNode, noise_type),  sizeof(noise_type),  "Simplex,Worley,Ridged"},
    {"frequency",   "Noise", PCGParamType::Float,  {0.001f,1.0f,0.001f},  PCG_OFFSETOF(NoiseFieldNode, frequency),   sizeof(frequency),   nullptr},
    {"lacunarity",  "Noise", PCGParamType::Float,  {1.0f,4.0f,0.1f},      PCG_OFFSETOF(NoiseFieldNode, lacunarity),  sizeof(lacunarity),  nullptr},
    {"persistence", "Noise", PCGParamType::Float,  {0.0f,1.0f,0.01f},     PCG_OFFSETOF(NoiseFieldNode, persistence), sizeof(persistence), nullptr},
    {"octaves",     "Noise", PCGParamType::UInt,   {1,8,1},               PCG_OFFSETOF(NoiseFieldNode, octaves),     sizeof(octaves),     nullptr},
    {"seed",        "Noise", PCGParamType::UInt,   {0,9999,1},            PCG_OFFSETOF(NoiseFieldNode, seed),        sizeof(seed),        nullptr},
};
inline const PCGPinDescriptor NoiseFieldNode::kPins[] = {
    {"noise_field", 0, PCGDataType::Field, false},
};

} // namespace primal::graphics::pcg
