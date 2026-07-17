#pragma once

#include "Engine/Graphics/Lumen/LumenTypes.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include <vector>

namespace primal::graphics::lumen {

struct ProbeCacheHeader {
    static constexpr u32 MAGIC{0x53504348}; // 'SPCH'
    // v3: grid changed from 8x4x8 to 9x5x9 so (count-1)*spacing = 64 exactly
    // covers Sponza's [-32,+32]³ bounds. v2's 8x4x8 left [+24,+32]×[+8,+16]
    // out-of-grid, producing a hard black edge on the right half of the screen.
    // Bump invalidates IDBFS-cached v2 volumes so live visitors re-seed.
    static constexpr u32 VERSION{3};

    u32 magic{MAGIC};
    u32 version{VERSION};
    u32 grid_dim_x{0};
    u32 grid_dim_y{0};
    u32 grid_dim_z{0};
    float spacing{0.0f};
    float origin[3]{0.0f, 0.0f, 0.0f};
    u32 probe_count{0};
    u32 bounce_count{0};
};

class StaticProbeVolume {
public:
    StaticProbeVolume() = default;
    ~StaticProbeVolume() = default;

    bool Initialize(rhi::RHIDeviceBase* device, const StaticProbeParams& params);
    void Shutdown();

    bool SaveToFile(const char* path) const;
    bool LoadFromFile(const char* path);
    bool UploadToGPU();

    // Populate every probe with an attenuated sky-seed (no ray tracing):
    //   irradiance L0 = sky_color × 0.15 × 2*sqrt(pi), L1..L8 = 0
    //   sky_sh L0     = sky_color × 0.15 × 2*sqrt(pi), L1..L8 = 0
    //   depth_mean    = ray_max_distance (no occlusion)
    //   depth_var     = 0
    //   sky_factor    = 1.0 (full sky visibility)
    // The 0.15 attenuation keeps the seed subtle — full-sky L0 makes every
    // surface as bright as direct sunlight, which swamps the image.
    // Used on WASM where the full CPU bake is too slow (20+ min) but the
    // runtime DDGI trace in Mode 11 converges from any non-zero seed within
    // ~60 frames. Mode 10 displays this seed as dim ambient sky light.
    void FillWithSkySeed(const math::v3& sky_color, float ray_max_distance);

    bool IsLoaded() const { return is_loaded_; }
    u32 ProbeCount() const { return grid_dim_x_ * grid_dim_y_ * grid_dim_z_; }
    const StaticProbeParams& GetParams() const { return params_; }

    rhi::ResourceHandle GetStaticIrradiance() const { return static_irradiance_; }
    rhi::ResourceHandle GetStaticSkySH() const { return static_sky_sh_; }
    rhi::ResourceHandle GetStaticDepthMean() const { return static_depth_mean_; }
    rhi::ResourceHandle GetStaticDepthVar() const { return static_depth_var_; }
    rhi::ResourceHandle GetStaticSkyFactor() const { return static_sky_factor_; }

    const math::v3* GetSkySH() const { return sky_sh_; }
    void SetSkySH(const math::v3 sh[9]) { memcpy(sky_sh_, sh, sizeof(sky_sh_)); }

    float* GetDepthMeanData() { return depth_mean_data_.data(); }
    float* GetDepthVarData() { return depth_var_data_.data(); }
    float* GetSkyFactorData() { return sky_factor_data_.data(); }
    math::v3* GetIrradianceData() { return irradiance_data_.data(); }
    math::v3* GetSkySHData() { return sky_sh_data_.data(); }

    void MarkLoaded() { is_loaded_ = true; }

    u32 GridDimX() const { return grid_dim_x_; }
    u32 GridDimY() const { return grid_dim_y_; }
    u32 GridDimZ() const { return grid_dim_z_; }
    float Spacing() const { return params_.spacing; }
    math::v3 Origin() const { return params_.origin; }

private:
    bool CreateGPUBuffers();
    void ReleaseGPUBuffers();

    rhi::RHIDeviceBase* device_{nullptr};
    StaticProbeParams params_{};
    bool is_loaded_{false};
    bool gpu_uploaded_{false};

    u32 grid_dim_x_{0};
    u32 grid_dim_y_{0};
    u32 grid_dim_z_{0};

    // CPU-side data storage
    // irradiance: [probeCount * 9] SH3 coefficients per probe, each float3
    std::vector<math::v3> irradiance_data_;
    // sky_sh: [probeCount * 9] per-probe sky SH contribution
    std::vector<math::v3> sky_sh_data_;
    // depth_mean: [probeCount * 64] octahedral depth mean per probe
    std::vector<float> depth_mean_data_;
    // depth_var: [probeCount * 64] octahedral depth variance per probe
    std::vector<float> depth_var_data_;
    // sky_factor: [probeCount] per-probe sky visibility factor
    std::vector<float> sky_factor_data_;

    // Global sky SH (9 coefficients)
    math::v3 sky_sh_[9]{};

    // GPU resource handles
    rhi::ResourceHandle static_irradiance_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle static_sky_sh_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle static_depth_mean_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle static_depth_var_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle static_sky_factor_{rhi::handles::INVALID_RESOURCE};
};

} // namespace primal::graphics::lumen
