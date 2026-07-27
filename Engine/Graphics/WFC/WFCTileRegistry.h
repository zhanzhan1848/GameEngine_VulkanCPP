#pragma once

#include "../../Common/CommonHeaders.h"
#include "../../Utilities/Vector.h"
#include "WFCTypes.h"

namespace primal::graphics::wfc {

class WFCTileRegistry {
public:
    // Registers a tile and returns its handle. The registry owns a copy.
    // id is auto-assigned (sequential starting at 0).
    wfc_tile_id Register(const WFCTile& tile);

    const WFCTile& Get(wfc_tile_id id) const;
    WFCTile&       GetMutable(wfc_tile_id id);

    u32 Count() const { return static_cast<u32>(tiles_.size()); }
    u32 MaxVariants() const { return max_variants_; }

private:
    utl::vector<WFCTile> tiles_;
    u32                  max_variants_{0};
};

} // namespace primal::graphics::wfc
