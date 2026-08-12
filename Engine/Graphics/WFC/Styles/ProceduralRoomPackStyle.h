// Engine/Graphics/WFC/Styles/ProceduralRoomPackStyle.h
//
// ProceduralRoomPackStyle — IWFCTileStyle wrapper around ProceduralRoomPack's
// 12 procedurally-generated room tiles (3 footprints × 4 door configurations).
// All tiles are category=Primitive. Variant-0 RHIMeshAssets are owned by this
// Style and exposed to the AutoSocketClassifier via AppendTiles.
//
// Lazy loading: pack_meshes_ is built on the first AppendTiles call.
#pragma once

#include "../IWFCTileStyle.h"
#include "../../RHI/Core/RHIMeshAsset.h"
#include <vector>

namespace primal::graphics::wfc {

class ProceduralRoomPackStyle final : public IWFCTileStyle {
public:
    ProceduralRoomPackStyle() = default;

    const char* GetName() const override        { return "ProceduralRoomPack"; }
    const char* GetDisplayName() const override { return "Procedural Room Pack (12 tiles)"; }
    WFCCategory GetCategory() const override    { return WFCCategory::Primitive; }

    u32 AppendTiles(WFCTileRegistry& registry,
                    std::vector<const rhi::RHIMeshAsset*>& assets_out) const override;

private:
    mutable std::vector<rhi::RHIMeshAsset> pack_meshes_;
};

} // namespace primal::graphics::wfc
