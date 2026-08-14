// Engine/Graphics/WFC/WFCTileStyleRegistry.h
//
// WFCTileStyleRegistry — singleton catalogue of IWFCTileStyle instances.
// GUI binaries (TestKenneyTilePreview, future editors) register styles at
// startup; the WASM/HTML dropdown auto-populates from this registry; the
// Compose() helper builds a (registry, adjacency, mask) triple from any
// subset of registered styles for the WFC solver to consume.
//
// Composition contract:
//   - All registered styles use the AutoSocketClassifier path. There is no
//     "Path 1" / hand-written-adjacency path on the registry — styles that
//     previously shipped hand-written sockets must expose their meshes and
//     let the classifier regenerate adjacency.
//   - The composition clears registry, adjacency, and assets_out before
//     appending. Caller passes empty containers.
//   - out_category_mask is the OR of CategoryMaskFor(style->GetCategory())
//     over the selected styles; assign directly to WFCConfig.active_category_mask.
#pragma once

#include "../../Common/CommonHeaders.h"
#include "WFCCategory.h"
#include <memory>
#include <vector>

namespace primal::graphics::rhi { struct RHIMeshAsset; }

namespace primal::graphics::wfc {

class IWFCTileStyle;
class TileAdjacencyTable;
class WFCTileRegistry;

class WFCTileStyleRegistry {
public:
    static WFCTileStyleRegistry& Instance();

    // Register a style. The registry takes ownership. Registration order
    // defines the stable index used by GetStyle(), Compose(), and the WASM
    // C ABI (so the dropdown order is deterministic across rebuilds).
    void RegisterStyle(std::unique_ptr<IWFCTileStyle> style);

    // Remove all styles. Useful for tests that want a clean slate.
    void Clear();

    u32  GetStyleCount() const;
    const IWFCTileStyle* GetStyle(u32 index) const;        // nullptr if out of range
    const IWFCTileStyle* FindByName(const char* name) const;

    // Compose a subset of styles into a single (registry, adjacency, mask).
    //
    //   style_indices: indices into the registry (e.g. {0, 2} = first + third
    //     registered style). Empty vector is a no-op and returns 0.
    //   registry:     cleared, then populated with all selected styles' tiles.
    //   adjacency:    cleared, then rebuilt via TileAdjacencyTable::BuildFromClassifier
    //                 over the union of variant-0 meshes.
    //   assets_out:   cleared, then filled with const RHIMeshAsset* indexed by
    //                 assigned tile ID. Pointers remain valid as long as the
    //                 underlying Style objects remain alive (i.e., the registry
    //                 is not destroyed and Clear() is not called).
    //   out_category_mask: OR of CategoryMaskFor(style->GetCategory()).
    //
    // Returns total tile count, or 0 on failure (empty selection, missing
    // index, or AppendTiles failure).
    u32 Compose(const std::vector<u32>& style_indices,
                WFCTileRegistry&         registry,
                TileAdjacencyTable&      adjacency,
                std::vector<const rhi::RHIMeshAsset*>& assets_out,
                u64&                     out_category_mask) const;

private:
    WFCTileStyleRegistry() = default;
    WFCTileStyleRegistry(const WFCTileStyleRegistry&) = delete;
    WFCTileStyleRegistry& operator=(const WFCTileStyleRegistry&) = delete;

    std::vector<std::unique_ptr<IWFCTileStyle>> styles_;
};

} // namespace primal::graphics::wfc
