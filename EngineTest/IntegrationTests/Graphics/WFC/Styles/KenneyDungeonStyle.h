// EngineTest/IntegrationTests/Graphics/WFC/Styles/KenneyDungeonStyle.h
//
// KenneyDungeonStyle — IWFCTileStyle wrapper around KenneyTileCatalog (8
// hand-picked Kenney dungeon tiles). Lives in EngineTest because the Kenney
// FBX assets are demo fixtures, not engine core (per the Core Layer as
// Capability Provider principle; see KenneyTileCatalog.h:1-11).
//
// This Style uses the AutoSocketClassifier path — it loads .engine_mesh files
// via KenneyTileCatalog, fetches the underlying RHIMeshAsset for each tile via
// content::get_rhi_mesh_asset, and exposes those pointers via AppendTiles.
// The hand-written byte-socket adjacency from KenneyTileCatalog::BuildWFCRegistry
// (Path 1, with its 0xAA vertical sentinel) is NOT used here — Compose() runs
// BuildFromClassifier uniformly over all selected styles, dropping the Y-sentinel
// behavior. See the plan's Risks section.
//
// Lazy loading: catalog_ + meshes_ are populated on the first AppendTiles call.
#pragma once

#include "Engine/Graphics/WFC/IWFCTileStyle.h"
#include "Engine/Graphics/RHI/Core/RHIMeshAsset.h"
#include "../KenneyTileCatalog.h"
#include <string>
#include <vector>

namespace primal::graphics { class StandardRenderPipeline; }

namespace primal::test::kenney {

class KenneyDungeonStyle final : public graphics::wfc::IWFCTileStyle {
public:
    KenneyDungeonStyle(graphics::StandardRenderPipeline* pipeline,
                       std::string dir_path,
                       std::string colormap_path = {})
        : pipeline_(pipeline)
        , dir_path_(std::move(dir_path))
        , colormap_path_(std::move(colormap_path)) {}

    const char* GetName() const override        { return "KenneyDungeon"; }
    const char* GetDisplayName() const override { return "Kenney Dungeon (8 tiles)"; }
    graphics::wfc::WFCCategory GetCategory() const override {
        return graphics::wfc::WFCCategory::Dungeon;
    }

    u32 AppendTiles(graphics::wfc::WFCTileRegistry& registry,
                    std::vector<const graphics::rhi::RHIMeshAsset*>& assets_out) const override;

private:
    graphics::StandardRenderPipeline* pipeline_;
    std::string                       dir_path_;
    std::string                       colormap_path_;

    mutable KenneyTileCatalog                    catalog_;
    mutable std::vector<graphics::rhi::RHIMeshAsset> meshes_;
    mutable bool                                  loaded_{false};
};

} // namespace primal::test::kenney
