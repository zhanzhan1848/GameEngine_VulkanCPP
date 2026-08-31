#include "OfflineSDFMerger.h"
#include "CommonHeaders.h"
// P4c: Vulkan 上传路径仅在 ENABLE_VULKAN 下编译(修 OFF 构建未守卫问题)
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#define OFFLINE_SDF_VULKAN_PATH 1
#else
#define OFFLINE_SDF_VULKAN_PATH 0
#endif
#include <fstream>
#include <cstring>
#include "Graphics/RHI/Core/RHICommand.h"
#include <cmath>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

namespace primal::graphics::nanite {

namespace {
// Read a u32 from a byte buffer at the given offset (little-endian).
u32 ReadU32(const u8* data, size_t offset) {
    u32 v;
    std::memcpy(&v, data + offset, 4);
    return v;
}
f32 ReadF32(const u8* data, size_t offset) {
    f32 v;
    std::memcpy(&v, data + offset, 4);
    return v;
}
}

OfflineSDFMerger::~OfflineSDFMerger() {
    if (device_ && global_sdf_texture_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(global_sdf_texture_);
    }
}

u32 OfflineSDFMerger::LoadFromPipelineModel(const char* filePath) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "[OfflineSDFMerger] Failed to open: " << filePath << std::endl;
        return 0;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<u8> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "[OfflineSDFMerger] Failed to read: " << filePath << std::endl;
        return 0;
    }

    const u8* data = buffer.data();
    size_t off = 0;

    // Prefix u32(0)
    u32 prefix = ReadU32(data, off); off += 4;
    if (prefix != 0) {
        // No prefix — rewind (some pipeline models don't have it).
        off = 0;
    }

    // num_materials
    u32 numMat = ReadU32(data, off); off += 4;
    if (numMat > 10000) {
        std::cerr << "[OfflineSDFMerger] Implausible material count: " << numMat << std::endl;
        return 0;
    }
    // Skip materials (3 fields each: name, diffuse, normal — all length-prefixed strings)
    for (u32 mi = 0; mi < numMat; ++mi) {
        for (u32 fi = 0; fi < 3; ++fi) {
            u32 slen = ReadU32(data, off); off += 4;
            if (slen > 10000) { std::cerr << "[OfflineSDFMerger] Bad string len" << std::endl; return 0; }
            off += slen;
        }
    }

    // num_lod_groups
    u32 numLods = ReadU32(data, off); off += 4;
    if (numLods == 0 || numLods > 100) return 0;

    // First LOD group: name + mesh_count
    u32 lodNameLen = ReadU32(data, off); off += 4 + lodNameLen;
    u32 meshCount = ReadU32(data, off); off += 4;
    if (meshCount > 100000) return 0;

    u32 sdfFound = 0;

    for (u32 i = 0; i < meshCount; ++i) {
        if (off + 32 > (size_t)size) {
            std::cerr << "[OfflineSDFMerger] Stop at mesh " << i << ": off=" << off << " size=" << size << std::endl;
            break;
        }

        // Mesh header (Compact format)
        u32 nameLen = ReadU32(data, off); off += 4;
        if (nameLen > 200) {
            std::cerr << "[OfflineSDFMerger] Bad nameLen=" << nameLen << " at mesh " << i << " off=" << off-4 << std::endl;
            break;
        }
        off += nameLen;

        u32 lodId    = ReadU32(data, off); off += 4;
        int32_t  matIdx  = (int32_t)ReadU32(data, off); off += 4;
        u32 elemSize = ReadU32(data, off); off += 4;
        u32 elemType = ReadU32(data, off); off += 4;
        u32 vertCount= ReadU32(data, off); off += 4;
        u32 idxSize  = ReadU32(data, off); off += 4;
        u32 idxCount = ReadU32(data, off); off += 4;
        off += 4; // lodThreshold (f32)

        // Skip position + element + index buffers (no padding in pipeline format)
        off += 12 * vertCount;       // positions
        off += elemSize * vertCount; // elements
        off += idxSize * idxCount;   // indices

        // Skip MSHL section
        if (off + 8 <= (size_t)size) {
            u32 magic = ReadU32(data, off);
            if (magic == 0x4C48534D) { // 'MSHL'
                off += 4;
                u32 mlCount = ReadU32(data, off); off += 4;
                off += mlCount * 60; // meshlet structs (60 bytes each)
                u32 mlvCount = ReadU32(data, off); off += 4 + mlvCount * 4;
                u32 mltCount = ReadU32(data, off); off += 4 + mltCount;
            } else {
                // No MSHL — this shouldn't happen for pipeline models.
                // Skip to next: we can't continue safely.
                break;
            }
        }

        // Check for SDF section (all meshes may have it — LOD1+ too).
        // We skip the SDF data for non-LOD0 meshes but must consume the bytes
        // so the offset points to the next mesh header.
        if (off + 8 <= (size_t)size) {
            u32 sdfMagic = ReadU32(data, off);
            if (sdfMagic == 0x20464453) { // 'SDF '
                off += 4;
                // Read header (needed to compute total SDF size for skipping).
                u32 sres[3];
                sres[0] = ReadU32(data, off); off += 4;
                sres[1] = ReadU32(data, off); off += 4;
                sres[2] = ReadU32(data, off); off += 4;
                f32 sbmin[3], sbmax[3];
                for (int j = 0; j < 3; ++j) { sbmin[j] = ReadF32(data, off); off += 4; }
                for (int j = 0; j < 3; ++j) { sbmax[j] = ReadF32(data, off); off += 4; }

                f32 sdim = std::max({sbmax[0]-sbmin[0], sbmax[1]-sbmin[1], sbmax[2]-sbmin[2]});
                if (sdim < 1e-6f) sdim = 1.0f;

                // Only LOD0 meshes store SDF for visualization.
                if (lodId == 0xFFFFFFFF) {
                    MeshSDF sdf{};
                    sdf.resolution[0] = sres[0]; sdf.resolution[1] = sres[1]; sdf.resolution[2] = sres[2];
                    for (int j = 0; j < 3; ++j) { sdf.bounds_min[j] = sbmin[j]; sdf.bounds_max[j] = sbmax[j]; }
                    sdf.max_dim = sdim;

                    u32 dataCount = ReadU32(data, off); off += 4;
                    if (dataCount > 0 && dataCount < 100000000) {
                        sdf.data.resize(dataCount);
                        std::memcpy(sdf.data.data(), data + off, dataCount * sizeof(uint16_t));
                        off += dataCount * 2;
                    }
                    u32 voxCount = ReadU32(data, off); off += 4;
                    if (voxCount > 0 && voxCount < 100000000) {
                        sdf.voxels.resize(voxCount);
                        std::memcpy(sdf.voxels.data(), data + off, voxCount);
                        off += voxCount;
                    }
                    u32 vfCount = ReadU32(data, off); off += 4;
                    if (vfCount < 100000000) off += vfCount * 2;

                    // Sanity filter: a minority of baked SDF sections in the
                    // shipped pipeline model carry garbage bounds (observed
                    // sdim 365..104460, e.g. ±14933) — their AABBs blanket
                    // the whole scene and poison every merged voxel with a
                    // constant ~900 distance field (all rays instantly miss).
                    // Sponza-scale geometry tops out ~75 units; anything past
                    // 150 or non-finite is a broken bake, not real geometry.
                    // (data/voxels/vf bytes are already consumed above.)
                    const bool boundsFinite =
                        std::isfinite(sbmin[0]) && std::isfinite(sbmin[1]) &&
                        std::isfinite(sbmin[2]) && std::isfinite(sbmax[0]) &&
                        std::isfinite(sbmax[1]) && std::isfinite(sbmax[2]);
                    const bool layoutConsistent =
                        (u64)sdf.resolution[0] * sdf.resolution[1] * sdf.resolution[2] ==
                        (u64)sdf.data.size();
                    if (!boundsFinite || sdim > 150.0f || !layoutConsistent) {
                        std::cerr << "[OfflineSDFMerger] Rejected broken SDF bake"
                                  << " (sdim=" << sdim
                                  << " finite=" << boundsFinite
                                  << " consistent=" << layoutConsistent << ")" << std::endl;
                        continue;
                    }

                    if (!sdf.data.empty()) {
                        // Per-mesh parse diagnostics: decode a few samples to
                        // verify the section layout (bad offset/scale shows up
                        // here as implausible distances immediately).
                        if (mesh_sdfs_.size() < 3 || sdim > 100.0f ||
                            (sdf.resolution[0] * sdf.resolution[1] * sdf.resolution[2]) !=
                                (u32)sdf.data.size()) {
                            u32 vmin = 0xFFFFFFFFu, vmax = 0;
                            for (u32 i = 0; i < sdf.data.size(); i += std::max<u32>(1u, sdf.data.size() / 4096u)) {
                                vmin = std::min(vmin, (u32)sdf.data[i]);
                                vmax = std::max(vmax, (u32)sdf.data[i]);
                            }
                            u32 voxNZ = 0;
                            for (u32 i = 0; i < sdf.voxels.size(); ++i) voxNZ += sdf.voxels[i] ? 1 : 0;
                            std::cerr << "[SDFParse] mesh#" << mesh_sdfs_.size()
                                      << " res=" << sdf.resolution[0] << "x" << sdf.resolution[1]
                                      << "x" << sdf.resolution[2]
                                      << " data=" << sdf.data.size()
                                      << " (res3=" << (u64)sdf.resolution[0] * sdf.resolution[1] * sdf.resolution[2] << ")"
                                      << " vox=" << sdf.voxels.size() << " nz=" << voxNZ
                                      << " sdim=" << sdim
                                      << " bounds=[" << sbmin[0] << "," << sbmin[1] << "," << sbmin[2]
                                      << "]-[" << sbmax[0] << "," << sbmax[1] << "," << sbmax[2] << "]"
                                      << " u16min=" << vmin << " u16max=" << vmax
                                      << " dmax=" << (f32)vmax / 65535.0f * sdim
                                      << std::endl;
                        }
                        mesh_sdfs_.push_back(std::move(sdf));
                        sdfFound++;
                    }
                } else {
                    // Non-LOD0: skip SDF data without storing.
                    u32 dataCount = ReadU32(data, off); off += 4 + dataCount * 2;
                    u32 voxCount = ReadU32(data, off); off += 4 + voxCount;
                    u32 vfCount = ReadU32(data, off); off += 4;
                    if (vfCount < 100000000) off += vfCount * 2;
                }
            }
        }
        continue;
    } // end mesh loop

    // Compute global bounds
    if (mesh_sdfs_.empty()) {
        std::cerr << "[OfflineSDFMerger] No SDF data found in " << filePath << std::endl;
        return 0;
    }

    // Compute global bounds — filter out extreme outlier meshes (e.g. physics
    // cloth meshes with distant vertices). Sponza fits within ~±100 units.
    constexpr f32 BOUND_LIMIT = 50.0f;
    origin_ = math::v3{BOUND_LIMIT, BOUND_LIMIT, BOUND_LIMIT};
    math::v3 maxPt{-BOUND_LIMIT, -BOUND_LIMIT, -BOUND_LIMIT};
    for (const auto& sdf : mesh_sdfs_) {
        // Skip meshes with implausible bounds
        bool plausible = true;
        for (int j = 0; j < 3; ++j) {
            if (sdf.bounds_min[j] < -BOUND_LIMIT || sdf.bounds_max[j] > BOUND_LIMIT ||
                sdf.bounds_max[j] - sdf.bounds_min[j] > BOUND_LIMIT) {
                plausible = false;
                break;
            }
        }
        if (!plausible) continue;
        for (int j = 0; j < 3; ++j) {
            origin_[j] = std::min(origin_[j], sdf.bounds_min[j]);
            maxPt[j]   = std::max(maxPt[j],   sdf.bounds_max[j]);
        }
    }
    // Add small margin
    math::v3 margin = (maxPt - origin_) * 0.02f;
    origin_ -= margin;
    maxPt += margin;
    extent_ = maxPt - origin_;
    voxel_size_ = std::max({extent_[0], extent_[1], extent_[2]}) / (f32)GLOBAL_RES;

    std::cerr << "[OfflineSDFMerger] Loaded " << sdfFound << " mesh SDFs"
              << ", global bounds: (" << origin_[0] << "," << origin_[1] << "," << origin_[2]
              << ") extent=(" << extent_[0] << "," << extent_[1] << "," << extent_[2] << ")"
              << " voxel_size=" << voxel_size_ << std::endl;

    return sdfFound;
}

f32 OfflineSDFMerger::SampleMeshSDF(const MeshSDF& sdf, f32 lx, f32 ly, f32 lz) const {
    // Convert local-space position to grid coordinates.
    f32 stepX = (sdf.bounds_max[0] - sdf.bounds_min[0]) / (f32)sdf.resolution[0];
    f32 stepY = (sdf.bounds_max[1] - sdf.bounds_min[1]) / (f32)sdf.resolution[1];
    f32 stepZ = (sdf.bounds_max[2] - sdf.bounds_min[2]) / (f32)sdf.resolution[2];

    f32 fx = (lx - sdf.bounds_min[0]) / stepX - 0.5f;
    f32 fy = (ly - sdf.bounds_min[1]) / stepY - 0.5f;
    f32 fz = (lz - sdf.bounds_min[2]) / stepZ - 0.5f;

    // T8.10b: OUT-OF-RANGE GUARD — a voxel center outside this mesh's grid
    // [0,res) must be treated as unoccupied positive distance, never clamped.
    // The old clamp made occupied() hit the border texel and sampleData() its
    // 1e10 sentinel, so min() over meshes wrote -1e10 into EMPTY-AIR voxels
    // wherever a mesh's AABB marginally covered them (poisoned air = instant
    // inside-hits for every SDF tracer).
    if (!(fx >= 0.0f && fx <= (f32)sdf.resolution[0] - 1.0f &&
          fy >= 0.0f && fy <= (f32)sdf.resolution[1] - 1.0f &&
          fz >= 0.0f && fz <= (f32)sdf.resolution[2] - 1.0f)) {
        return 1.0e10f;   // outside grid: positive far distance, NOT occupiable
    }

    int32_t x0 = (int32_t)std::floor(fx), x1 = x0 + 1;
    int32_t y0 = (int32_t)std::floor(fy), y1 = y0 + 1;
    int32_t z0 = (int32_t)std::floor(fz), z1 = z0 + 1;

    f32 tx = fx - (f32)x0;
    f32 ty = fy - (f32)y0;
    f32 tz = fz - (f32)z0;

    auto clampCoord = [](int32_t v, u32 res) -> int32_t { return std::max(0, std::min((int32_t)res - 1, v)); };
    x0 = clampCoord(x0, sdf.resolution[0]); x1 = clampCoord(x1, sdf.resolution[0]);
    y0 = clampCoord(y0, sdf.resolution[1]); y1 = clampCoord(y1, sdf.resolution[1]);
    z0 = clampCoord(z0, sdf.resolution[2]); z1 = clampCoord(z1, sdf.resolution[2]);

    // Sign via the bake's occupancy voxels (design documented in the header,
    // never wired before): cells the bake marked inside/near-surface return a
    // NEGATIVE distance, so the merged global field is properly signed and
    // sphere tracing cannot step over thin/grazing surfaces — unsigned fields
    // let relaxed marches pass straight through walls into the environment.
    auto occupied = [&](int32_t x, int32_t y, int32_t z) -> bool {
        if (sdf.voxels.empty()) return false;
        const u32 idx = (u32)z * sdf.resolution[0] * sdf.resolution[1]
                      + (u32)y * sdf.resolution[0]
                      + (u32)x;
        return idx < sdf.voxels.size() && sdf.voxels[idx] != 0;
    };
    const bool inside = occupied(
        clampCoord((int32_t)std::lround(fx), sdf.resolution[0]),
        clampCoord((int32_t)std::lround(fy), sdf.resolution[1]),
        clampCoord((int32_t)std::lround(fz), sdf.resolution[2]));

    auto sampleData = [&](int32_t x, int32_t y, int32_t z, f32 wx, f32 wy, f32 wz) -> f32 {
        u32 idx = (u32)z * sdf.resolution[0] * sdf.resolution[1]
                + (u32)y * sdf.resolution[0]
                + (u32)x;
        if (idx >= sdf.data.size()) return 1e10f;
        // Decode unsigned distance: dist = u16 / 65535 * max_dim
        return (f32)sdf.data[idx] / 65535.0f * sdf.max_dim;
    };

    // Trilinear interpolation
    f32 c000 = sampleData(x0, y0, z0, lx, ly, lz), c100 = sampleData(x1, y0, z0, lx, ly, lz);
    f32 c010 = sampleData(x0, y1, z0, lx, ly, lz), c110 = sampleData(x1, y1, z0, lx, ly, lz);
    f32 c001 = sampleData(x0, y0, z1, lx, ly, lz), c101 = sampleData(x1, y0, z1, lx, ly, lz);
    f32 c011 = sampleData(x0, y1, z1, lx, ly, lz), c111 = sampleData(x1, y1, z1, lx, ly, lz);

    f32 c00 = c000 * (1 - tx) + c100 * tx;
    f32 c01 = c001 * (1 - tx) + c101 * tx;
    f32 c10 = c010 * (1 - tx) + c110 * tx;
    f32 c11 = c011 * (1 - tx) + c111 * tx;

    f32 c0 = c00 * (1 - ty) + c10 * ty;
    f32 c1 = c01 * (1 - ty) + c11 * ty;

    f32 d = c0 * (1 - tz) + c1 * tz;
    // T8.10b FINAL SEMANTICS: UNSIGNED distance only. The occupancy-signed
    // variant (inside → −d) filled hollow architecture: Sponza's atrium cavity
    // was marked solid, so every interior probe ray insta-hit at d≈0 and the
    // whole inner scene traced as one black block (measured: nave-air probe
    // samples returned −2.7/−0.38/−3.5 m). Unsigned fields are what relaxed
    // sphere-tracers (DDGITraceSDF / ScreenProbeTraceSDF) are designed for;
    // the cross-nave transport now lives in SC-cached radiance on hit.
    return d;
}

bool OfflineSDFMerger::BuildAndUpload(rhi::RHIDeviceBase* device, const char* cachePath) {
    device_ = device;
    if (mesh_sdfs_.empty() || !device_) return false;

    const u32 res = GLOBAL_RES;
    const u64 totalVoxels = (u64)res * res * res;

    // Allocate CPU-side half-float buffer.
    // R16F: we store the signed distance as half-float.
    // We'll compute as f32 then convert to f16.
    std::vector<u16> globalData(totalVoxels, 0x3C00); // init to 1.0 in half-float

    f32 stepX = extent_[0] / (f32)res;
    f32 stepY = extent_[1] / (f32)res;
    f32 stepZ = extent_[2] / (f32)res;

    // Convert f32 to f16 (IEEE 754 half).
    auto f32toF16 = [](f32 f) -> u16 {
        u32 bits;
        std::memcpy(&bits, &f, 4);
        u32 sign = (bits >> 16) & 0x8000;
        int32_t exp = (int32_t)((bits >> 23) & 0xFF) - 127 + 15;
        u32 mant = bits & 0x7FFFFF;
        if (exp <= 0) {
            if (exp < -10) return sign;
            mant |= 0x800000;
            u32 sh = (u32)(14 - exp);
            u32 rounded = mant >> sh;
            if ((mant >> (sh - 1)) & 1) rounded++;
            return sign | (u16)rounded;
        } else if (exp == 0xFF - (127 - 15)) {
            return sign | 0x7C00 | (mant ? 0x200 : 0);
        }
        if (mant & 0x1000) {
            mant += 0x2000;
            if (mant & 0x800000) { mant = 0; exp++; }
        }
        if (exp >= 0x1F) return sign | 0x7C00;
        return (u16)(sign | (exp << 10) | (mant >> 13));
    };

    std::cerr << "[OfflineSDFMerger] Merging " << mesh_sdfs_.size()
              << " mesh SDFs into " << res << "³ global volume (per-mesh AABB, threaded)..."
              << std::endl;

    // Per-mesh AABB sweep instead of per-voxel × all-meshes: empty space (most
    // of the volume) is never touched by any mesh, and z slices are disjoint
    // so workers need no locking. Semantics match the old loop exactly —
    // min over meshes containing the voxel center (±0.01 margin), empty
    // voxels get voxel_size*4.
    {
        std::atomic<u32> nextSlice{0};
        std::atomic<u32> doneSlices{0};
        const f32 emptyDist = voxel_size_ * 4.0f;

        auto worker = [&]() {
            std::vector<f32> best((size_t)res * res, 1e10f);
            for (;;) {
                const u32 gz = nextSlice.fetch_add(1);
                if (gz >= res) break;
                const f32 wz = origin_[2] + ((f32)gz + 0.5f) * stepZ;
                std::fill(best.begin(), best.end(), 1e10f);

                for (const auto& sdf : mesh_sdfs_) {
                    if (wz < sdf.bounds_min[2] - 0.01f || wz > sdf.bounds_max[2] + 0.01f)
                        continue;
                    const f32 wy0 = (sdf.bounds_min[1] - 0.01f - origin_[1]) / stepY - 0.5f;
                    const f32 wy1 = (sdf.bounds_max[1] + 0.01f - origin_[1]) / stepY - 0.5f;
                    const f32 wx0 = (sdf.bounds_min[0] - 0.01f - origin_[0]) / stepX - 0.5f;
                    const f32 wx1 = (sdf.bounds_max[0] + 0.01f - origin_[0]) / stepX - 0.5f;
                    const int32_t gy0 = std::max(0, (int32_t)std::floor(wy0));
                    const int32_t gy1 = std::min((int32_t)res - 1, (int32_t)std::ceil(wy1));
                    const int32_t gx0 = std::max(0, (int32_t)std::floor(wx0));
                    const int32_t gx1 = std::min((int32_t)res - 1, (int32_t)std::ceil(wx1));

                    for (int32_t gy = gy0; gy <= gy1; ++gy) {
                        const f32 wy = origin_[1] + ((f32)gy + 0.5f) * stepY;
                        for (int32_t gx = gx0; gx <= gx1; ++gx) {
                            const f32 wx = origin_[0] + ((f32)gx + 0.5f) * stepX;
                            const f32 d = SampleMeshSDF(sdf, wx, wy, wz);
                            // T8.10b: reject sentinel/garbage candidates —
                            // anything beyond ±1e4 (the 1e10 out-of-grid
                            // sentinel or a NaN/inf reinterpretation) must
                            // never enter the min() reduction.
                            if (!(d >= -1.0e4f && d <= 1.0e4f)) continue;
                            f32& b = best[(size_t)gy * res + gx];
                            if (d < b) b = d;
                        }
                    }
                }

                const u64 zBase = (u64)gz * res * res;
                for (u32 gy = 0; gy < res; ++gy) {
                    for (u32 gx = 0; gx < res; ++gx) {
                        f32 bd = best[(size_t)gy * res + gx];
                        if (bd > 1e9f) bd = emptyDist;
                        globalData[zBase + (u64)gy * res + gx] = f32toF16(bd);
                    }
                }
                const u32 done = doneSlices.fetch_add(1) + 1;
                if (done % 96 == 0) {
                    std::cerr << "  Merged slice " << done << "/" << res << std::endl;
                }
            }
        };

        u32 hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        hw = std::min(hw, 12u);
        std::vector<std::thread> pool;
        for (u32 t = 1; t < hw; ++t) pool.emplace_back(worker);
        worker();
        for (auto& t : pool) t.join();
    }
    std::cerr << "[OfflineSDFMerger] Merge complete. Uploading to GPU..." << std::endl;

    // T8.10d: SEAM SEAL — the merge is a union of per-mesh boxes; adjacent
    // wall segments leave ≤1-voxel cracks where the field reads emptyDist.
    // Tracer rays slip through the cracks and register sky (F6 horizontal
    // gray stripes; probes lose wall-hit shading). One 6-neighbor closing
    // pass: any voxel with a surface-band texel (d < 2×voxel) in its
    // 6-neighborhood is clamped to 2×voxel. In-place scan; the directional
    // bias of in-place closing is harmless at this scale.
    // DISABLED BY DEFAULT — T8.10d experiment verdict: sealing paints a
    // 1-voxel skin over EVERY surface (38.8M voxels = 69% of the volume, the
    // crack set and the surface skin are indistinguishable at voxel
    // granularity), flattening the field to ≈sealDist and collapsing
    // L_2F 39.2→19.3. Set OFFLINE_SDF_SEAL=1 to reproduce. The real fix is
    // asset-side: ContentTools must bake segment-gap-free SDFs (or emit
    // combined watertight geometry).
    if (std::getenv("OFFLINE_SDF_SEAL")) {
        const f32 sealDist = voxel_size_ * 1.0f;
        u64 sealed = 0;
        auto at = [&](int32_t x, int32_t y, int32_t z) -> f32 {
            if (x < 0 || y < 0 || z < 0 || x >= (int32_t)res || y >= (int32_t)res || z >= (int32_t)res)
                return 1e30f;
            const u16 h = globalData[(u64)z * res * res + (u64)y * res + x];
            u32 sign = (h >> 15) & 1, ex = (h >> 10) & 0x1F, m = h & 0x3FF;
            if (ex == 0) return sign ? -0.f : 0.f;
            u32 bits = (sign << 31) | ((ex + 112) << 23) | (m << 13);
            f32 f; std::memcpy(&f, &bits, 4); return f;
        };
        for (u32 z = 0; z < res; ++z)
        for (u32 y = 0; y < res; ++y)
        for (u32 x = 0; x < res; ++x) {
            const u64 i = (u64)z * res * res + (u64)y * res + x;
            f32 d = at((int32_t)x, (int32_t)y, (int32_t)z);
            if (d <= sealDist) continue;
            const f32 n = std::min(std::min(at((int32_t)x-1,(int32_t)y,(int32_t)z), at((int32_t)x+1,(int32_t)y,(int32_t)z)),
                                   std::min(at((int32_t)x,(int32_t)y-1,(int32_t)z), at((int32_t)x,(int32_t)y+1,(int32_t)z)));
            const f32 f = at((int32_t)x, (int32_t)y, (int32_t)z-1), bk = at((int32_t)x, (int32_t)y, (int32_t)z+1);
            if (std::min(std::min(n, f), bk) <= sealDist) {
                globalData[i] = f32toF16(sealDist);
                ++sealed;
            }
        }
        std::cerr << "[OfflineSDFMerger] Seam seal: " << sealed << " voxels clamped to "
                  << sealDist << std::endl;
    }

    // Dump SDF statistics to verify data has both positive and negative values.
    {
        int negCount = 0, posCount = 0, zeroCount = 0;
        f32 minVal = 1e10f, maxVal = -1e10f;
        for (u64 i = 0; i < totalVoxels; ++i) {
            // Decode half-float back to float for stats
            u16 h = globalData[i];
            // Simple f16→f32 decode
            u32 sign = (h >> 15) & 1;
            u32 exp = (h >> 10) & 0x1F;
            u32 mant = h & 0x3FF;
            f32 f;
            if (exp == 0 && mant == 0) { f = sign ? -0.0f : 0.0f; }
            else if (exp == 0) { f = 0.0f; }
            else {
                u32 bits = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
                std::memcpy(&f, &bits, 4);
            }
            if (f < -0.001) negCount++;
            else if (f > 0.001) posCount++;
            else zeroCount++;
            minVal = std::min(minVal, f);
            maxVal = std::max(maxVal, f);
        }
        std::cerr << "[OfflineSDFMerger] SDF stats: neg=" << negCount
                  << " pos=" << posCount << " zero=" << zeroCount
                  << " min=" << minVal << " max=" << maxVal << std::endl;
    }

    // Optional disk cache — the merge is a multi-minute one-shot for a static
    // scene; later runs LoadFromCache instead of re-parsing the pipeline model.
    if (cachePath) {
        struct CacheHeader {
            char magic[8];   // "PGSDFC01"
            u32  res;
            f32  origin[3];
            f32  extent[3];
            f32  voxel;
            u64  texelCount;
        };
        CacheHeader hdr{};
        std::memcpy(hdr.magic, "PGSDFC01", 8);
        hdr.res = res;
        hdr.origin[0] = origin_[0]; hdr.origin[1] = origin_[1]; hdr.origin[2] = origin_[2];
        hdr.extent[0] = extent_[0]; hdr.extent[1] = extent_[1]; hdr.extent[2] = extent_[2];
        hdr.voxel = voxel_size_;
        hdr.texelCount = totalVoxels;
        std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
            out.write(reinterpret_cast<const char*>(globalData.data()),
                      (std::streamsize)(totalVoxels * sizeof(u16)));
            out.close();
            std::cerr << "[OfflineSDFMerger] Cache written: " << cachePath << " ("
                      << (totalVoxels * sizeof(u16)) / (1024 * 1024) << " MB)" << std::endl;
        } else {
            std::cerr << "[OfflineSDFMerger] WARNING: cache write failed: " << cachePath << std::endl;
        }
    }

    return UploadGlobal(device_, globalData.data());
}

bool OfflineSDFMerger::LoadFromCache(rhi::RHIDeviceBase* device, const char* cachePath) {
    device_ = device;
    if (!device_ || !cachePath) return false;

    struct CacheHeader {
        char magic[8];
        u32  res;
        f32  origin[3];
        f32  extent[3];
        f32  voxel;
        u64  texelCount;
    };
    std::ifstream in(cachePath, std::ios::binary);
    if (!in) return false;

    CacheHeader hdr{};
    in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!in || std::memcmp(hdr.magic, "PGSDFC01", 8) != 0 ||
        hdr.res != GLOBAL_RES || hdr.texelCount != (u64)GLOBAL_RES * GLOBAL_RES * GLOBAL_RES) {
        std::cerr << "[OfflineSDFMerger] Cache stale/invalid: " << cachePath << std::endl;
        return false;
    }

    std::vector<u16> globalData(hdr.texelCount);
    in.read(reinterpret_cast<char*>(globalData.data()),
            (std::streamsize)(hdr.texelCount * sizeof(u16)));
    if (!in) {
        std::cerr << "[OfflineSDFMerger] Cache truncated: " << cachePath << std::endl;
        return false;
    }

    origin_[0] = hdr.origin[0]; origin_[1] = hdr.origin[1]; origin_[2] = hdr.origin[2];
    extent_[0] = hdr.extent[0]; extent_[1] = hdr.extent[1]; extent_[2] = hdr.extent[2];
    voxel_size_ = hdr.voxel;

    std::cerr << "[OfflineSDFMerger] Loaded cache: " << cachePath << " ("
              << (hdr.texelCount * sizeof(u16)) / (1024 * 1024) << " MB)" << std::endl;
    return UploadGlobal(device_, globalData.data());
}

bool OfflineSDFMerger::UploadGlobal(rhi::RHIDeviceBase* device, const u16* globalData) {
    const u32 res = GLOBAL_RES;
    const u64 totalVoxels = (u64)res * res * res;

    // Create Texture3D and upload.
    rhi::TextureDesc desc{};
    desc.size = {res, res, res};
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = rhi::DataFormat::R16_Float;
    desc.type = rhi::TextureType::Texture3D;
    desc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopyDest;
    desc.memoryUsage = rhi::GPUMemoryUsage::Static;
    desc.name = "OfflineSDFMerger_GlobalSDF";

    global_sdf_texture_ = device_->CreateTexture(desc);
    if (global_sdf_texture_ == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[OfflineSDFMerger] Failed to create Texture3D" << std::endl;
        return false;
    }

    // Upload via staging buffer (mirrors RHIGpuMesh::CreateAndUploadTexture3D).
    u32 bytesPerTexel = 2; // R16F
    u32 srcRowPitch = res * bytesPerTexel;
    u32 alignedRowPitch = (srcRowPitch + 255) & ~255;
    u64 slicePitch = (u64)alignedRowPitch * res;
    u64 totalSize = slicePitch * res;

    rhi::BufferDesc stagingDesc{};
    stagingDesc.size = totalSize;
    stagingDesc.bindFlags = (u32)rhi::BufferUsageFlags::TransferSrc;
    stagingDesc.memoryUsage = rhi::GPUMemoryUsage::Staging;
    stagingDesc.usage = rhi::GPUMemoryUsage::Staging;
    stagingDesc.name = "OfflineSDF_Staging";

    rhi::ResourceHandle staging = device_->CreateBuffer(stagingDesc);
    if (staging == rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[OfflineSDFMerger] Failed to create staging buffer" << std::endl;
        return false;
    }

    void* mapped = device_->MapBuffer(staging, 0, totalSize);
    if (!mapped) {
        device_->DestroyBuffer(staging);
        return false;
    }

    // Copy row-by-row with alignment padding.
    u8* dst = static_cast<u8*>(mapped);
    for (u32 z = 0; z < res; ++z) {
        for (u32 y = 0; y < res; ++y) {
            const u16* srcRow = &globalData[(u64)z * res * res + (u64)y * res];
            std::memcpy(dst, srcRow, srcRowPitch);
            dst += alignedRowPitch;
        }
    }
    device_->UnmapBuffer(staging);

    // Copy staging → texture (mirrors RHIGpuMesh::CreateAndUploadTexture3D).
    // T8.13 Vulkan↔Metal sync: use the platform-generic RHI command buffer —
    // the old Vulkan-only cast left the Metal texture UNINITIALIZED (the
    // "Upload complete" log printed unconditionally), so the Metal DDGI/SPGI
    // probes traced garbage instead of the merged field.
    rhi::CommandBufferHandle cmdHandle = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
    rhi::RHICommandBuffer* cmd = rhi::GetCommandBuffer(cmdHandle);
    if (!cmd) {
        std::cerr << "[OfflineSDFMerger] Unsupported device for texture upload" << std::endl;
        device_->DestroyCommandBuffer(cmdHandle);
        device_->DestroyBuffer(staging);
        return false;
    }
    cmd->Begin();

    rhi::BufferTextureCopyRegion region{};
    region.bufferOffset = 0;
    region.bufferRowLength = alignedRowPitch / bytesPerTexel;
    region.bufferImageHeight = res;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {res, res, res};

    cmd->CopyBufferToTexture(staging, global_sdf_texture_, &region, 1);
    cmd->End();
    cmd->Submit();
    cmd->WaitForCompletion();
    device_->DestroyCommandBuffer(cmdHandle);
    device_->DestroyBuffer(staging);

    std::cerr << "[OfflineSDFMerger] Upload complete (" << totalSize / (1024*1024) << " MB)" << std::endl;

    // T8.10b self-check (OFFLINE_SDF_VERIFY=1): read the texture back and
    // bit-compare against the CPU-side source. A clean pass proves the upload
    // path; any row/slice drift or format reinterpretation shows up as
    // mismatches with coordinates that fingerprint the bug class.
    if (std::getenv("OFFLINE_SDF_VERIFY")) {
        rhi::BufferDesc vDesc{};
        vDesc.size = totalSize;
        vDesc.bindFlags = (u32)rhi::BufferUsageFlags::TransferDst;
        vDesc.memoryUsage = rhi::GPUMemoryUsage::Readback;
        vDesc.usage = rhi::GPUMemoryUsage::Readback;
        vDesc.name = "OfflineSDF_VerifyStaging";
        rhi::ResourceHandle vStage = device_->CreateBuffer(vDesc);
        bool ok = vStage != rhi::handles::INVALID_RESOURCE;
        if (ok) {
            auto vcHandle = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
            ok = vcHandle != rhi::handles::INVALID_COMMAND_BUFFER;
            if (ok) {
#if OFFLINE_SDF_VULKAN_PATH
                auto* vc = dynamic_cast<rhi::VulkanDevice*>(device_)
                               ? static_cast<rhi::VulkanDevice*>(device_)->GetCommandBuffer(vcHandle)
                               : nullptr;
                if (!vc) { ok = false; }
                else {
                    rhi::BufferTextureCopyRegion vr{};
                    vr.bufferOffset = 0;
                    vr.bufferRowLength = alignedRowPitch / bytesPerTexel;
                    vr.bufferImageHeight = res;
                    vr.imageSubresource.mipLevel = 0;
                    vr.imageSubresource.baseArrayLayer = 0;
                    vr.imageSubresource.layerCount = 1;
                    vr.imageOffset = {0, 0, 0};
                    vr.imageExtent = {res, res, res};
                    vc->Begin();
                    rhi::ResourceBarrier toRd{};
                    toRd.resource = global_sdf_texture_;
                    toRd.beforeState = rhi::ResourceState::ShaderResource;
                    toRd.afterState = rhi::ResourceState::CopySource;
                    toRd.subresource = 0xFFFFFFFF;
                    vc->InsertBarrier(&toRd, 1);
                    vc->CopyTextureToBuffer(global_sdf_texture_, vStage, &vr, 1);
                    rhi::ResourceBarrier back{};
                    back.resource = global_sdf_texture_;
                    back.beforeState = rhi::ResourceState::CopySource;
                    back.afterState = rhi::ResourceState::ShaderResource;
                    back.subresource = 0xFFFFFFFF;
                    vc->InsertBarrier(&back, 1);
                    vc->End();
                    vc->Submit();
                    vc->WaitForCompletion();
                }
#endif
            }
            if (ok && std::getenv("OFFLINE_SDF_VERIFY")) {
                if (void* vm = device_->MapBuffer(vStage, 0, totalSize)) {
                    const u16* gpu = static_cast<const u16*>(vm);
                    u64 mismatch = 0; f32 maxAbs = 0.f;
                    int shown = 0;
                    for (u32 gz = 0; gz < res && shown < 8; ++gz)
                    for (u32 gy = 0; gy < res && shown < 8; ++gy)
                    for (u32 gx = 0; gx < res; ++gx) {
                        const u64 li = (u64)gz * res * res + (u64)gy * res + gx;
                        const u64 gi = (u64)gz * slicePitch / sizeof(u16)
                                     + (u64)gy * alignedRowPitch / sizeof(u16) + gx;
                        u16 c = globalData[li], g = gpu[gi];
                        if (c != g) {
                            ++mismatch;
                            if (shown < 8) {
                                auto dec = [](u16 h) -> f32 {
                                    u32 sign = (h >> 15) & 1, ex = (h >> 10) & 0x1F, m = h & 0x3FF;
                                    if (ex == 0) return sign ? -0.f : 0.f;
                                    u32 b = (sign << 31) | ((ex + 112) << 23) | (m << 13);
                                    f32 f; std::memcpy(&f, &b, 4); return f;
                                };
                                std::cerr << "[OfflineSDFVerify] first diffs (x,y,z cpu gpu): "
                                          << gx << "," << gy << "," << gz << " "
                                          << dec(c) << " vs " << dec(g) << "\n";
                                ++shown;
                            }
                        }
                        if (li == totalVoxels - 1 || gi >= totalVoxels) break;
                    }
                    // Full-count pass without early-exit cap.
                    if (shown == 8) {
                        mismatch = 0;
                        for (u64 li = 0; li < totalVoxels; ++li) {
                            u32 gz = u32(li / ((u64)res * res));
                            u32 rem = u32(li % ((u64)res * res));
                            u32 gy = rem / res, gx = rem % res;
                            u64 gi = (u64)gz * slicePitch / sizeof(u16)
                                   + (u64)gy * alignedRowPitch / sizeof(u16) + gx;
                            if (globalData[li] != gpu[gi]) ++mismatch;
                        }
                    }
                    std::cerr << "[OfflineSDFVerify] texels=" << totalVoxels
                              << " mismatched=" << mismatch
                              << " (" << (100.0 * double(mismatch) / double(totalVoxels)) << "%)"
                              << (mismatch ? "  => UPLOAD PATH CORRUPT" : "  => upload bit-exact")
                              << std::endl;
                    device_->UnmapBuffer(vStage);
                } else ok = false;
            } else if (!ok) {
                std::cerr << "[OfflineSDFVerify] readback setup failed" << std::endl;
            }
            device_->DestroyBuffer(vStage);
        }
    }
    return true;
}

} // namespace primal::graphics::nanite
