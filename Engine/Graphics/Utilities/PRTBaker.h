#pragma once
#include "CommonHeaders.h"
#include "Utilities/SphericalHarmonics.h"

namespace primal::graphics::utl
{
    // 同 BVH.hpp:命名空间打开会遮蔽 primal::utl,显式引入 vector 使遮蔽无害化。
    using primal::utl::vector;

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
