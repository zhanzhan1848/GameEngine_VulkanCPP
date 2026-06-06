#pragma once

#include "Graphics/PCG/PCGTypes.h"
#include <vector>
#include <memory>

namespace primal::graphics::pcg {

// Base class for all PCG graph nodes.
//
// Lifecycle:
//   1. Construct node and set public parameters (e.g., scatterNode->target_count = 1000)
//   2. Add to graph via PCGGraph::AddNode() — returns node ID
//   3. Wire inputs/outputs via PCGGraph::Connect()
//   4. PCGGraph::Execute() propagates data to input pins, then calls Execute()
//   5. Read output from output pins or via PCGGraph::GetOutputPoints()
//
// Each node declares its pin layout in the constructor by resizing inputs/outputs
// and setting expected_type on each pin. Pin indices are fixed per node type —
// see individual node headers for pin layout.
//
// To implement a custom node:
//   class MyNode : public PCGNode {
//   public:
//       MyNode() {
//           inputs.resize(1);   // 1 input: point set
//           inputs[0].expected_type = PCGDataType::PointSet;
//           outputs.resize(1);  // 1 output: point set
//           outputs[0].expected_type = PCGDataType::PointSet;
//       }
//       const char* TypeName() const override { return "MyNode"; }
//       void Execute() override {
//           auto* src = inputs[0].AsPointSet();
//           if (!src) return;
//           auto* out = CreateOutput<PCGPointSet>(0);
//           out->Init(src->count, src->attr_stride);
//           // ... transform src into out ...
//       }
//   };
class PCGNode {
public:
    virtual ~PCGNode() = default;
    virtual const char* TypeName() const = 0;

    // Called by PCGGraph::Execute after input pins have been populated.
    // Read inputs[], process data, write outputs[] via CreateOutput().
    virtual void Execute() = 0;

    std::vector<PCGPin> inputs;   // Input pins — data set by graph before Execute()
    std::vector<PCGPin> outputs;  // Output pins — data set by node during Execute()

protected:
    std::vector<std::unique_ptr<PCGData>> owned_data_;

    // Create a new data object, take ownership, and bind it to an output pin.
    // Returns raw pointer for immediate use. Lifetime managed by this node.
    template<typename T, typename... Args>
    T* CreateOutput(u32 pin_index, Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = ptr.get();
        owned_data_.push_back(std::move(ptr));
        if (pin_index < outputs.size()) {
            outputs[pin_index].data = raw;
        }
        return raw;
    }
};

} // namespace primal::graphics::pcg
