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
#include <sstream>
#include <cstdlib>
#include <unordered_map>
#include "Engine/Common/CoreLogStream.h"

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

// ============================================================================
static void PropagateMeshSDF(OfflineSDFMerger::MeshSDF& sdf) {
    const int res[3] = { (int)sdf.resolution[0], (int)sdf.resolution[1],
                         (int)sdf.resolution[2] };
    const u64 n = (u64)res[0] * res[1] * res[2];
    const f32 sdim = sdf.max_dim;
    const f32 sdim2 = 2.0f * sdim;
    const f32 step[3] = { (sdf.bounds_max[0] - sdf.bounds_min[0]) / res[0],
                          (sdf.bounds_max[1] - sdf.bounds_min[1]) / res[1],
                          (sdf.bounds_max[2] - sdf.bounds_min[2]) / res[2] };

    std::vector<f32> sd(n);
    for (u64 i = 0; i < n; ++i)
        sd[i] = (f32)sdf.data[i] / 65535.0f * sdim2 - sdim;

    auto encode = [&](u64 i) {
        f32 v = std::clamp(sd[i], -sdim, sdim);
        sdf.data[i] = (u16)std::clamp(
            (int)((v + sdim) / sdim2 * 65535.0f + 0.5f), 0, 65535);
    };

    for (int iter = 0; iter < 2; ++iter)
    for (int pass = 0; pass < 6; ++pass) {
        const int axis = pass / 2;
        const bool forward = (pass % 2) == 0;
        const int dir = forward ? 1 : -1;
        for (int pz = 0; pz < res[2]; ++pz)
        for (int py = 0; py < res[1]; ++py)
        for (int px = 0; px < res[0]; ++px) {
            int x = px, y = py, z = pz;
            if (!forward) {
                if (axis == 0) x = res[0] - 1 - px;
                if (axis == 1) y = res[1] - 1 - py;
                if (axis == 2) z = res[2] - 1 - pz;
            }
            // neighbor = 1 voxel behind the sweep direction on `axis`
            int nx = x, ny = y, nz = z;
            if (axis == 0) nx -= dir;
            else if (axis == 1) ny -= dir;
            else nz -= dir;
            const f32 stp = step[axis];
            if (nx < 0 || nx >= res[0] || ny < 0 || ny >= res[1] || nz < 0 || nz >= res[2])
                continue;
            const u64 idx = (u64)z * res[0] * res[1] + (u64)y * res[0] + x;
            const u64 nidx = (u64)nz * res[0] * res[1] + (u64)ny * res[0] + nx;
            const f32 cand = std::fabs(sd[nidx]) + stp;
            const f32 cur = std::fabs(sd[idx]);
            if (cand < cur) {
                sd[idx] = sd[idx] < 0.0f ? -cand : cand;
                encode(idx);
            }
        }
    }
}

u32 OfflineSDFMerger::LoadFromPipelineModel(const char* filePath, bool signed_encoding) {
    signed_encoding_ = signed_encoding;
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Failed to open: " << filePath << std::endl;
        return 0;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<u8> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Failed to read: " << filePath << std::endl;
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
        PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Implausible material count: " << numMat << std::endl;
        return 0;
    }
    // Skip materials (3 fields each: name, diffuse, normal — all length-prefixed strings)
    for (u32 mi = 0; mi < numMat; ++mi) {
        for (u32 fi = 0; fi < 3; ++fi) {
            u32 slen = ReadU32(data, off); off += 4;
            if (slen > 10000) { PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Bad string len" << std::endl; return 0; }
            off += slen;
        }
    }

    // num_lod_groups
    u32 numLods = ReadU32(data, off); off += 4;
    // 2026-09 signed_v4 bake: one group per submesh cluster (381 groups for
    // Sponza). The old 100-group cap rejected the new layout outright.
    if (numLods == 0 || numLods > 100000) return 0;

    u32 sdfFound = 0;

    // Group headers and their meshes interleave: header, meshCount meshes,
    // next header, ... Walk them in lockstep (a pre-scan of headers would
    // consume bytes the flat mesh loop then misparses).
    for (u32 g = 0; g < numLods; ++g) {
        if (off + 8 > (size_t)size) break;
        u32 lodNameLen = ReadU32(data, off); off += 4 + lodNameLen;
        u32 meshCount = ReadU32(data, off); off += 4;
        if (meshCount > 100000) break;

        for (u32 i = 0; i < meshCount; ++i) {
        if (off + 32 > (size_t)size) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Stop at mesh " << i << ": off=" << off << " size=" << size << std::endl;
            break;
        }

        // Mesh header (Compact format)
        u32 nameLen = ReadU32(data, off); off += 4;
        if (nameLen > 200) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Bad nameLen=" << nameLen << " at mesh " << i << " off=" << off-4 << std::endl;
            break;
        }
        const std::string sourceName(reinterpret_cast<const char*>(data + off), nameLen);
        off += nameLen;

        u32 lodId    = ReadU32(data, off); off += 4;
        int32_t  matIdx  = (int32_t)ReadU32(data, off); off += 4;
        u32 elemSize = ReadU32(data, off); off += 4;
        u32 elemType = ReadU32(data, off); off += 4;
        u32 vertCount= ReadU32(data, off); off += 4;
        u32 idxSize  = ReadU32(data, off); off += 4;
        u32 idxCount = ReadU32(data, off); off += 4;
        off += 4; // lodThreshold (f32)

        const size_t positionOffset = off;
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

                // Only LOD0 meshes store SDF for the merge. LOD0 is
                // 0xFFFFFFFF in the legacy single-group layout and 0 in the
                // 2026-09 multi-group (signed_v4) layout; LOD1+ use 1..N in
                // both.
                if (lodId == 0xFFFFFFFF || lodId == 0) {
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
                        PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Rejected broken SDF bake"
                                  << " (sdim=" << sdim
                                  << " finite=" << boundsFinite
                                  << " consistent=" << layoutConsistent << ")" << std::endl;
                        continue;
                    }

                    if (geometry_decode_) {
                        // Baker encodes with unpadded geometry span. Check
                        // this mesh's padding, not a global 1/1.4 guess.
                        if (!signed_encoding_ || !vertCount || positionOffset + u64(vertCount)*12u > buffer.size()) return 0;
                        float geometryMin[3]={1e30f,1e30f,1e30f}, geometryMax[3]={-1e30f,-1e30f,-1e30f};
                        u32 finiteVertices=0;
                        for (u32 vi=0;vi<vertCount;++vi) {
                            float p[3];
                            std::memcpy(p,data+positionOffset+size_t(vi)*12u,12);
                            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
                            ++finiteVertices;
                            for (int k=0;k<3;++k) { geometryMin[k]=std::min(geometryMin[k],p[k]); geometryMax[k]=std::max(geometryMax[k],p[k]); }
                        }
                        if (!finiteVertices) return 0;
                        float encodingSpan=std::max({geometryMax[0]-geometryMin[0],geometryMax[1]-geometryMin[1],geometryMax[2]-geometryMin[2]});
                        if (!std::isfinite(encodingSpan) || encodingSpan<.0001f) encodingSpan=1.f;
                        for (int k=0;k<3;++k) {
                            if (std::abs(geometryMin[k]-encodingSpan*.2f-sbmin[k])>1e-5f ||
                                std::abs(geometryMax[k]+encodingSpan*.2f-sbmax[k])>1e-5f) {
                                PRIMAL_LOG_STREAM(Error,"Render") << "[SDFGeometryDecode] padding contract mismatch: " << sourceName << std::endl;
                                return 0;
                            }
                        }
                        sdf.max_dim=encodingSpan; // Before propagation/decode.
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
                            PRIMAL_LOG_STREAM(Error, "Render") << "[SDFParse] mesh#" << mesh_sdfs_.size()
                                      << " res=" << sdf.resolution[0] << "x" << sdf.resolution[1]
                                      << "x" << sdf.resolution[2]
                                      << " data=" << sdf.data.size()
                                      << " (res3=" << (u64)sdf.resolution[0] * sdf.resolution[1] * sdf.resolution[2] << ")"
                                      << " vox=" << sdf.voxels.size() << " nz=" << voxNZ
                                      << " signed=" << signed_encoding_
                                      << " sdim=" << sdim
                                      << " bounds=[" << sbmin[0] << "," << sbmin[1] << "," << sbmin[2]
                                      << "]-[" << sbmax[0] << "," << sbmax[1] << "," << sbmax[2] << "]"
                                      << " u16min=" << vmin << " u16max=" << vmax
                                      << " dmax=" << (signed_encoding_
                                          ? (f32)vmax / 65535.0f * 2.0f * sdim - sdim
                                          : (f32)vmax / 65535.0f * sdim)
                                      << std::endl;
                        }
                        // M3-A1: propagate clamped air to true distances
                        // before the merge consumes the field (SDF_NO_
                        // PROPAGATE=1 disables for A/B).
                        if (!std::getenv("SDF_NO_PROPAGATE"))
                            PropagateMeshSDF(sdf);
                        sdf.source_name = sourceName;
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

        // A9 Lightmap UV sidecar: flagged by bit 0x10 in elemType, a "UVLM"
        // section (magic | uv_count | uv_count * f32 u,v) trails the SDF
        // block. Must be consumed or the next mesh header misparses.
        if ((elemType & 0x10) && off + 8 <= (size_t)size) {
            off += 4; // UVLM magic
            u32 uvlmCount = ReadU32(data, off); off += 4;
            off += (size_t)uvlmCount * 8;
        }

        // T4.2 card bake: flagged by bit 0x20, a "CDBK" section (magic |
        // card_count | cards | surfel_count | surfels, 32 B per record)
        // trails the UVLM section. Same consume-or-misparse contract.
        if ((elemType & 0x20) && off + 8 <= (size_t)size) {
            off += 4; // CDBK magic
            u32 cardCount = ReadU32(data, off); off += 4;
            off += (size_t)cardCount * 32;
            if (off + 4 <= (size_t)size) {
                u32 surfelCount = ReadU32(data, off); off += 4;
                off += (size_t)surfelCount * 32;
            }
        }
        continue;
    } // end mesh loop
    } // end lod-group loop

    // Compute global bounds
    if (mesh_sdfs_.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] No SDF data found in " << filePath << std::endl;
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
            math::set_component(origin_, j, std::min(math::component(origin_, j), sdf.bounds_min[j]));
            math::set_component(maxPt, j, std::max(math::component(maxPt, j), sdf.bounds_max[j]));
        }
    }
    // Add small margin
    math::v3 margin = (maxPt - origin_) * 0.02f;
    origin_ -= margin;
    maxPt += margin;
    extent_ = maxPt - origin_;
    voxel_size_ = std::max({math::component(extent_, 0), math::component(extent_, 1), math::component(extent_, 2)}) / (f32)GLOBAL_RES;

    PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Loaded " << sdfFound << " mesh SDFs"
              << ", global bounds: (" << math::component(origin_, 0) << "," << math::component(origin_, 1) << "," << math::component(origin_, 2)
              << ") extent=(" << math::component(extent_, 0) << "," << math::component(extent_, 1) << "," << math::component(extent_, 2) << ")"
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
        // 2026-09-10 M3-A1: out-of-grid used to return the 1e10 sentinel —
        // min() over meshes then went BLIND to this mesh (took a far other
        // mesh's distance), so the merged field OVERESTIMATED near grid
        // seams: tracers stepped straight through thin slabs (the arcade -y
        // floor, grid only ±0.35 m, was invisible until landed in). Replace
        // with the conservative AABB distance — a valid LOWER bound of the
        // true distance (sphere-tracing-safe: smaller steps, no tunneling).
        f32 ax = std::max(std::max(sdf.bounds_min[0] - lx, lx - sdf.bounds_max[0]), 0.0f);
        f32 ay = std::max(std::max(sdf.bounds_min[1] - ly, ly - sdf.bounds_max[1]), 0.0f);
        f32 az = std::max(std::max(sdf.bounds_min[2] - lz, lz - sdf.bounds_max[2]), 0.0f);
        return std::sqrt(ax * ax + ay * ay + az * az);
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

    auto sampleData = [&](int32_t x, int32_t y, int32_t z) -> f32 {
        u32 idx = (u32)z * sdf.resolution[0] * sdf.resolution[1]
                + (u32)y * sdf.resolution[0]
                + (u32)x;
        if (idx >= sdf.data.size()) return 1e10f;
        if (signed_encoding_) {
            // Signed bake (ContentTools fast-winding-number inside test):
            // packed as pack_float<16>(signedDist + max_dim, 0, 2*max_dim) —
            // negative inside, positive outside.
            f32 sd = (f32)sdf.data[idx] / 65535.0f * 2.0f * sdf.max_dim - sdf.max_dim;
            // Hollow-architecture sign repair. A building-scale enclosing
            // shell (one mesh = whole atrium envelope) geometrically encloses
            // the room air: winding number ≈ 1, distance clamps to −max_dim
            // (−25..−52 m on Sponza). Materially that air is NOT solid, and
            // min() over meshes would poison every cavity probe (v4 ran the
            // whole nave black this way). True material interiors only extend
            // a wall's thickness below the surface, so negatives deeper than
            // the band flip positive. False-cavity readings always sit at the
            // −max_dim clamp, far beyond the band; real masonry keeps its
            // sign within the band (thin-wall overstep protection intact);
            // deep pillar cores degrade to the nearest-surface distance,
            // which is what the tracers wanted anyway.
            const f32 band = 8.0f * voxel_size_;
            if (sd < -band) sd = -sd;
            return sd;
        }
        // Legacy unsigned bake: dist = u16 / 65535 * max_dim
        return (f32)sdf.data[idx] / 65535.0f * sdf.max_dim;
    };

    // Trilinear interpolation
    f32 c000 = sampleData(x0, y0, z0), c100 = sampleData(x1, y0, z0);
    f32 c010 = sampleData(x0, y1, z0), c110 = sampleData(x1, y1, z0);
    f32 c001 = sampleData(x0, y0, z1), c101 = sampleData(x1, y0, z1);
    f32 c011 = sampleData(x0, y1, z1), c111 = sampleData(x1, y1, z1);

    f32 c00 = c000 * (1 - tx) + c100 * tx;
    f32 c01 = c001 * (1 - tx) + c101 * tx;
    f32 c10 = c010 * (1 - tx) + c110 * tx;
    f32 c11 = c011 * (1 - tx) + c111 * tx;

    f32 c0 = c00 * (1 - ty) + c10 * ty;
    f32 c1 = c01 * (1 - ty) + c11 * ty;

    f32 d = c0 * (1 - tz) + c1 * tz;
    // T8.10b HISTORY: this function once forced unsigned output because the
    // sign came from occupancy voxels (which marked only a near-surface skin
    // and flooded Sponza's atrium cavity as "solid": nave-air probes read
    // −2.7/−0.38/−3.5 m). With the ContentTools ray-cast signed bake the
    // signed distance is real data, not a voxel-skin heuristic — the decode
    // above returns it directly and the T8.10b pathology does not apply.
    // Unsigned legacy bakes still decode positively (branch above).
    return d;
}


// ============================================================================
// 2026-09-10 M3-A1: per-mesh SDF rebake (see the header comment). Walks the
// pipeline model byte-for-byte; every LOD0 'SDF ' section is decoded, its
// stored field propagated (sign-aware 6-direction sweep — the v5 bake
// clamped air beyond the 3³ closest-point search to max_dim, so the stored
// field was skin+clamp, not a distance field), re-encoded, and emitted.
// All other bytes copy verbatim; section sizes are unchanged.

// ============================================================================
// 2026-09-10 M3-A1: propagate the stored per-mesh SDF field. The v5-era
// bake's closest-point search only covers the 3³ cell neighborhood — air
// beyond ~1 voxel was clamped to max_dim, so the stored field is
// skin+clamp, not a distance field (tracers tunnelled through thin slabs /
// escaped through walls; interior probes read garbage). Classic sign-aware
// 6-direction sweep: each voxel relaxes to |neighbor distance| + 1 step,
// sign carried by the voxel's own stored sign.
// ============================================================================
u32 OfflineSDFMerger::RebakePerMeshSDF(const char* inPath, const char* outPath) {
    std::ifstream in(inPath, std::ios::binary | std::ios::ate);
    if (!in) return 0;
    const std::streamsize fsize = in.tellg();
    in.seekg(0);
    std::vector<uint8_t> data((size_t)fsize);
    if (!in.read(reinterpret_cast<char*>(data.data()), fsize)) return 0;
    in.close();

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) return 0;
    size_t off = 0;          // read cursor
    size_t written = 0;      // output bytes committed
    u32 rebaked = 0;
    auto fail = [&](const char* stage) {
        out.flush(); out.close();
        std::remove(outPath);
        std::cerr << "[SDF_REBAKE] aborted at input offset " << off << " stage=" << stage << std::endl;
        return 0u;
    };
    // Bounds-checked reads and writes; any anomaly aborts all-or-nothing.
    auto need = [&](size_t n) -> bool { return off + n <= data.size(); };
    auto copyIn = [&](size_t n) { out.write(reinterpret_cast<const char*>(data.data() + off), (std::streamsize)n); off += n; written += n; };
    auto putU32 = [&](u32 v) { out.write(reinterpret_cast<const char*>(&v), 4); written += 4; };
    auto putBuf = [&](const void* p, size_t n) { out.write(reinterpret_cast<const char*>(p), (std::streamsize)n); written += n; };
    auto u32rd = [&](size_t o) -> u32 { u32 v; std::memcpy(&v, data.data() + o, 4); return v; };
    auto f32rd = [&](size_t o) -> f32 { f32 v; std::memcpy(&v, data.data() + o, 4); return v; };
    auto u16rd = [&](size_t o) -> u16 { u16 v; std::memcpy(&v, data.data() + o, 2); return v; };
    auto atEof = [&]() { return off >= data.size(); };

    // Prefix u32(0)
    if (!need(4)) return fail("numMat_need");
    const u32 prefix = u32rd(off); copyIn(4);
    if (prefix != 0) {
        // No prefix — restart output without it.
        out.close(); out.open(outPath, std::ios::binary | std::ios::trunc);
        written = 0; off = 0;
    }

    // num_materials + materials (3 length-prefixed strings each)
    if (!need(4)) return fail("materials_loop_need4");
    const u32 numMat = u32rd(off); copyIn(4);
    if (numMat > 10000) return fail("materials_slen");
    for (u32 mi = 0; mi < numMat; ++mi) {
        for (int fi = 0; fi < 3; ++fi) {
            if (!need(4)) return fail("materials_data");
            const u32 slen = u32rd(off); copyIn(4);
            if (slen > 10000 || !need(slen)) return fail("numLods_need4");
            copyIn(slen);
        }
    }

    // num_lod_groups
    if (!need(4)) return fail("group_need8");
    const u32 numLods = u32rd(off); copyIn(4);
    if (numLods == 0 || numLods > 100000) return fail("lodNameLen_need4");

    for (u32 g = 0; g < numLods; ++g) {
        if (!need(8)) return fail("name_need");
        const u32 lodNameLen = u32rd(off); copyIn(4);
        if (!need(lodNameLen)) return fail("meshhdr_need32");
        copyIn(lodNameLen);
        if (!need(4)) return fail("geo_need");
        const u32 meshCount = u32rd(off); copyIn(4);
        if (meshCount > 100000) return fail("mshl_need8");

        for (u32 i = 0; i < meshCount; ++i) {
            if (!need(32)) return fail("mshl_magic");
            const u32 nameLen = u32rd(off); copyIn(4);
            if (nameLen > 200 || !need(nameLen)) return fail("mshl_need4");
            copyIn(nameLen);
            static bool dbg0 = false;
            if (!dbg0) { dbg0 = true;
                std::cerr << "[SDF_REBAKE][DBG] mesh0 header off=" << off
                          << " lodId=" << u32rd(off) << " matIdx=" << u32rd(off+4)
                          << " elemSize=" << u32rd(off+8) << " elemType=" << u32rd(off+12)
                          << " vertCount=" << u32rd(off+16) << " idxSize=" << u32rd(off+20)
                          << " idxCount=" << u32rd(off+24) << std::endl;
                std::cerr << "[SDF_REBAKE][DBG] preceding bytes:" << std::endl;
                for (int i = -32; i < 32; i += 16) {
                    if (off + i >= 0 && off + i + 16 <= (int)data.size()) {
                        char hex[50];
                        for (int j = 0; j < 16; ++j)
                            std::snprintf(hex + j*3, 4, "%02X ", data[off + i + j]);
                        std::cerr << "  off+" << i << ": " << hex << std::endl;
                    }
                }
            }
            const u32 lodId     = u32rd(off);
            const u32 elemSize  = u32rd(off + 8);
            const u32 elemType  = u32rd(off + 12);
            const u32 vertCount = u32rd(off + 16);
            const u32 idxSize   = u32rd(off + 20);
            const u32 idxCount  = u32rd(off + 24);
            copyIn(32);
            const size_t geoBytes = 12ull * vertCount + (size_t)elemSize * vertCount +
                                    (size_t)idxSize * idxCount;
            if (!need(geoBytes)) {
                std::cerr << "[SDF_REBAKE][DBG] geoBytes=" << geoBytes << " off=" << off
                          << " data.size()=" << data.size() << std::endl;
                return fail("mlv_need4");
            }
            copyIn(geoBytes);

            // MSHL section (mandatory for pipeline models)
            if (!need(8) || u32rd(off) != 0x4C48534D) return fail("mlv_data");
            copyIn(4);
            if (!need(4)) return fail("mlt_need4");
            const u32 mlCount = u32rd(off); copyIn(4);
            if (!need((size_t)mlCount * 60)) return fail("mlt_data");
            copyIn((size_t)mlCount * 60);
            if (!need(4)) return fail("sdf_need8");
            const u32 mlvCount = u32rd(off); copyIn(4);
            if (!need((size_t)mlvCount * 4)) return fail("sdf_hdr_need");
            copyIn((size_t)mlvCount * 4);
            if (!need(4)) return fail("sdf_cnt_need");
            const u32 mltCount = u32rd(off); copyIn(4);
            if (!need(mltCount)) return fail("nonlod0_data");
            copyIn(mltCount);

            const bool lod0 = (lodId == 0xFFFFFFFFu || lodId == 0u);

            // SDF section
            if (!need(8) || u32rd(off) != 0x20464453) return fail("nonlod0_vox");
            copyIn(4);
            if (!need(12 + 24 + 4)) return fail("nonlod0_vf_need4");
            const u32 sres[3] = { u32rd(off), u32rd(off + 4), u32rd(off + 8) };
            const f32 sbmin[3] = { f32rd(off + 12), f32rd(off + 16), f32rd(off + 20) };
            const f32 sbmax[3] = { f32rd(off + 24), f32rd(off + 28), f32rd(off + 32) };
            const u32 dataCount = u32rd(off + 36);
            const u32 voxCountFile = u32rd(off + 40);
            const u32 vfCount = u32rd(off + 44);
            const u32 voxCount = sres[0] * sres[1] * sres[2];
            const bool layoutOk = dataCount == voxCount && voxCountFile == voxCount &&
                                  vfCount == voxCount * 4u && voxCount > 0 &&
                                  sres[0] && sres[1] && sres[2];


            if (!lod0 || !layoutOk) {
                // Consume verbatim (guards mirror LoadFromPipelineModel).
                if (!need(4 + (size_t)dataCount * 2)) return fail("nonlod0_vf_data");
                copyIn(4 + (size_t)dataCount * 2);
                if (!need(4 + voxCountFile)) return fail("trail_need");
                copyIn(4 + voxCountFile);
                if (!need(4)) return fail("nonlod0_vf_need4");
                const u32 vfN = u32rd(off);
                if (vfN > 100000000) return fail("nonlod0_vf_need4");
                if (!need(4 + (size_t)vfN * 2)) return fail("nonlod0_vf_need4");
                copyIn(4 + (size_t)vfN * 2);
                continue;
            }

            copyIn(36 + 12); // header: res + bounds + counts (already read)

            f32 sdim = std::max({ sbmax[0] - sbmin[0], sbmax[1] - sbmin[1],
                                  sbmax[2] - sbmin[2] });
            if (sdim < 1e-6f) sdim = 1.0f;
            const f32 sdim2 = 2.0f * sdim;
            const f32 step[3] = { (sbmax[0] - sbmin[0]) / sres[0],
                                  (sbmax[1] - sbmin[1]) / sres[1],
                                  (sbmax[2] - sbmin[2]) / sres[2] };

            // Decode the stored field.
            std::vector<f32> sd(voxCount);
            std::vector<f32> cpts(voxCount * 3);
            std::vector<uint8_t> inside(voxCount);
            std::vector<uint16_t> voxels(voxCount);
            std::vector<uint16_t> newData(voxCount);
            std::vector<uint16_t> newVf(voxCount * 4);
            const size_t dataOff = off;
            const size_t voxOff = dataOff + (size_t)dataCount * 2;
            const size_t vfOff = voxOff + voxCount;
            if (!need((size_t)dataCount * 2 + voxCount + (size_t)vfCount * 2))
                return fail("nonlod0_vf_need4");
            std::memcpy(voxels.data(), data.data() + voxOff, voxCount);
            auto enc = [sdim, sdim2](f32 v) -> u16 {
                v = std::clamp(v, -sdim, sdim);
                return (u16)std::clamp(
                    (int)((v + sdim) / sdim2 * 65535.0f + 0.5f), 0, 65535);
            };
            auto relax = [&](u32 idx, u32 nidx) {
                const f32 dx = cpts[nidx * 3 + 0] - cpts[idx * 3 + 0];
                const f32 dy = cpts[nidx * 3 + 1] - cpts[idx * 3 + 1];
                const f32 dz = cpts[nidx * 3 + 2] - cpts[idx * 3 + 2];
                const f32 len = std::sqrt(dx * dx + dy * dy + dz * dz);
                const f32 cand = inside[idx] ? -len : len;
                if (std::fabs(cand) < std::fabs(sd[idx])) {
                    sd[idx] = cand;
                    cpts[idx * 3 + 0] = cpts[nidx * 3 + 0];
                    cpts[idx * 3 + 1] = cpts[nidx * 3 + 1];
                    cpts[idx * 3 + 2] = cpts[nidx * 3 + 2];
                }
            };

            for (u32 vz = 0; vz < sres[2]; ++vz)
            for (u32 vy = 0; vy < sres[1]; ++vy)
            for (u32 vx = 0; vx < sres[0]; ++vx) {
                const u32 idx = vz * sres[0] * sres[1] + vy * sres[0] + vx;
                sd[idx] = (f32)u16rd(dataOff + (size_t)idx * 2) / 65535.0f * sdim2 - sdim;
                inside[idx] = sd[idx] < 0.0f ? 1u : 0u;
                const f32 cx = sbmin[0] + ((f32)vx + 0.5f) * step[0];
                const f32 cy = sbmin[1] + ((f32)vy + 0.5f) * step[1];
                const f32 cz = sbmin[2] + ((f32)vz + 0.5f) * step[2];
                for (int c = 0; c < 3; ++c) {
                    const f32 o = (f32)u16rd(vfOff + (size_t)idx * 8 + (size_t)c * 2) /
                                  65535.0f * sdim2 - sdim;
                    cpts[idx * 3 + c] = (c == 0 ? cx : c == 1 ? cy : cz) + o;
                }
            }

            for (int iter = 0; iter < 2; ++iter)
            for (int pass = 0; pass < 6; ++pass) {
                const int axis = pass / 2;
                const bool forward = (pass % 2) == 0;
                const int sx = axis == 0 ? (forward ? 1 : -1) : 0;
                const int sy = axis == 1 ? (forward ? 1 : -1) : 0;
                const int sz = axis == 2 ? (forward ? 1 : -1) : 0;
                for (int pz = 0; pz < (int)sres[2]; ++pz)
                for (int py = 0; py < (int)sres[1]; ++py)
                for (int px = 0; px < (int)sres[0]; ++px) {
                    const int x = sx > 0 ? px : (int)sres[0] - 1 - px;
                    const int y = sy > 0 ? py : (int)sres[1] - 1 - py;
                    const int z = sz > 0 ? pz : (int)sres[2] - 1 - pz;
                    const int nx = x - sx, ny = y - sy, nz = z - sz;
                    if (nx < 0 || nx >= (int)sres[0] ||
                        ny < 0 || ny >= (int)sres[1] ||
                        nz < 0 || nz >= (int)sres[2]) continue;
                    relax((u32)z * sres[0] * sres[1] + (u32)y * sres[0] + (u32)x,
                          (u32)nz * sres[0] * sres[1] + (u32)ny * sres[0] + (u32)nx);
                }
            }

            for (u32 vz = 0; vz < sres[2]; ++vz)
            for (u32 vy = 0; vy < sres[1]; ++vy)
            for (u32 vx = 0; vx < sres[0]; ++vx) {
                const u32 idx = vz * sres[0] * sres[1] + vy * sres[0] + vx;
                newData[idx] = enc(sd[idx]);
                const f32 cx = sbmin[0] + ((f32)vx + 0.5f) * step[0];
                const f32 cy = sbmin[1] + ((f32)vy + 0.5f) * step[1];
                const f32 cz = sbmin[2] + ((f32)vz + 0.5f) * step[2];
                for (int c = 0; c < 3; ++c)
                    newVf[(size_t)idx * 4 + c] =
                        enc(cpts[idx * 3 + c] - (c == 0 ? cx : c == 1 ? cy : cz));
                newVf[(size_t)idx * 4 + 3] = 0;
            }

            putU32(dataCount);
            putBuf(newData.data(), (size_t)voxCount * 2);
            putU32(voxCountFile);
            putBuf(voxels.data(), voxCount);
            putU32(vfCount);
            putBuf(newVf.data(), (size_t)vfCount * 2);
            off = vfOff + (size_t)vfCount * 2;
            ++rebaked;
            continue;
        }

        // Per-mesh UVLM/CDBK sidecar sections are consumed inside the mesh
        // loop by the caller-side logic; Sponza's v5 model carries them per
        // mesh, and any remaining bytes flow through the walk below.
    }

    // Trailing bytes
    if (off < data.size()) copyIn(data.size() - off);
    out.flush();
    std::cerr << "[SDF_REBAKE] wrote " << outPath << " (" << rebaked
              << " sections propagated)" << std::endl;
    return rebaked;
}

bool OfflineSDFMerger::BuildAndUpload(rhi::RHIDeviceBase* device, const char* cachePath) {
    if (geometry_decode_ && hit_evidence_) return false;
    if (geometry_decode_ && cachePath) {
        // Also protects path aliases/symlinks to a normal production cache.
        // A stale experiment may rebuild, but never overwrite another format.
        std::ifstream existing(cachePath,std::ios::binary);
        if (existing) {
            char magic[8];
            if (!existing.read(magic,8) || std::memcmp(magic,"PGSDFG01",8)) {
                PRIMAL_LOG_STREAM(Error,"Render") << "[SDFGeometryDecode] refused overwrite of non-geometry cache: " << cachePath << std::endl;
                return false;
            }
        }
    }
    device_ = device;
    if (mesh_sdfs_.empty() || !device_) return false;
    if (hit_evidence_ && (std::getenv("SDF_NO_PROPAGATE") || std::getenv("OFFLINE_SDF_SEAL") || std::getenv("PRIMAL_OFFLINE_SDF_ZERO"))) return false;

    const u32 res = GLOBAL_RES;
    const u64 totalVoxels = (u64)res * res * res;

    // Allocate CPU-side half-float buffer.
    // R16F: we store the signed distance as half-float.
    // We'll compute as f32 then convert to f16.
    const u32 channels=hit_evidence_?2u:1u;
    std::vector<u16> globalData(totalVoxels*channels, 0x3C00);

    f32 stepX = math::component(extent_, 0) / (f32)res;
    f32 stepY = math::component(extent_, 1) / (f32)res;
    f32 stepZ = math::component(extent_, 2) / (f32)res;

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

    PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Merging " << mesh_sdfs_.size()
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
            std::vector<f32> measured(hit_evidence_?size_t(res)*res:0,1e10f);
            for (;;) {
                const u32 gz = nextSlice.fetch_add(1);
                if (gz >= res) break;
                const f32 wz = math::component(origin_, 2) + ((f32)gz + 0.5f) * stepZ;
                std::fill(best.begin(), best.end(), 1e10f);
                std::fill(measured.begin(), measured.end(), 1e10f);

                for (const auto& sdf : mesh_sdfs_) {
                    if (wz < sdf.bounds_min[2] - 0.01f || wz > sdf.bounds_max[2] + 0.01f)
                        continue;
                    const f32 wy0 = (sdf.bounds_min[1] - 0.01f - math::component(origin_, 1)) / stepY - 0.5f;
                    const f32 wy1 = (sdf.bounds_max[1] + 0.01f - math::component(origin_, 1)) / stepY - 0.5f;
                    const f32 wx0 = (sdf.bounds_min[0] - 0.01f - math::component(origin_, 0)) / stepX - 0.5f;
                    const f32 wx1 = (sdf.bounds_max[0] + 0.01f - math::component(origin_, 0)) / stepX - 0.5f;
                    const int32_t gy0 = std::max(0, (int32_t)std::floor(wy0));
                    const int32_t gy1 = std::min((int32_t)res - 1, (int32_t)std::ceil(wy1));
                    const int32_t gx0 = std::max(0, (int32_t)std::floor(wx0));
                    const int32_t gx1 = std::min((int32_t)res - 1, (int32_t)std::ceil(wx1));

                    for (int32_t gy = gy0; gy <= gy1; ++gy) {
                        const f32 wy = math::component(origin_, 1) + ((f32)gy + 0.5f) * stepY;
                        for (int32_t gx = gx0; gx <= gx1; ++gx) {
                            const f32 wx = math::component(origin_, 0) + ((f32)gx + 0.5f) * stepX;
                            const f32 d = SampleMeshSDF(sdf, wx, wy, wz);
                            // T8.10b: reject sentinel/garbage candidates —
                            // anything beyond ±1e4 (the 1e10 out-of-grid
                            // sentinel or a NaN/inf reinterpretation) must
                            // never enter the min() reduction.
                            if (!(d >= -1.0e4f && d <= 1.0e4f)) continue;
                            f32& b = best[(size_t)gy * res + gx];
                            if (d < b) b = d;
                            if (hit_evidence_) {
                                const float world[3]={wx,wy,wz};
                                bool supported=true;
                                for (int k=0;k<3;++k) {
                                    const float local=(world[k]-sdf.bounds_min[k])/((sdf.bounds_max[k]-sdf.bounds_min[k])/float(sdf.resolution[k]))-.5f;
                                    supported &= local>=0.f && local<=float(sdf.resolution[k])-1.f;
                                }
                                if (supported) measured[size_t(gy)*res+gx]=std::min(measured[size_t(gy)*res+gx],d);
                            }
                        }
                    }
                }

                const u64 zBase = (u64)gz * res * res;
                for (u32 gy = 0; gy < res; ++gy) {
                    for (u32 gx = 0; gx < res; ++gx) {
                        f32 bd = best[(size_t)gy * res + gx];
                        if (bd > 1e9f) bd = emptyDist;
                        const u64 at=(zBase + (u64)gy * res + gx)*channels;
                        globalData[at] = f32toF16(bd);
                        if (hit_evidence_) globalData[at+1]=f32toF16(std::min(measured[size_t(gy)*res+gx],65504.f));
                    }
                }
                const u32 done = doneSlices.fetch_add(1) + 1;
                if (done % 96 == 0) {
                    PRIMAL_LOG_STREAM(Error, "Render") << "  Merged slice " << done << "/" << res << std::endl;
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
    PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Merge complete. Uploading to GPU..." << std::endl;

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
        PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Seam seal: " << sealed << " voxels clamped to "
                  << sealDist << std::endl;
    }

    // Dump SDF statistics to verify data has both positive and negative values.
    {
        int negCount = 0, posCount = 0, zeroCount = 0;
        f32 minVal = 1e10f, maxVal = -1e10f;
        for (u64 i = 0; i < totalVoxels; ++i) {
            // Decode half-float back to float for stats
            u16 h = globalData[i*channels];
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
        PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] SDF stats: neg=" << negCount
                  << " pos=" << posCount << " zero=" << zeroCount
                  << " min=" << minVal << " max=" << maxVal << std::endl;
    }

    // Optional disk cache — the merge is a multi-minute one-shot for a static
    // scene; later runs LoadFromCache instead of re-parsing the pipeline model.
    if (cachePath) {
        struct CacheHeader {
            char magic[8];   // "PGSDFC02"
            u32  res;
            f32  origin[3];
            f32  extent[3];
            f32  voxel;
            u32  flags;      // bit0: signed encoding
            u32  _pad;       // keep 8-byte alignment for the u64 below
            u64  texelCount;
        };
        CacheHeader hdr{};
        std::memcpy(hdr.magic, geometry_decode_?"PGSDFG01":(hit_evidence_?"PGSDFE01":"PGSDFC02"), 8);
        hdr.res = res;
        hdr.origin[0] = math::component(origin_, 0); hdr.origin[1] = math::component(origin_, 1); hdr.origin[2] = math::component(origin_, 2);
        hdr.extent[0] = math::component(extent_, 0); hdr.extent[1] = math::component(extent_, 1); hdr.extent[2] = math::component(extent_, 2);
        hdr.voxel = voxel_size_;
        hdr.flags = signed_encoding_ ? 1u : 0u;
        hdr._pad = hit_evidence_?1u:0u; // evidence algorithm v1, propagation must be enabled
        hdr.texelCount = totalVoxels;
        std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
            out.write(reinterpret_cast<const char*>(globalData.data()),
                      (std::streamsize)(totalVoxels * channels * sizeof(u16)));
            out.close();
            PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Cache written: " << cachePath << " ("
                      << (totalVoxels * channels * sizeof(u16)) / (1024 * 1024) << " MB, signed="
                      << signed_encoding_ << ")" << std::endl;
        } else {
            PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] WARNING: cache write failed: " << cachePath << std::endl;
        }
    }

    return UploadGlobal(device_, globalData.data());
}

bool OfflineSDFMerger::LoadFromCache(rhi::RHIDeviceBase* device, const char* cachePath) {
    if (geometry_decode_ && hit_evidence_) return false;
    device_ = device;
    if (!device_ || !cachePath) return false;

    struct CacheHeader {
        char magic[8];
        u32  res;
        f32  origin[3];
        f32  extent[3];
        f32  voxel;
        u32  flags;
        u32  _pad;
        u64  texelCount;
    };
    std::ifstream in(cachePath, std::ios::binary);
    if (!in) return false;

    CacheHeader hdr{};
    in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    // PGSDFC01 (pre-signed bake) is stale by definition — its field semantics
    // differ, so accept only v2 caches whose encoding matches this merger's.
    const bool hdrOk = std::memcmp(hdr.magic, geometry_decode_?"PGSDFG01":(hit_evidence_?"PGSDFE01":"PGSDFC02"), 8) == 0 && (!hit_evidence_ || hdr._pad==1u);
    const bool flagsOk = (hdr.flags & 1u) == (signed_encoding_ ? 1u : 0u);
    if (!in || !hdrOk || !flagsOk ||
        hdr.res != GLOBAL_RES || hdr.texelCount != (u64)GLOBAL_RES * GLOBAL_RES * GLOBAL_RES) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Cache stale/invalid: " << cachePath
                  << (hdrOk ? (flagsOk ? " (geometry)" : " (encoding mismatch)") : " (magic)")
                  << std::endl;
        return false;
    }

    const u32 channels=hit_evidence_?2u:1u;
    std::vector<u16> globalData(hdr.texelCount*channels);
    in.read(reinterpret_cast<char*>(globalData.data()),
            (std::streamsize)(hdr.texelCount * channels * sizeof(u16)));
    if (!in) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Cache truncated: " << cachePath << std::endl;
        return false;
    }

    math::set_component(origin_, 0, hdr.origin[0]); math::set_component(origin_, 1, hdr.origin[1]); math::set_component(origin_, 2, hdr.origin[2]);
    math::set_component(extent_, 0, hdr.extent[0]); math::set_component(extent_, 1, hdr.extent[1]); math::set_component(extent_, 2, hdr.extent[2]);
    voxel_size_ = hdr.voxel;

    PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Loaded cache: " << cachePath << " ("
              << (hdr.texelCount * channels * sizeof(u16)) / (1024 * 1024) << " MB)" << std::endl;
    return UploadGlobal(device_, globalData.data());
}

bool OfflineSDFMerger::UploadGlobal(rhi::RHIDeviceBase* device, const u16* globalData) {
    const u32 res = GLOBAL_RES;
    const u32 channels=hit_evidence_?2u:1u;
    const u64 totalVoxels = (u64)res * res * res * channels; // components for bit verification

    // Create Texture3D and upload.
    rhi::TextureDesc desc{};
    desc.size = {res, res, res};
    desc.mipLevels = 1;
    desc.arraySize = 1;
    desc.format = hit_evidence_?rhi::DataFormat::RG16_Float:rhi::DataFormat::R16_Float;
    desc.type = rhi::TextureType::Texture3D;
    desc.usage = rhi::TextureUsage::ShaderResource | rhi::TextureUsage::CopyDest |
                 rhi::TextureUsage::CopySource;
    desc.memoryUsage = rhi::GPUMemoryUsage::Static;
    desc.name = "OfflineSDFMerger_GlobalSDF";

    global_sdf_texture_ = device_->CreateTexture(desc);
    if (global_sdf_texture_ == rhi::handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Failed to create Texture3D" << std::endl;
        return false;
    }

    // Upload via staging buffer (mirrors RHIGpuMesh::CreateAndUploadTexture3D).
    u32 bytesPerTexel = hit_evidence_?4u:2u;
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
        PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Failed to create staging buffer" << std::endl;
        return false;
    }

    void* mapped = device_->MapBuffer(staging, 0, totalSize);
    if (!mapped) {
        device_->DestroyBuffer(staging);
        return false;
    }

    // Copy row-by-row with alignment padding.
    // PRIMAL_OFFLINE_SDF_ZERO=1：判定实验——整场填 0.0（d=0 < 命中阈值，
    // 每根 trace 射线必然瞬时命中）。若 ray-path 直方图 miss 仍 ~97%，
    // 说明 DDGI trace 采样的不是这块纹理（绑定/解析断链）。
    const bool debugZeroField = std::getenv("PRIMAL_OFFLINE_SDF_ZERO") != nullptr;
    u8* dst = static_cast<u8*>(mapped);
    if (debugZeroField) {
        std::memset(dst, 0, totalSize);
        std::cerr << "[OfflineSDFMerger] DEBUG zero field uploaded (PRIMAL_OFFLINE_SDF_ZERO=1)" << std::endl;
    } else {
        for (u32 z = 0; z < res; ++z) {
            for (u32 y = 0; y < res; ++y) {
                const u16* srcRow = &globalData[((u64)z * res * res + (u64)y * res)*channels];
                std::memcpy(dst, srcRow, srcRowPitch);
                dst += alignedRowPitch;
            }
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
        PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFMerger] Unsupported device for texture upload" << std::endl;
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
    // The first SC/DDGI/SPGI consumer may run immediately after upload.
    // Leaving the 3D image in CopyDest (TRANSFER_DST_OPTIMAL on Vulkan)
    // makes sampled-image descriptors invalid even though the texels are
    // correct; publish it as a shader resource before the upload completes.
    rhi::ResourceBarrier ready{};
    ready.resource = global_sdf_texture_;
    ready.beforeState = rhi::ResourceState::CopyDest;
    ready.afterState = rhi::ResourceState::ShaderResource;
    ready.subresource = 0xFFFFFFFFu;
    cmd->InsertBarrier(&ready, 1);
    cmd->End();
    cmd->Submit();
    cmd->WaitForCompletion();
    device_->DestroyCommandBuffer(cmdHandle);
    device_->DestroyBuffer(staging);

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
        rhi::CommandBufferHandle vcHandle = rhi::handles::INVALID_COMMAND_BUFFER;
        if (ok) {
            vcHandle = device_->CreateCommandBuffer(rhi::CommandQueueType::Graphics);
            ok = vcHandle != rhi::handles::INVALID_COMMAND_BUFFER;
        }
        if (ok) {
            // T8.13 fix: platform-generic readback (the old Vulkan-only
            // cast left vStage unwritten on Metal — the verify then compared
            // against a zeroed buffer and reported bogus mismatches).
            rhi::RHICommandBuffer* vc = rhi::GetCommandBuffer(vcHandle);
            if (!vc) {
                ok = false;
                PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFVerify] no command buffer for readback" << std::endl;
            } else {
                vc->Begin();
                rhi::BufferTextureCopyRegion vr{};
                vr.bufferOffset = 0;
                vr.bufferRowLength = alignedRowPitch / bytesPerTexel;
                vr.bufferImageHeight = res;
                vr.imageSubresource.mipLevel = 0;
                vr.imageSubresource.baseArrayLayer = 0;
                vr.imageSubresource.layerCount = 1;
                vr.imageOffset = {0, 0, 0};
                vr.imageExtent = {res, res, res};
                vc->CopyTextureToBuffer(global_sdf_texture_, vStage, &vr, 1);
                vc->End();
                vc->Submit();
                vc->WaitForCompletion();
            }
            if (ok && std::getenv("OFFLINE_SDF_VERIFY")) {
                if (void* vm = device_->MapBuffer(vStage, 0, totalSize)) {
                    const u16* gpu = static_cast<const u16*>(vm);
                    u64 mismatch = 0; f32 maxAbs = 0.f;
                    int shown = 0;
                    for (u32 gz = 0; gz < res && shown < 8; ++gz)
                    for (u32 gy = 0; gy < res && shown < 8; ++gy)
                    for (u32 gx = 0; gx < res*channels; ++gx) {
                        const u64 li = ((u64)gz * res * res + (u64)gy * res)*channels + gx;
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
                                PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFVerify] first diffs (x,y,z cpu gpu): "
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
                            u32 gz = u32(li / ((u64)res * res * channels));
                            u32 rem = u32(li % ((u64)res * res * channels));
                            u32 gy = rem / (res*channels), gx = rem % (res*channels);
                            u64 gi = (u64)gz * slicePitch / sizeof(u16)
                                   + (u64)gy * alignedRowPitch / sizeof(u16) + gx;
                            if (globalData[li] != gpu[gi]) ++mismatch;
                        }
                    }
                    PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFVerify] texels=" << totalVoxels
                              << " mismatched=" << mismatch
                              << " (" << (100.0 * double(mismatch) / double(totalVoxels)) << "%)"
                              << (mismatch ? "  => UPLOAD PATH CORRUPT" : "  => upload bit-exact")
                              << std::endl;
                    device_->UnmapBuffer(vStage);
                } else ok = false;
            } else if (!ok) {
                PRIMAL_LOG_STREAM(Error, "Render") << "[OfflineSDFVerify] readback setup failed" << std::endl;
            }
            device_->DestroyBuffer(vStage);
        }
    }
    return true;
}

bool OfflineSDFMerger::AuditTraceEvidence(const char* raysCsv, const char* cachePath, const char* outputCsv) const {
    struct Header { char magic[8]; u32 res; f32 origin[3], extent[3], voxel; u32 flags, pad; u64 count; };
    static_assert(sizeof(Header)==56);
    Header h{};
    std::ifstream cache(cachePath,std::ios::binary), input(raysCsv);
    if (!cache.read(reinterpret_cast<char*>(&h),sizeof(h)) || !input || mesh_sdfs_.empty() ||
        std::memcmp(h.magic,"PGSDFC02",8)!=0 || h.res!=GLOBAL_RES ||
        h.count!=u64(GLOBAL_RES)*GLOBAL_RES*GLOBAL_RES || ((h.flags&1u)!=0u)!=signed_encoding_) return false;
    float step[3];
    for (int k=0;k<3;++k) {
        if (!(h.extent[k]>0.f) || std::abs(h.origin[k]-math::component(origin_, k))>.001f || std::abs(h.extent[k]-math::component(extent_, k))>.001f) return false;
        step[k]=h.extent[k]/float(h.res);
    }
    std::vector<u16> texels(h.count);
    if (!cache.read(reinterpret_cast<char*>(texels.data()),texels.size()*sizeof(u16))) return false;
    auto half=[](u16 v) {
        const int sign=(v&0x8000)?-1:1;
        const u32 e=(v>>10)&31u,m=v&1023u;
        return sign*(e==0u?std::ldexp(float(m),-24):std::ldexp(1.f+float(m)/1024.f,int(e)-15));
    };
    // Lazily replay just the experimental rays' texels, never a complete
    // cache rebuild. Unknown evidence is 1e10, NOT the marching emptyDist.
    std::unordered_map<u64,float> evidence;
    auto supportedTexel=[&](int x,int y,int z) {
        const u64 index=(u64(z)*h.res+u64(y))*h.res+u64(x);
        if (auto found=evidence.find(index);found!=evidence.end()) return found->second;
        const int grid[3]={x,y,z};
        float world[3];
        for (int k=0;k<3;++k) world[k]=h.origin[k]+(float(grid[k])+.5f)*step[k];
        float best=1e10f;
        for (const auto& sdf:mesh_sdfs_) {
            bool supported=true;
            for (int k=0;k<3;++k) {
                const float local=(world[k]-sdf.bounds_min[k])/((sdf.bounds_max[k]-sdf.bounds_min[k])/float(sdf.resolution[k]))-.5f;
                supported &= local>=0.f && local<=float(sdf.resolution[k])-1.f;
            }
            if (!supported) continue;
            const float d=SampleMeshSDF(sdf,world[0],world[1],world[2]);
            if (d>=-1e4f && d<=1e4f) best=std::min(best,d);
        }
        evidence.emplace(index,best);
        return best;
    };
    struct Sample { float coarse, trusted; bool known; };
    auto sample=[&](const float* pos,bool needEvidence) {
        Sample result{0.f,0.f,true};
        int base[3]; float f[3];
        for (int k=0;k<3;++k) {
            if (pos[k]<h.origin[k] || pos[k]>h.origin[k]+h.extent[k]) return Sample{1e10f,1e10f,false};
            const float p=(pos[k]-h.origin[k])/h.extent[k]*float(h.res)-.5f;
            base[k]=int(std::floor(p)); f[k]=p-float(base[k]);
        }
        for (int c=0;c<8;++c) {
            int grid[3]; float weight=1.f;
            for (int k=0;k<3;++k) {
                const int offset=(c>>k)&1;
                grid[k]=std::clamp(base[k]+offset,0,int(h.res)-1);
                weight*=offset?f[k]:1.f-f[k];
            }
            const u64 index=(u64(grid[2])*h.res+u64(grid[1]))*h.res+u64(grid[0]);
            result.coarse+=weight*half(texels[index]);
            if (needEvidence && weight>1e-7f) {
                const float d=supportedTexel(grid[0],grid[1],grid[2]);
                if (d>=1e9f) result.known=false;
                else result.trusted+=weight*d;
            }
        }
        if (!result.known) result.trusted=1e10f;
        return result;
    };
    std::ofstream output(outputCsv);
    if (!output) return false;
    output << "probe,ray,policy,budget,gpu_distance,triangle_distance,replay_distance,steps,rejected,unknown,last_t,hit_x,hit_y,hit_z\n";
    std::string line; std::getline(input,line);
    u32 rays=0;
    while (std::getline(input,line)) {
        std::stringstream stream(line); std::vector<std::string> columns; std::string column;
        while (std::getline(stream,column,',')) columns.push_back(column);
        if (columns.size()<19) continue;
        float values[19]{};
        for (int index:{5,6,7,9,10,11,12,13}) {
            char* end=nullptr; values[index]=std::strtof(columns[index].c_str(),&end);
            if (end==columns[index].c_str() || *end!='\0' || !std::isfinite(values[index])) return false;
        }
        ++rays;
        // 0: reference scalar rules; 1: independent evidence + same crawl;
        // 2/3: evidence gate + rejected-band stride cap at two global voxels.
        // Policy3 only increases budget; origins/directions are unchanged.
        for (int policy=0;policy<4;++policy) {
            const int budget=policy==3?256:64;
            float t=0.f,prevT=0.f,prevD=3.4e38f,hit=-1.f;
            bool cleared=false; int used=0,rejected=0,unknown=0;
            const float minStep=h.voxel*.25f,threshold=std::max(h.voxel*1.25f,.45f);
            for (int iter=0;iter<budget;++iter) {
                ++used;
                float pos[3]; for (int k=0;k<3;++k) pos[k]=values[5+k]+values[9+k]*t;
                const auto s=sample(pos,policy!=0);
                const float d=s.coarse;
                if (d>=1e9f) {
                    t+=h.extent[0]*.1f;
                    if (t>50.f) { hit=-2.f; break; }
                    continue;
                }
                const bool crossing=signed_encoding_ && cleared && d<=0.f && prevD>0.f && prevD<1e9f;
                const bool candidate=d<threshold || crossing;
                const bool valid=policy==0 || (s.known && s.trusted<threshold);
                if (candidate && valid) {
                    if (!cleared) {
                        t+=std::max(d,minStep);
                        if (t>50.f) { hit=-2.f; break; }
                        continue;
                    }
                    if (crossing) {
                        float lo=prevT,hi=t;
                        for (int b=0;b<5;++b) {
                            const float mid=.5f*(lo+hi);
                            float p[3];for (int k=0;k<3;++k) p[k]=values[5+k]+values[9+k]*mid;
                            const auto m=sample(p,policy!=0);
                            const float md=policy==0?m.coarse:m.trusted;
                            if (md<=0.f) hi=mid;else lo=mid;
                        }
                        hit=hi;
                    } else hit=t;
                    break;
                }
                if (candidate) { ++rejected; if (!s.known) ++unknown; }
                // Valid evidence outside hit skin permits clearing; unknown
                // air is NOT a surface but also doesn't certify free space.
                if (policy==0 || (s.known && s.trusted>=threshold)) cleared=true;
                float advance;
                if (candidate) advance=policy>=2?std::max(h.voxel*2.f,.20f):std::max(d,minStep);
                else if (d>6.f && d>=prevD) advance=d*1.5f;
                else advance=std::clamp(d,minStep,std::max(h.voxel*2.f,.20f));
                prevT=t;prevD=d;t+=advance;
                if (t>50.f) { hit=-2.f; break; }
            }
            float hp[3];for (int k=0;k<3;++k) hp[k]=values[5+k]+values[9+k]*std::max(hit,0.f);
            output << columns[1] << ',' << columns[3] << ',' << policy << ',' << budget << ',' << values[12] << ',' << values[13]
                   << ',' << hit << ',' << used << ',' << rejected << ',' << unknown << ',' << t << ',' << hp[0] << ',' << hp[1] << ',' << hp[2] << '\n';
        }
    }
    PRIMAL_LOG_STREAM(Error,"Render") << "[SDFTraceEvidence] rays=" << rays << " evidence_texels=" << evidence.size() << " output=" << outputCsv << std::endl;
    return rays>0 && bool(output);
}

bool OfflineSDFMerger::AuditCacheVoxels(const char* voxelsCsv, const char* cachePath, const char* outputCsv) const {
    struct Header { char magic[8]; u32 res; f32 origin[3], extent[3], voxel; u32 flags, pad; u64 count; };
    static_assert(sizeof(Header) == 56);
    Header h{};
    std::ifstream cache(cachePath, std::ios::binary);
    if (!cache.read(reinterpret_cast<char*>(&h), sizeof(h)) || mesh_sdfs_.empty() ||
        std::memcmp(h.magic, "PGSDFC02", 8) || h.res != GLOBAL_RES ||
        h.count != u64(GLOBAL_RES)*GLOBAL_RES*GLOBAL_RES ||
        ((h.flags & 1u) != 0u) != signed_encoding_) return false;
    cache.seekg(0, std::ios::end);
    if (cache.tellg() != std::streamoff(sizeof(h) + h.count*sizeof(u16))) return false;
    float step[3];
    for (int k=0; k<3; ++k) {
        if (!std::isfinite(h.extent[k]) || !(h.extent[k]>0.f) ||
            !std::isfinite(h.origin[k]) || std::abs(h.origin[k]-math::component(origin_, k))>.001f ||
            std::abs(h.extent[k]-math::component(extent_, k))>.001f) return false;
        step[k]=h.extent[k]/float(h.res);
    }
    std::ifstream input(voxelsCsv);
    std::string line;
    if (!std::getline(input,line) || line != "id,gx,gy,gz") return false;
    std::ofstream output(outputCsv);
    std::ofstream candidates(std::string(outputCsv)+".candidates.csv");
    if (!output || !candidates) return false;
    output.precision(9); candidates.precision(9);
    output << "id,gx,gy,gz,x,y,z,cache_distance,replayed_distance,winner_kind,mesh_name,measured_min,measured_mesh,match,candidates\n";
    candidates << "id,mesh_name,distance,kind,local_x,local_y,local_z,res_x,res_y,res_z\n";
    auto quote=[](const std::string& text) {
        std::string q="\"";
        for (char c:text) { if(c=='\"') q+='\"'; q+=c; }
        return q+'\"';
    };
    u32 points=0,matches=0;
    while (std::getline(input,line)) {
        std::stringstream stream(line);
        std::vector<std::string> columns; std::string column;
        while (std::getline(stream,column,',')) columns.push_back(column);
        if (columns.size()!=4 || columns[0].empty()) return false;
        int grid[3]; float world[3];
        for (int k=0;k<3;++k) {
            char* end=nullptr;
            const long value=std::strtol(columns[k+1].c_str(),&end,10);
            if (end==columns[k+1].c_str() || *end!='\0' || value<0 || value>=long(h.res)) return false;
            grid[k]=int(value); world[k]=h.origin[k]+(float(grid[k])+.5f)*step[k];
        }
        float best=1e10f,measured=1e10f;
        const MeshSDF* winner=nullptr; const MeshSDF* measuredWinner=nullptr;
        bool winnerFallback=false; u32 participants=0;
        for (const auto& sdf:mesh_sdfs_) {
            // Match BuildAndUpload's actual asymmetric sweep exactly: Z
            // tests the center, XY uses floor/ceil inclusive grid bounds.
            if (world[2]<sdf.bounds_min[2]-.01f || world[2]>sdf.bounds_max[2]+.01f) continue;
            bool participates=true,fallback=false; float local[3];
            for (int k=0;k<3;++k) {
                if (!sdf.resolution[k] || !(sdf.bounds_max[k]>sdf.bounds_min[k])) return false;
                if (k<2) {
                    const int lo=std::max(0,int(std::floor((sdf.bounds_min[k]-.01f-h.origin[k])/step[k]-.5f)));
                    const int hi=std::min(int(h.res)-1,int(std::ceil((sdf.bounds_max[k]+.01f-h.origin[k])/step[k]-.5f)));
                    participates &= grid[k]>=lo && grid[k]<=hi;
                }
                local[k]=(world[k]-sdf.bounds_min[k])/((sdf.bounds_max[k]-sdf.bounds_min[k])/float(sdf.resolution[k]))-.5f;
                fallback |= !(local[k]>=0.f && local[k]<=float(sdf.resolution[k])-1.f);
            }
            if (!participates) continue;
            const float d=SampleMeshSDF(sdf,world[0],world[1],world[2]);
            if (!(d>=-1e4f && d<=1e4f)) continue;
            ++participants;
            candidates << quote(columns[0]) << ',' << quote(sdf.source_name) << ',' << d << ','
                << (fallback?"aabb_lower_bound":"mesh_sample") << ',' << local[0] << ',' << local[1] << ',' << local[2]
                << ',' << sdf.resolution[0] << ',' << sdf.resolution[1] << ',' << sdf.resolution[2] << '\n';
            if (!fallback && d<measured) { measured=d; measuredWinner=&sdf; }
            if (d<best) { best=d; winner=&sdf; winnerFallback=fallback; }
        }
        if (best>1e9f) best=h.voxel*4.f;
        const u64 index=(u64(grid[2])*h.res+u64(grid[1]))*h.res+u64(grid[0]);
        cache.seekg(std::streamoff(sizeof(h)+index*sizeof(u16)));
        u16 bits;
        if (!cache.read(reinterpret_cast<char*>(&bits),sizeof(bits))) return false;
        const int sign=(bits&0x8000)?-1:1;
        const u32 e=(bits>>10)&31u,m=bits&1023u;
        const float actual=sign*(e==0u?std::ldexp(float(m),-24):std::ldexp(1.f+float(m)/1024.f,int(e)-15));
        const bool match=std::isfinite(actual) && std::abs(actual-best)<=.001f+std::abs(best)*.001f;
        ++points; matches+=match;
        output << quote(columns[0]) << ',' << grid[0] << ',' << grid[1] << ',' << grid[2]
            << ',' << world[0] << ',' << world[1] << ',' << world[2] << ',' << actual << ',' << best
            << ',' << (winner?(winnerFallback?"aabb_lower_bound":"mesh_sample"):"empty")
            << ',' << quote(winner?winner->source_name:"none") << ',' << measured
            << ',' << quote(measuredWinner?measuredWinner->source_name:"none") << ',' << match << ',' << participants << '\n';
    }
    PRIMAL_LOG_STREAM(Info,"Render") << "[SDFVoxelProvenance] points=" << points << " matches=" << matches
        << " complete=" << bool(output && candidates) << " output=" << outputCsv << std::endl;
    return points>0 && bool(output && candidates);
}

bool OfflineSDFMerger::AuditCachePoints(const char* raysCsv, const char* cachePath, const char* outputCsv) const {
    struct Header { char magic[8]; u32 res; f32 origin[3], extent[3], voxel; u32 flags, pad; u64 count; };
    static_assert(sizeof(Header) == 56);
    Header header{};
    std::ifstream cache(cachePath, std::ios::binary);
    if (!cache.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
        std::memcmp(header.magic,"PGSDFC02",8) != 0 || header.res != GLOBAL_RES ||
        header.count != u64(GLOBAL_RES)*GLOBAL_RES*GLOBAL_RES || mesh_sdfs_.empty()) return false;
    if (((header.flags & 1u) != 0u) != signed_encoding_) return false;
    std::vector<u16> texels(header.count);
    if (!cache.read(reinterpret_cast<char*>(texels.data()), texels.size()*sizeof(u16))) return false;
    std::ifstream input(raysCsv);
    if (!input) return false;
    auto half = [](u16 h) {
        const int sign = (h & 0x8000) ? -1 : 1;
        const u32 e=(h>>10)&31u, m=h&1023u;
        return sign * (e == 0u ? std::ldexp(float(m),-24) : std::ldexp(1.f+float(m)/1024.f,int(e)-15));
    };
    float step[3];
    for (int k=0;k<3;++k) {
        if (!(header.extent[k]>0.f)) return false;
        step[k]=header.extent[k]/float(header.res);
        if (std::abs(header.origin[k]-math::component(origin_, k))>.001f || std::abs(header.extent[k]-math::component(extent_, k))>.001f) {
            PRIMAL_LOG_STREAM(Error,"Render") << "[SDFProvenance] model/cache bounds differ; cannot replay" << std::endl;
            return false;
        }
    }
    std::ofstream output(outputCsv);
    if (!output) return false;
    output << "probe,ray,corner,x,y,z,weight,cache_distance,replayed_distance,winner_kind,mesh_name,measured_min,match\n";
    std::string line;
    std::getline(input,line); // triangle audit header
    u32 points=0, matches=0, corners=0;
    while (std::getline(input,line)) {
        std::stringstream stream(line);
        std::vector<std::string> columns;
        std::string column;
        while (std::getline(stream,column,',')) columns.push_back(column);
        if (columns.size()<19) continue;
        {
            float values[19]{};
            for (int index : {5,6,7,9,10,11,12,17}) {
                char* end=nullptr;
                values[index]=std::strtof(columns[index].c_str(), &end);
                if (end==columns[index].c_str() || *end!='\0' || !std::isfinite(values[index])) {
                    PRIMAL_LOG_STREAM(Error,"Render") << "[SDFProvenance] malformed CSV row" << std::endl;
                    return false;
                }
            }
            const float distance=values[12], surfaceDistance=values[17];
            if (distance<0.f || surfaceDistance<=.75f) continue;
            float p[3], fraction[3]; int base[3];
            for (int k=0;k<3;++k) {
                const float world=values[5+k]+values[9+k]*distance;
                p[k]=(world-header.origin[k])/step[k]-.5f;
                base[k]=int(std::floor(p[k])); fraction[k]=p[k]-float(base[k]);
            }
            ++points;
            for (int corner=0;corner<8;++corner) {
                int grid[3]; float world[3], weight=1.f;
                for (int k=0;k<3;++k) {
                    const int offset=(corner>>k)&1;
                    grid[k]=std::clamp(base[k]+offset,0,int(header.res)-1);
                    world[k]=header.origin[k]+(float(grid[k])+.5f)*step[k];
                    weight *= offset ? fraction[k] : 1.f-fraction[k];
                }
                float best=1e10f, measured=1e10f;
                const MeshSDF* winner=nullptr;
                bool winnerFallback=false;
                for (const auto& sdf:mesh_sdfs_) {
                    // Exact BuildAndUpload AABB sweep participation, including
                    // asymmetric z-slice / xy floor-ceil rules.
                    if (world[2]<sdf.bounds_min[2]-.01f || world[2]>sdf.bounds_max[2]+.01f) continue;
                    bool participates=true, fallback=false;
                    for (int k=0;k<3;++k) {
                        if (k<2) {
                            const int lo=std::max(0,int(std::floor((sdf.bounds_min[k]-.01f-header.origin[k])/step[k]-.5f)));
                            const int hi=std::min(int(header.res)-1,int(std::ceil((sdf.bounds_max[k]+.01f-header.origin[k])/step[k]-.5f)));
                            participates &= grid[k]>=lo && grid[k]<=hi;
                        }
                        const float local=(world[k]-sdf.bounds_min[k])/((sdf.bounds_max[k]-sdf.bounds_min[k])/float(sdf.resolution[k]))-.5f;
                        fallback |= !(local>=0.f && local<=float(sdf.resolution[k])-1.f);
                    }
                    if (!participates) continue;
                    const float d=SampleMeshSDF(sdf,world[0],world[1],world[2]);
                    if (!(d>=-1e4f && d<=1e4f)) continue;
                    if (!fallback) measured=std::min(measured,d);
                    if (d<best) { best=d; winner=&sdf; winnerFallback=fallback; }
                }
                if (best>1e9f) best=header.voxel*4.f;
                const u64 index=(u64(grid[2])*header.res+u64(grid[1]))*header.res+u64(grid[0]);
                const float actual=half(texels[index]);
                const bool match=std::abs(actual-best)<=.001f+std::abs(best)*.001f;
                matches+=match; ++corners;
                output << columns[1] << ',' << columns[3] << ',' << corner << ',' << world[0] << ',' << world[1] << ',' << world[2]
                       << ',' << weight << ',' << actual << ',' << best << ','
                       << (winner ? (winnerFallback ? "aabb_lower_bound" : "mesh_sample") : "empty") << ','
                       << (winner ? winner->source_name : "none") << ',' << measured << ',' << match << '\n';
            }
        }
    }
    PRIMAL_LOG_STREAM(Error,"Render") << "[SDFProvenance] points=" << points << " corners=" << corners
        << " cache_matches=" << matches << " output=" << outputCsv << std::endl;
    return points>0 && bool(output);
}

} // namespace primal::graphics::nanite
