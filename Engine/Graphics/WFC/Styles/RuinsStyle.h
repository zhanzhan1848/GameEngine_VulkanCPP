// Engine/Graphics/WFC/Styles/RuinsStyle.h
//
// RuinsStyle — IWFCTileStyle wrapper around WFCTileCatalog::Populate + the
// 15-tile per-tile procedural factory dispatch (cube / ramp / corners /
// pillar / broken / rubble / debris / etc.). All tiles are category=Ruins.
// Variant-0 RHIMeshAssets are owned by this Style and exposed to the
// AutoSocketClassifier via AppendTiles.
//
// Adjacency is NOT computed by this Style — Compose() runs the classifier
// uniformly over all selected styles' meshes. Populate's hand-written
// adjacency output is discarded.
//
// Lazy loading: ruins_meshes_ is built on the first AppendTiles call
// (mutable; AppendTiles is const per the IWFCTileStyle contract). Subsequent
// calls reuse the cached assets.
#pragma once

#include "../IWFCTileStyle.h"
#include "../../RHI/Core/RHIMeshAsset.h"
#include <vector>

namespace primal::graphics::wfc {

class RuinsStyle final : public IWFCTileStyle {
public:
    RuinsStyle() = default;

    const char* GetName() const override        { return "Ruins"; }
    const char* GetDisplayName() const override { return "Ruins (15 tiles)"; }
    WFCCategory GetCategory() const override    { return WFCCategory::Ruins; }

    u32 AppendTiles(WFCTileRegistry& registry,
                    std::vector<const rhi::RHIMeshAsset*>& assets_out) const override;

private:
    // Built lazily on first AppendTiles. mutable because AppendTiles is const
    // per the IWFCTileStyle contract (Compose holds a const Style pointer).
    mutable std::vector<rhi::RHIMeshAsset> ruins_meshes_;
};

} // namespace primal::graphics::wfc
