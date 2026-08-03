// Engine/Graphics/WFC/RuinsMaterialPalette.cpp
// Phase C.1 §4 — Ruins flat-color material palette.
//
// Values are hand-picked linear-RGB tints + PBR scalars for the seven
// canonical ruins variants. The same data drives both the procedural mesh
// generators (T7-T11) and, eventually, the flat-color material asset
// registration path. See header for why GetRuinsMaterialId is currently
// a placeholder.

#include "RuinsMaterialPalette.h"

#include "../../Common/CommonHeaders.h"

namespace primal::graphics::wfc {

const RuinsMaterial kRuinsPalette[kRuinsPaletteCount] = {
    /* 0 stone_gray        */ {math::v3{0.45f, 0.45f, 0.45f}, 0.0f, 0.85f},
    /* 1 stone_dark        */ {math::v3{0.30f, 0.30f, 0.32f}, 0.0f, 0.90f},
    /* 2 moss_green        */ {math::v3{0.35f, 0.45f, 0.20f}, 0.0f, 0.95f},
    /* 3 wood_brown        */ {math::v3{0.40f, 0.25f, 0.12f}, 0.0f, 0.80f},
    /* 4 rubble_earth      */ {math::v3{0.32f, 0.26f, 0.18f}, 0.0f, 1.00f},
    /* 5 weathered_lime    */ {math::v3{0.55f, 0.50f, 0.42f}, 0.0f, 0.80f},
    /* 6 cracked_concrete  */ {math::v3{0.42f, 0.40f, 0.38f}, 0.0f, 0.75f},
};

u32 GetRuinsMaterialId(u32 palette_idx) {
    // TODO: wire to engine material registration once the engine exposes a
    // flat-color MaterialAssetDesc / register_material_asset API. See
    // header comment for why none of the current material APIs are usable
    // from the unit-test / procedural-tile path.
    //
    // Returning the index itself is a stable placeholder that lets T7-T11
    // mesh generators reference the palette without booting the RHI.
    if (palette_idx >= kRuinsPaletteCount) {
        return 0xFFFFFFFFu; // sentinel for invalid index
    }
    return palette_idx;
}

} // namespace primal::graphics::wfc
