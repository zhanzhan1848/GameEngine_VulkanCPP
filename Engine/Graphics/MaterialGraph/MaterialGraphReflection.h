#pragma once

#include "CommonHeaders.h"
#include "Graphics/MaterialGraph/MaterialGraphTypes.h"
#include <cstddef>

namespace primal::graphics::material_graph {

enum class MaterialParamType : u8 {
    Bool,
    Float,
    Float2,
    Float3,
    Float4,
    Int,
    UInt,
    Enum,
    Texture,
    Curve
};

struct MaterialParamRange {
    f32 min_val{0.0f};
    f32 max_val{1.0f};
    f32 step{0.01f};
};

struct MaterialParamDescriptor {
    const char* name;
    const char* group;
    MaterialParamType type;
    MaterialParamRange range;
    u32 offset;
    u32 size;
    const char* enum_names;
};

struct MaterialPinDescriptor {
    const char* name;
    u32 index;
    MaterialDataType data_type;
    bool is_input;
};

#define MAT_OFFSETOF(T, m) static_cast<u32>(offsetof(T, m))

} // namespace primal::graphics::material_graph
