// Engine/Graphics/WFC/WFCCategory.h
#pragma once

#include "../../Common/CommonHeaders.h"

namespace primal::graphics::wfc {

// Tile category for grouping tiles into thematic sets (Phase C.1).
// Used by category-mask filtering during tile candidate selection so that
// a solver can restrict the wave to a single category (e.g. Ruins-only).
enum class WFCCategory : u8 {
    Primitive = 0,
    Ruins     = 1,
    Dungeon   = 2,
    Cyber     = 3,
    Organic   = 4,
};

constexpr u32 kWFCCategoryCount = 5;

// Single-bit mask for a category. u64 so 64 categories can be represented
// without widening later; small enums promote cleanly into the high bits.
constexpr u64 CategoryMaskFor(WFCCategory c) {
    return 1ULL << static_cast<u32>(c);
}

constexpr bool CategoryInMask(WFCCategory c, u64 mask) {
    return (mask & CategoryMaskFor(c)) != 0;
}

} // namespace primal::graphics::wfc
