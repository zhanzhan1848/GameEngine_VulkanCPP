// Engine/Graphics/WFC/WFCTileStyleRegistry.cpp
#include "WFCTileStyleRegistry.h"
#include "IWFCTileStyle.h"
#include "TileAdjacency.h"
#include "WFCTileRegistry.h"

#include <cstring>

namespace primal::graphics::wfc {

WFCTileStyleRegistry& WFCTileStyleRegistry::Instance() {
    static WFCTileStyleRegistry instance;
    return instance;
}

void WFCTileStyleRegistry::RegisterStyle(std::unique_ptr<IWFCTileStyle> style) {
    if (!style) return;
    styles_.push_back(std::move(style));
}

void WFCTileStyleRegistry::Clear() {
    styles_.clear();
}

u32 WFCTileStyleRegistry::GetStyleCount() const {
    return static_cast<u32>(styles_.size());
}

const IWFCTileStyle* WFCTileStyleRegistry::GetStyle(u32 index) const {
    if (index >= styles_.size()) return nullptr;
    return styles_[index].get();
}

const IWFCTileStyle* WFCTileStyleRegistry::FindByName(const char* name) const {
    if (!name) return nullptr;
    for (const auto& s : styles_) {
        if (std::strcmp(s->GetName(), name) == 0) return s.get();
    }
    return nullptr;
}

u32 WFCTileStyleRegistry::Compose(const std::vector<u32>& style_indices,
                                  WFCTileRegistry&         registry,
                                  TileAdjacencyTable&      adjacency,
                                  std::vector<const rhi::RHIMeshAsset*>& assets_out,
                                  u64&                     out_category_mask) const {
    adjacency.Clear();
    assets_out.clear();
    out_category_mask = 0;

    if (style_indices.empty()) return 0;

    // The caller is responsible for ensuring `registry` is fresh (no tiles
    // already registered). The test binary recreates the registry on each
    // mode switch; WFCTileRegistry has no Clear() method by design.
    if (registry.Count() != 0) return 0;

    u32 total_count = 0;
    for (u32 idx : style_indices) {
        const IWFCTileStyle* s = GetStyle(idx);
        if (!s) return 0;

        const u32 before = registry.Count();
        const u32 added  = s->AppendTiles(registry, assets_out);
        total_count += added;

        // Defensive sanity: AppendTiles must register exactly `added` tiles.
        // Mismatch signals a Style bug (registered fewer than promised, or
        // forgot to register some tiles before returning).
        if (registry.Count() != before + added) return 0;

        out_category_mask |= CategoryMaskFor(s->GetCategory());
    }

    if (total_count == 0) return 0;

    // Build adjacency uniformly via the AutoSocketClassifier. The lookup
    // returns nullptr for variant > 0 — every Style in this registry exposes
    // only variant-0 meshes; higher variants are synthesized at solver time
    // via the variant_transform passed to ClassifyFace.
    auto lookup = [&assets_out](u32 tile_id, u32 variant) -> const rhi::RHIMeshAsset* {
        if (variant != 0) return nullptr;
        if (tile_id >= assets_out.size()) return nullptr;
        return assets_out[tile_id];
    };
    adjacency.BuildFromClassifier(registry, lookup);

    return total_count;
}

} // namespace primal::graphics::wfc
