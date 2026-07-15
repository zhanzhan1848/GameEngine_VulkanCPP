#include "StaticProbeVolume.h"

#include <cmath>
#include <cstring>
#include <fstream>

namespace primal::graphics::lumen {

static constexpr float PI_F{3.14159265358979323846f};
// SH9 DC coefficient for a uniform radiance L projected over the sphere:
//   L_0 = (1/4π) ∫ L · Y_0 dΩ = L · 4π · Y_0 = L · 4π / (2√π) = L · 2√π
// Higher SH bands integrate to zero, so only L0 is non-zero.
static inline math::v3 UniformSkySH0(const math::v3& sky_color) {
    const float sh0_scale = 2.0f * std::sqrt(PI_F);
    return math::v3{sky_color.x * sh0_scale, sky_color.y * sh0_scale, sky_color.z * sh0_scale};
}

// ============================================================================
// Initialize / Shutdown
// ============================================================================

bool StaticProbeVolume::Initialize(rhi::RHIDeviceBase* device, const StaticProbeParams& params) {
    if (!device) return false;

    device_ = device;
    params_ = params;
    grid_dim_x_ = params.grid_dim_x;
    grid_dim_y_ = params.grid_dim_y;
    grid_dim_z_ = params.grid_dim_z;

    const u32 totalProbes = ProbeCount();

    // Allocate CPU-side data vectors
    irradiance_data_.resize(totalProbes * 9, math::v3{0.0f});
    sky_sh_data_.resize(totalProbes * 9, math::v3{0.0f});
    depth_mean_data_.resize(totalProbes * 64, 0.0f);
    depth_var_data_.resize(totalProbes * 64, 0.0f);
    sky_factor_data_.resize(totalProbes, 0.0f);

    memset(sky_sh_, 0, sizeof(sky_sh_));

    is_loaded_ = false;
    gpu_uploaded_ = false;

    return true;
}

void StaticProbeVolume::Shutdown() {
    ReleaseGPUBuffers();

    irradiance_data_.clear();
    irradiance_data_.shrink_to_fit();
    sky_sh_data_.clear();
    sky_sh_data_.shrink_to_fit();
    depth_mean_data_.clear();
    depth_mean_data_.shrink_to_fit();
    depth_var_data_.clear();
    depth_var_data_.shrink_to_fit();
    sky_factor_data_.clear();
    sky_factor_data_.shrink_to_fit();

    memset(sky_sh_, 0, sizeof(sky_sh_));

    device_ = nullptr;
    is_loaded_ = false;
    gpu_uploaded_ = false;
    grid_dim_x_ = grid_dim_y_ = grid_dim_z_ = 0;
}

// ============================================================================
// GPU Buffer Management
// ============================================================================

bool StaticProbeVolume::CreateGPUBuffers() {
    if (!device_) return false;

    const u32 totalProbes = ProbeCount();

    // Irradiance buffer: probeCount * 9 * float3
    {
        u64 size = (u64)totalProbes * 9 * sizeof(float) * 3;
        rhi::BufferDesc desc{};
        desc.size = size;
        desc.type = rhi::BufferType::Structured;
        desc.usage = rhi::GPUMemoryUsage::Dynamic;
        desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes * 9 * 3;
        desc.structured.elementStride = sizeof(float);
        static_irradiance_ = device_->CreateBuffer(desc);
    }

    // Sky SH buffer: probeCount * 9 * float3
    {
        u64 size = (u64)totalProbes * 9 * sizeof(float) * 3;
        rhi::BufferDesc desc{};
        desc.size = size;
        desc.type = rhi::BufferType::Structured;
        desc.usage = rhi::GPUMemoryUsage::Dynamic;
        desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes * 9 * 3;
        desc.structured.elementStride = sizeof(float);
        static_sky_sh_ = device_->CreateBuffer(desc);
    }

    // Depth mean buffer: probeCount * 64 * float
    {
        u64 size = (u64)totalProbes * 64 * sizeof(float);
        rhi::BufferDesc desc{};
        desc.size = size;
        desc.type = rhi::BufferType::Structured;
        desc.usage = rhi::GPUMemoryUsage::Dynamic;
        desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes * 64;
        desc.structured.elementStride = sizeof(float);
        static_depth_mean_ = device_->CreateBuffer(desc);
    }

    // Depth variance buffer: probeCount * 64 * float
    {
        u64 size = (u64)totalProbes * 64 * sizeof(float);
        rhi::BufferDesc desc{};
        desc.size = size;
        desc.type = rhi::BufferType::Structured;
        desc.usage = rhi::GPUMemoryUsage::Dynamic;
        desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes * 64;
        desc.structured.elementStride = sizeof(float);
        static_depth_var_ = device_->CreateBuffer(desc);
    }

    // Sky factor buffer: probeCount * float
    {
        u64 size = (u64)totalProbes * sizeof(float);
        rhi::BufferDesc desc{};
        desc.size = size;
        desc.type = rhi::BufferType::Structured;
        desc.usage = rhi::GPUMemoryUsage::Dynamic;
        desc.memoryUsage = rhi::GPUMemoryUsage::Dynamic;
        desc.structured.elementCount = totalProbes;
        desc.structured.elementStride = sizeof(float);
        static_sky_factor_ = device_->CreateBuffer(desc);
    }

    // Verify all buffers were created successfully
    return static_irradiance_ != rhi::handles::INVALID_RESOURCE
        && static_sky_sh_ != rhi::handles::INVALID_RESOURCE
        && static_depth_mean_ != rhi::handles::INVALID_RESOURCE
        && static_depth_var_ != rhi::handles::INVALID_RESOURCE
        && static_sky_factor_ != rhi::handles::INVALID_RESOURCE;
}

void StaticProbeVolume::ReleaseGPUBuffers() {
    if (static_irradiance_ != rhi::handles::INVALID_RESOURCE && device_) {
        device_->DestroyBuffer(static_irradiance_);
        static_irradiance_ = rhi::handles::INVALID_RESOURCE;
    }
    if (static_sky_sh_ != rhi::handles::INVALID_RESOURCE && device_) {
        device_->DestroyBuffer(static_sky_sh_);
        static_sky_sh_ = rhi::handles::INVALID_RESOURCE;
    }
    if (static_depth_mean_ != rhi::handles::INVALID_RESOURCE && device_) {
        device_->DestroyBuffer(static_depth_mean_);
        static_depth_mean_ = rhi::handles::INVALID_RESOURCE;
    }
    if (static_depth_var_ != rhi::handles::INVALID_RESOURCE && device_) {
        device_->DestroyBuffer(static_depth_var_);
        static_depth_var_ = rhi::handles::INVALID_RESOURCE;
    }
    if (static_sky_factor_ != rhi::handles::INVALID_RESOURCE && device_) {
        device_->DestroyBuffer(static_sky_factor_);
        static_sky_factor_ = rhi::handles::INVALID_RESOURCE;
    }
    gpu_uploaded_ = false;
}

// ============================================================================
// Upload to GPU
// ============================================================================

bool StaticProbeVolume::UploadToGPU() {
    if (!is_loaded_) return false;
    if (gpu_uploaded_) return true;
    if (!device_) return false;

    if (!CreateGPUBuffers()) return false;

    const u32 totalProbes = ProbeCount();

    // Upload irradiance data (simd::float3 has 16-byte stride, GPU expects flat float[3])
    {
        void* mapped = device_->MapBuffer(static_irradiance_);
        if (mapped) {
            float* dst = static_cast<float*>(mapped);
            u32 totalCoeffs = totalProbes * 9;
            for (u32 i = 0; i < totalCoeffs; ++i) {
                dst[i * 3 + 0] = irradiance_data_[i].x;
                dst[i * 3 + 1] = irradiance_data_[i].y;
                dst[i * 3 + 2] = irradiance_data_[i].z;
            }
            device_->UnmapBuffer(static_irradiance_);
        }
    }

    // Upload sky SH data
    {
        void* mapped = device_->MapBuffer(static_sky_sh_);
        if (mapped) {
            float* dst = static_cast<float*>(mapped);
            u32 totalCoeffs = totalProbes * 9;
            for (u32 i = 0; i < totalCoeffs; ++i) {
                dst[i * 3 + 0] = sky_sh_data_[i].x;
                dst[i * 3 + 1] = sky_sh_data_[i].y;
                dst[i * 3 + 2] = sky_sh_data_[i].z;
            }
            device_->UnmapBuffer(static_sky_sh_);
        }
    }

    // Upload depth mean data
    {
        u64 size = (u64)totalProbes * 64 * sizeof(float);
        void* mapped = device_->MapBuffer(static_depth_mean_);
        if (mapped) {
            memcpy(mapped, depth_mean_data_.data(), size);
            device_->UnmapBuffer(static_depth_mean_);
        }
    }

    // Upload depth variance data
    {
        u64 size = (u64)totalProbes * 64 * sizeof(float);
        void* mapped = device_->MapBuffer(static_depth_var_);
        if (mapped) {
            memcpy(mapped, depth_var_data_.data(), size);
            device_->UnmapBuffer(static_depth_var_);
        }
    }

    // Upload sky factor data
    {
        u64 size = (u64)totalProbes * sizeof(float);
        void* mapped = device_->MapBuffer(static_sky_factor_);
        if (mapped) {
            memcpy(mapped, sky_factor_data_.data(), size);
            device_->UnmapBuffer(static_sky_factor_);
        }
    }

    gpu_uploaded_ = true;
    return true;
}

// ============================================================================
// Serialization
// ============================================================================

bool StaticProbeVolume::SaveToFile(const char* path) const {
    if (!is_loaded_) return false;

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    ProbeCacheHeader header{};
    header.grid_dim_x = grid_dim_x_;
    header.grid_dim_y = grid_dim_y_;
    header.grid_dim_z = grid_dim_z_;
    header.spacing = params_.spacing;
    header.origin[0] = params_.origin.x;
    header.origin[1] = params_.origin.y;
    header.origin[2] = params_.origin.z;
    header.probe_count = ProbeCount();
    header.bounce_count = 3;

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    const u32 pc = ProbeCount();
    file.write(reinterpret_cast<const char*>(irradiance_data_.data()), static_cast<u64>(pc) * 9 * sizeof(math::v3));
    file.write(reinterpret_cast<const char*>(sky_sh_data_.data()), static_cast<u64>(pc) * 9 * sizeof(math::v3));
    file.write(reinterpret_cast<const char*>(depth_mean_data_.data()), static_cast<u64>(pc) * 64 * sizeof(float));
    file.write(reinterpret_cast<const char*>(depth_var_data_.data()), static_cast<u64>(pc) * 64 * sizeof(float));
    file.write(reinterpret_cast<const char*>(sky_factor_data_.data()), static_cast<u64>(pc) * sizeof(float));

    return file.good();
}

bool StaticProbeVolume::LoadFromFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;

    ProbeCacheHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.magic != ProbeCacheHeader::MAGIC || header.version != ProbeCacheHeader::VERSION)
        return false;

    grid_dim_x_ = header.grid_dim_x;
    grid_dim_y_ = header.grid_dim_y;
    grid_dim_z_ = header.grid_dim_z;
    params_.spacing = header.spacing;
    params_.origin = {header.origin[0], header.origin[1], header.origin[2]};
    params_.grid_dim_x = grid_dim_x_;
    params_.grid_dim_y = grid_dim_y_;
    params_.grid_dim_z = grid_dim_z_;

    const u32 pc = header.probe_count;
    irradiance_data_.resize(pc * 9);
    sky_sh_data_.resize(pc * 9);
    depth_mean_data_.resize(pc * 64);
    depth_var_data_.resize(pc * 64);
    sky_factor_data_.resize(pc);

    file.read(reinterpret_cast<char*>(irradiance_data_.data()), static_cast<u64>(pc) * 9 * sizeof(math::v3));
    file.read(reinterpret_cast<char*>(sky_sh_data_.data()), static_cast<u64>(pc) * 9 * sizeof(math::v3));
    file.read(reinterpret_cast<char*>(depth_mean_data_.data()), static_cast<u64>(pc) * 64 * sizeof(float));
    file.read(reinterpret_cast<char*>(depth_var_data_.data()), static_cast<u64>(pc) * 64 * sizeof(float));
    file.read(reinterpret_cast<char*>(sky_factor_data_.data()), static_cast<u64>(pc) * sizeof(float));

    if (!file.good()) {
        Shutdown();
        return false;
    }

    is_loaded_ = true;
    return true;
}

// ============================================================================
// FillWithSkySeed — uniform sky seed (no ray tracing)
// ============================================================================
// Bypass the CPU BVH bake on WASM where ASYNCIFY + single-threaded JS makes
// the full bake take 20+ minutes. The runtime DDGI trace in Mode 11
// converges from any non-zero seed within ~60 frames, and Mode 10 displays
// this seed as uniform ambient sky light.
void StaticProbeVolume::FillWithSkySeed(const math::v3& sky_color, float ray_max_distance) {
    const u32 pc = ProbeCount();
    if (pc == 0) return;

    const math::v3 sh0 = UniformSkySH0(sky_color);

    // Irradiance + sky_sh: L0 = sky_color * 2*sqrt(pi), L1..L8 = 0
    for (u32 p = 0; p < pc; ++p) {
        irradiance_data_[p * 9 + 0] = sh0;
        for (u32 i = 1; i < 9; ++i) {
            irradiance_data_[p * 9 + i] = math::v3{0.0f};
        }
        sky_sh_data_[p * 9 + 0] = sh0;
        for (u32 i = 1; i < 9; ++i) {
            sky_sh_data_[p * 9 + i] = math::v3{0.0f};
        }
    }

    // Depth: no occlusion → max distance, zero variance
    for (u32 p = 0; p < pc; ++p) {
        for (u32 oct = 0; oct < 64; ++oct) {
            depth_mean_data_[p * 64 + oct] = ray_max_distance;
            depth_var_data_[p * 64 + oct] = 0.0f;
        }
        sky_factor_data_[p] = 1.0f;
    }

    // Global sky SH (9 coeffs) — L0 only
    for (u32 i = 0; i < 9; ++i) {
        sky_sh_[i] = (i == 0) ? sh0 : math::v3{0.0f};
    }

    is_loaded_ = true;
    gpu_uploaded_ = false;
}

} // namespace primal::graphics::lumen
