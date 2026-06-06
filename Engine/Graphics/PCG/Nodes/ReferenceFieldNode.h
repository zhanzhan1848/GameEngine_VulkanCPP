#pragma once

#include "Graphics/PCG/PCGNode.h"
#include "Graphics/Field/FieldRegistry.h"

namespace primal::graphics::pcg {

// Outputs a reference field backed by FieldRegistry (e.g., GlobalSDF).
// Used to query signed distance to scene geometry for constraint filtering.
//
// Pin layout:
//   Outputs: [0] Field — PCGReferenceField with sampled SDF values
//   Inputs:  (none)
//
// Parameters:
//   semantic — which field to reference from FieldRegistry (default: GlobalSDF)
//
// Phase 1 limitation: Always returns ground plane SDF (y=0) regardless of semantic.
// Real FieldRegistry GPU readback integration is deferred to Phase 2+.
class ReferenceFieldNode : public PCGNode {
public:
    field::FieldSemantic semantic{field::FieldSemantic::GlobalSDF};

    ReferenceFieldNode() {
        outputs.resize(1);
        outputs[0].expected_type = PCGDataType::Field;
    }

    const char* TypeName() const override { return "ReferenceField"; }

    void Execute() override {
        auto* field = CreateOutput<PCGReferenceField>(0);
        field->semantic = semantic;
        field->BindRegistry();
    }
};

} // namespace primal::graphics::pcg
