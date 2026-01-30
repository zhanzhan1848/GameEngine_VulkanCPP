#pragma once
#include "CommonHeaders.h"
#include "Utilities/SphericalHarmonics.h"

namespace primal::graphics::utl
{
    struct PRTBakingDesc
    {
        const math::v3* positions;
        const math::v3* normals;
        u32 vertex_count;
        const u32* indices;
        u32 index_count;
        u32 num_samples{ 1024 };
    };

    class PRTBaker
    {
    public:
        // Bake shadowed transfer (AO-like, projected to SH)
        // Output: vector of SH9 coefficients (one per vertex)
        static bool BakeShadowedTransfer(const PRTBakingDesc& desc, primal::utl::vector<math::sh::SH9>& out_coeffs);
    };
}
