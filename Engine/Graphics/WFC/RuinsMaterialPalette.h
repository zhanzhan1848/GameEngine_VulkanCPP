// Engine/Graphics/WFC/RuinsMaterialPalette.h
#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/MathTypes.h"

namespace primal::graphics::wfc {

// Flat-color material descriptor for ruins tiles (Phase C.1 §4).
// `albedo_tint` is the linear RGB flat color (no texture); `metallic` and
// `roughness` follow the PBR conventions used by the engine's ForwardPBR /
// DeferredLighting pipelines. Ruins tiles are mostly dielectric, so
// `metallic` is 0 for every entry.
struct RuinsMaterial {
    math::v3 albedo_tint;
    f32      metallic;
    f32      roughness;
};

constexpr u32 kRuinsPaletteCount = 8;

extern const RuinsMaterial kRuinsPalette[kRuinsPaletteCount];

// Returns the engine-global material_asset id for palette index `idx`.
//
// First call for a given idx registers the material (engine must be
// initialized); subsequent calls return the cached id.
//
// NOTE: as of Phase C.1 Task 6, the engine does NOT expose a public
// `register_material_asset` / `MaterialAssetDesc` API for ad-hoc flat-color
// registration from outside the Content pipeline. The closest existing
// helpers (`Engine::Content::create_material_resource`,
// `GPUMaterialRegistry::RegisterMaterial`, `RenderSystem::RegisterMaterialInstance`)
// all require either a serialized material blob or an existing
// `MaterialInstance*`. None of those are available in a unit test that
// cannot boot the RHI.
//
// Until a flat-color registration API is exposed (tracked as Phase C.1 M2
// follow-up), this function returns `palette_idx` itself as a stable
// placeholder id. The two consumers of this id (T7-T11 mesh generators)
// are not yet implemented, so there is no caller to break.
//
// Tests that cannot boot the engine should treat this as "stable for a
// given process" and not assert the value.
u32 GetRuinsMaterialId(u32 palette_idx);

} // namespace primal::graphics::wfc
