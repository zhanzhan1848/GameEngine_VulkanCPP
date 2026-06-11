#pragma once

#include "Graphics/PCG/PCGTypes.h"
#include "Graphics/PCG/PCGReflection.h"
#include <vector>
#include <memory>

namespace primal::graphics::pcg {

// Base class for all PCG graph nodes.
//
// Lifecycle:
//   1. Construct node and set public parameters (e.g., scatterNode->target_count = 1000)
//      — or use SetParamByName() for type-agnostic parameter setting
//   2. Add to graph via PCGGraph::AddNode() — returns node ID
//   3. Wire inputs/outputs via PCGGraph::Connect()
//   4. PCGGraph::Execute() propagates data to input pins, then calls Execute()
//   5. Read output from output pins or via PCGGraph::GetOutputPoints()
//
// Each node declares its pin layout in the constructor by resizing inputs/outputs
// and setting expected_type on each pin. Pin indices are fixed per node type —
// see individual node headers for pin layout.
//
// Reflection API:
//   Each concrete node overrides GetParamDescriptors/GetPinDescriptors to expose
//   parameter metadata for the editor UI. The base class provides empty defaults
//   so legacy nodes that haven't been updated still compile.
//
// To implement a custom node:
//   class MyNode : public PCGNode {
//   public:
//       f32 my_param{1.0f};
//
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
//
//       // Reflection — descriptor arrays defined AFTER class closing brace
//       // using PCG_OFFSETOF (requires complete type):
//       const PCGParamDescriptor* GetParamDescriptors(u32& c) const override { c = 1; return kParams; }
//       const PCGPinDescriptor* GetPinDescriptors(u32& c) const override { c = 2; return kPins; }
//       bool SetParamByName(const char* n, f32 v) override {
//           if (std::strcmp(n, "my_param") == 0) { my_param = v; return true; }
//           return false;
//       }
//
//   private:
//       static constexpr u32 kParamCount = 1;
//       static constexpr u32 kPinCount = 2;
//       static const PCGParamDescriptor kParams[];
//       static const PCGPinDescriptor kPins[];
//   };
//
//   // Define AFTER class — offsetof requires complete type
//   inline const PCGParamDescriptor MyNode::kParams[] = {
//       {"my_param", "MyGroup", PCGParamType::Float, {0.0f,10.0f,0.01f},
//        PCG_OFFSETOF(MyNode, my_param), sizeof(my_param), nullptr},
//   };
//   inline const PCGPinDescriptor MyNode::kPins[] = {
//       {"points", 0, PCGDataType::PointSet, true},
//       {"points", 0, PCGDataType::PointSet, false},
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

    // --- Reflection API (for editor/UI layer) ---

    // Returns a pointer to this node's static parameter descriptor array.
    // out_count is set to the number of descriptors. Returns nullptr if
    // the node doesn't expose parameters (default implementation).
    virtual const PCGParamDescriptor* GetParamDescriptors(u32& out_count) const {
        out_count = 0; return nullptr;
    }

    // Returns a pointer to this node's static pin descriptor array.
    // out_count is set to the number of descriptors.
    virtual const PCGPinDescriptor* GetPinDescriptors(u32& out_count) const {
        out_count = 0; return nullptr;
    }

    // Set a scalar parameter by name. Handles Float, UInt, Int, Enum, Bool.
    // Returns true if the parameter was found and set, false otherwise.
    // UInt/Int: value is cast to u32. Enum: value is cast to the enum's underlying type.
    virtual bool SetParamByName(const char* name, f32 value) { (void)name; (void)value; return false; }

    // Set a Vec3 parameter by name. Handles position/bounds/scale parameters.
    // Returns true if the parameter was found and set, false otherwise.
    virtual bool SetParamByName(const char* name, math::v3 value) { (void)name; (void)value; return false; }

    // Set a float array parameter by name. Currently only used by MeshAssignNode
    // for weights. Returns true if the parameter was found and set.
    virtual bool SetParamArrayByName(const char* name, const f32* values, u32 count) {
        (void)name; (void)values; (void)count; return false;
    }

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
