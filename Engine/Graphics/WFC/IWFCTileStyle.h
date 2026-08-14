// Engine/Graphics/WFC/IWFCTileStyle.h
//
// IWFCTileStyle — abstraction for a composable WFC tile source. Each
// implementation appends some number of tiles into a shared WFCTileRegistry
// and exposes variant-0 RHIMeshAsset pointers for the AutoSocketClassifier
// to ray-trace. Styles do NOT ship their own adjacency tables — the
// WFCTileStyleRegistry runs BuildFromClassifier uniformly over the union of
// all selected styles, so any subset of styles composes without per-pair glue.
//
// Lifetime: a Style object must outlive any WFCTileRegistry composed from it,
// because the registry's adjacency holds raw RHIMeshAsset pointers into the
// Style's internal storage. The registry (via WFCTileStyleRegistry::Compose)
// does not copy or take ownership of the assets.
//
// Adding a new style:
//   1. Implement IWFCTileStyle (override the four methods below).
//   2. Register an instance via WFCTileStyleRegistry::RegisterStyle before the
//      GUI/WASM dropdown is populated.
//   3. The style appears in the dropdown automatically; no other site needs
//      editing.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCCategory.h"
#include <vector>

namespace primal::graphics::rhi { struct RHIMeshAsset; }

namespace primal::graphics::wfc {

class WFCTileRegistry;

class IWFCTileStyle {
public:
    virtual ~IWFCTileStyle() = default;

    // Stable identifier (e.g. "Ruins", "KenneyDungeon"). Used for lookup and
    // must be unique within a WFCTileStyleRegistry. Return value must point to
    // static or stable storage (string literal or class member).
    virtual const char* GetName() const = 0;

    // Human-readable label for UI dropdowns (e.g. "Ruins (15 tiles)").
    virtual const char* GetDisplayName() const = 0;

    // Category this style contributes. Compositions OR these together via
    // CategoryMaskFor to build WFCConfig.active_category_mask.
    virtual WFCCategory GetCategory() const = 0;

    // Append this style's tiles to `registry` (caller has already Clear()ed
    // it). For each tile appended, push its variant-0 RHIMeshAsset* into
    // assets_out at the index equal to the tile's assigned ID. The Style MUST
    // retain ownership of the pointed-to assets for the lifetime of the
    // registry.
    //
    // assets_out is indexed by tile ID (NOT by per-style slot). If a Style is
    // composed alongside others, its tiles receive IDs offset by prior styles'
    // tile counts; the Style is responsible for reading back the assigned IDs
    // via the registry and placing pointers at the correct indices.
    //
    // Returns the count of tiles appended.
    virtual u32 AppendTiles(WFCTileRegistry& registry,
                            std::vector<const rhi::RHIMeshAsset*>& assets_out) const = 0;
};

} // namespace primal::graphics::wfc
