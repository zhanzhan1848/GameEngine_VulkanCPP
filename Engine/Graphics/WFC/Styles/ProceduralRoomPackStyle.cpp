// Engine/Graphics/WFC/Styles/ProceduralRoomPackStyle.cpp
#include "ProceduralRoomPackStyle.h"
#include "../ProceduralRoomPack.h"
#include "../WFCTileRegistry.h"
#include "../WFCTypes.h"
#include "../../../Utilities/MathTypes.h"

namespace primal::graphics::wfc {

u32 ProceduralRoomPackStyle::AppendTiles(
    WFCTileRegistry& registry,
    std::vector<const rhi::RHIMeshAsset*>& assets_out) const {
    const u32 start = registry.Count();

    // Lazy-build all 12 tile meshes on first call.
    if (pack_meshes_.empty()) {
        pack_meshes_.resize(ProceduralRoomPack::kTileCount);
        for (u32 i = 0; i < ProceduralRoomPack::kTileCount; ++i) {
            ProceduralRoomPack::GenerateTileMesh(i, pack_meshes_[i]);
        }
    }

    // Register each tile with metadata from TileDefs().
    for (u32 i = 0; i < ProceduralRoomPack::kTileCount; ++i) {
        WFCTile t{};
        t.name          = ProceduralRoomPack::TileDefs()[i].name;
        t.category      = WFCCategory::Primitive;
        t.variant_count = 1;
        t.bounds_extents = math::v3{1.0f, 1.0f, 1.0f};
        registry.Register(t);
    }

    const u32 added = registry.Count() - start;
    if (added == 0) return 0;

    if (assets_out.size() < start + added) {
        assets_out.resize(start + added, nullptr);
    }
    const u32 mesh_count = static_cast<u32>(pack_meshes_.size());
    for (u32 i = 0; i < added && i < mesh_count; ++i) {
        assets_out[start + i] = &pack_meshes_[i];
    }

    return added;
}

} // namespace primal::graphics::wfc
