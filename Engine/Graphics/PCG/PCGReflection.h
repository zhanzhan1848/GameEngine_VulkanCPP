#pragma once

#include "CommonHeaders.h"
#include "Graphics/PCG/PCGTypes.h"
#include <cstddef>

namespace primal::graphics::pcg {

// Parameter types recognized by the reflection system.
// Each type maps to a specific C++ storage and UI control:
//   Bool       → bool (checkbox)
//   Float      → f32  (slider + spin box)
//   Int        → i32  (spin box)
//   UInt       → u32  (spin box)
//   Enum       → u8/u32 backed enum (dropdown, labels from enum_names)
//   Vec3       → math::v3 (3 sliders for X/Y/Z)
//   FloatArray → std::vector<f32> (dynamic list of sliders)
enum class PCGParamType : u8 {
    Bool,
    Float,
    Int,
    UInt,
    Enum,
    Vec3,
    FloatArray
};

// Range constraints for a numeric parameter.
// UI layer uses these to configure slider/spinbox bounds and step size.
struct PCGParamRange {
    f32 min_val{0.0f};
    f32 max_val{1.0f};
    f32 step{0.01f};
};

// Describes a single node parameter for the editor reflection system.
//
// Typical usage (editor layer):
//   u32 count;
//   auto* descs = node->GetParamDescriptors(count);
//   for (u32 i = 0; i < count; ++i) {
//       auto& d = descs[i];
//       // Create UI control based on d.type, with d.range for limits
//       // Read current value via: *(T*)((char*)node + d.offset)
//       // Write value via: node->SetParamByName(d.name, new_value)
//   }
//
// offset/size:
//   For Bool/Float/Int/UInt/Enum/Vec3, offset is offsetof(NodeType, member)
//   and size is sizeof(member). The editor can read values directly via pointer.
//   For FloatArray, offset/size are 0 (stored in std::vector, accessed via
//   SetParamArrayByName/GetParamArrayByName).
//
// enum_names:
//   Comma-separated label string for Enum type, e.g. "Simplex,Worley,Ridged".
//   nullptr for non-Enum types.
struct PCGParamDescriptor {
    const char* name;         // "frequency", "bounds_min"
    const char* group;        // "Noise", "Scatter", "Transform"
    PCGParamType type;
    PCGParamRange range;      // Slider/spinbox limits
    u32 offset;               // offsetof(NodeType, member)
    u32 size;                 // sizeof(member)
    const char* enum_names;   // "Simplex,Worley,Ridged" or nullptr
};

// Describes a single input or output pin on a node.
// UI layer uses these to render pin connectors and validate connection types.
//
// is_input:
//   true  → input pin (data flows in; populated by graph before Execute())
//   false → output pin (data flows out; set by node during Execute())
struct PCGPinDescriptor {
    const char* name;         // "density_field", "points"
    u32 index;                // Pin index within the node
    PCGDataType data_type;    // Field, PointSet, etc.
    bool is_input;
};

// offsetof helper for descriptor arrays. Must be used outside the class body
// (after closing brace) because offsetof requires a complete type.
#define PCG_OFFSETOF(T, m) static_cast<u32>(offsetof(T, m))

} // namespace primal::graphics::pcg
