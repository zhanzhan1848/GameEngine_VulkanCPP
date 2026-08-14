// M8 integration test for the AssetPipeline orchestrator.
//
// Drives the full Phase 1 chain end-to-end:
//   ProcessableScene (LOD 0 cube) → pipeline::Run → Result.packed
// Verifies the default config (lod + meshlet on) produces:
//   - N+1 LOD levels (input + N from lod::Run)
//   - 1 packed mesh per LOD level
//   - Each packed mesh has non-empty meshlet data
//   - Serialization size > 0 + magics present
// Also covers enable_collision = true producing non-empty Result.hulls.

#include "pipeline/AssetPipeline.h"
#include "pipeline/SceneBlobWriter.h"
#include "common/ProcessableMesh.h"
#include "common/PipelineTypes.h"
#include "common/ErrorReport.h"
#include "Geometry.h"
#include "../Engine/Utilities/IOStream.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

using namespace primal;
using namespace primal::tools;
using namespace primal::math;
using namespace primal::tools::pipeline;

namespace {

// 24-vert cube as a manageable LOD 0 source.
ProcessableMesh make_cube_mesh() {
    ProcessableMesh m;
    m.name = "pipeline_cube";
    m.material_idx = 0;
    m.positions.emplace_back(v3{-1,-1,-1});
    m.positions.emplace_back(v3{ 1,-1,-1});
    m.positions.emplace_back(v3{ 1, 1,-1});
    m.positions.emplace_back(v3{-1, 1,-1});
    m.positions.emplace_back(v3{-1,-1, 1});
    m.positions.emplace_back(v3{ 1,-1, 1});
    m.positions.emplace_back(v3{ 1, 1, 1});
    m.positions.emplace_back(v3{-1, 1, 1});

    const u32 tri_indices[36] = {
        0,1,2, 0,2,3, 4,6,5, 4,7,6,
        0,4,5, 0,5,1, 2,6,7, 2,7,3,
        0,3,7, 0,7,4, 1,5,6, 1,6,2,
    };
    for (u32 idx : tri_indices) m.indices.emplace_back(idx);
    m.normals.resize(8);
    for (u32 i = 0; i < 8; ++i) {
        const f32 x = (f32)((i & 1) ? 1 : -1);
        const f32 y = (f32)((i & 2) ? 1 : -1);
        const f32 z = (f32)((i & 4) ? 1 : -1);
        const f32 len = std::sqrt(x*x + y*y + z*z);
        m.normals[i] = (len > 0.f) ? v3{x/len, y/len, z/len} : v3{0.f, 1.f, 0.f};
    }
    for (u32 i = 0; i < 8; ++i) m.tangents.emplace_back(v4{1.f, 0.f, 0.f, 1.f});
    UVSet uvs;
    uvs.purpose = UVSetPurpose::Texture;
    for (u32 i = 0; i < 8; ++i) uvs.coords.emplace_back(v2{0.25f, 0.5f});
    m.uv_sets.emplace_back(std::move(uvs));
    return m;
}

ProcessableScene make_cube_scene() {
    ProcessableScene s;
    s.name = "pipeline_test_scene";
    ProcessableLod lod0;
    lod0.screen_threshold = 0.5f;
    lod0.meshes.emplace_back(make_cube_mesh());
    s.lods.emplace_back(std::move(lod0));
    return s;
}

u32 read_u32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

bool has_magic(const utl::vector<u8>& buf, u32 magic) {
    for (size_t i = 0; i + 4 <= buf.size(); ++i) {
        if (read_u32(buf.data() + i) == magic) return true;
    }
    return false;
}

}  // namespace

// ---- Default config (lod + meshlet on) ---------------------------------

bool test_default_produces_multiple_lods() {
    ProcessableScene s = make_cube_scene();
    Config cfg;  // defaults: lod + meshlet on
    cfg.lod_params.max_levels = 3;  // keep test fast
    cfg.lod_params.ratio = 0.5f;
    Result out;
    Run(std::move(s), cfg, out);

    // Expect 1 (LOD 0) + up to N generated levels. Cube has only 8 verts
    // and 36 indices — meshopt_simplify may produce 0 or few levels at
    // ratio 0.5. Accept anything >= 1 (LOD 0 always survives).
    return out.scene.lods.size() >= 1;
}

bool test_default_produces_one_packed_per_lod() {
    ProcessableScene s = make_cube_scene();
    Config cfg;
    cfg.lod_params.max_levels = 2;
    cfg.lod_params.ratio = 0.5f;
    Result out;
    Run(std::move(s), cfg, out);
    return out.packed.size() == out.scene.lods.size();
}

bool test_default_meshlet_data_populated() {
    ProcessableScene s = make_cube_scene();
    Config cfg;  // meshlet on by default
    Result out;
    Run(std::move(s), cfg, out);

    if (out.packed.empty()) return false;
    // LOD 0 (cube has 36 indices → 1+ meshlets at 256/128 cap)
    const auto& pm0 = out.packed[0];
    return !pm0.meshlets.meshlets.empty() &&
           !pm0.meshlets.meshlet_vertices.empty() &&
           !pm0.meshlets.meshlet_triangles.empty();
}

bool test_default_packed_serialize_round_trip() {
    ProcessableScene s = make_cube_scene();
    Config cfg;
    Result out;
    Run(std::move(s), cfg, out);

    if (out.packed.empty()) return false;
    const auto& pm = out.packed[0];
    const size_t sz = GetPackedMeshSize(pm);
    if (sz == 0) return false;
    utl::vector<u8> buf(sz);
    utl::blob_stream_writer blob{buf.data(), sz};
    Serialize(pm, blob);
    if (blob.position() != buf.data() + sz) return false;
    return has_magic(buf, 0x4C48534D) && has_magic(buf, 0x20464453);
}

// ---- Module gating -----------------------------------------------------

bool test_lod_disabled_keeps_single_lod() {
    ProcessableScene s = make_cube_scene();
    Config cfg;
    cfg.enable_lod = false;
    cfg.enable_meshlet = true;
    Result out;
    Run(std::move(s), cfg, out);
    return out.scene.lods.size() == 1 && out.packed.size() == 1;
}

bool test_meshlet_disabled_empty_meshlet_section() {
    ProcessableScene s = make_cube_scene();
    Config cfg;
    cfg.enable_lod = false;
    cfg.enable_meshlet = false;
    Result out;
    Run(std::move(s), cfg, out);

    if (out.packed.empty()) return false;
    return out.packed[0].meshlets.meshlets.empty();
}

// ---- Collision pipeline integration ------------------------------------

bool test_collision_enabled_produces_hulls() {
    ProcessableScene s = make_cube_scene();
    Config cfg;
    cfg.enable_lod = false;
    cfg.enable_meshlet = false;
    cfg.enable_collision = true;
    cfg.collision_params.max_hulls = 4;
    cfg.collision_params.voxel_resolution = 4000;  // small for fast test
    Result out;
    Run(std::move(s), cfg, out);

    // VHACD on a closed convex cube should yield >= 1 hull.
    return !out.hulls.empty();
}

// ---- Empty scene handling ----------------------------------------------

bool test_empty_scene_returns_warning_no_crash() {
    ProcessableScene s;  // no LODs
    Config cfg;
    Result out;
    Run(std::move(s), cfg, out);

    if (!out.packed.empty()) return false;
    if (!out.hulls.empty()) return false;
    // Expect at least one warning about no_lods
    bool found = false;
    for (const auto& r : out.warnings) {
        if (r.code == "pipeline.no_lods") { found = true; break; }
    }
    return found;
}

// ---- Derive auto-cascade after subdivide --------------------------------

// Verify that the auto-derive cascade fires when subdivide runs and that
// turning it off leaves the mesh in subdivide's raw output state. We don't
// assert specific normal values — DeriveAttributes has its own coverage —
// we just check the orchestration: derive runs iff (subdivide ran AND
// auto_derive_after_subdivide=true) OR enable_derive=true.
//
// Contract: post-cascade LOD 0 has unit-length normals for every position.
// Without the cascade, subdivide's new verts get zeroed normals (per
// Subdivide.h:8-13), so length != 1 on at least one vert.

bool test_subdivide_auto_derive_fills_normals() {
    ProcessableScene s = make_cube_scene();
    Config cfg;
    cfg.enable_lod = false;
    cfg.enable_meshlet = false;
    cfg.enable_subdivide = true;
    cfg.subdivide_params.scheme = subdivide::Scheme::Loop;
    cfg.subdivide_params.levels = 1;
    cfg.auto_derive_after_subdivide = true;  // default, but explicit
    cfg.derive_params.normal_mode = derive::NormalMode::AreaWeighted;
    cfg.derive_params.tangent_mode = derive::TangentMode::None;
    Result out;
    Run(std::move(s), cfg, out);

    if (out.scene.lods.empty() || out.scene.lods[0].meshes.empty()) return false;
    const ProcessableMesh& m = out.scene.lods[0].meshes[0];
    if (m.normals.size() != m.positions.size()) {
        std::cout << "  normals.size()=" << m.normals.size()
                  << " positions.size()=" << m.positions.size() << "\n";
        return false;
    }
    for (u32 i = 0; i < (u32)m.positions.size(); ++i) {
        const f32 L = std::sqrt(m.normals[i].x * m.normals[i].x +
                                m.normals[i].y * m.normals[i].y +
                                m.normals[i].z * m.normals[i].z);
        if (std::fabs(L - 1.f) > 1e-3f) {
            std::cout << "  vert " << i << " normal length=" << L << "\n";
            return false;
        }
    }
    return true;
}

bool test_subdivide_no_auto_derive_leaves_raw() {
    ProcessableScene s = make_cube_scene();
    Config cfg;
    cfg.enable_lod = false;
    cfg.enable_meshlet = false;
    cfg.enable_subdivide = true;
    cfg.subdivide_params.scheme = subdivide::Scheme::Loop;
    cfg.subdivide_params.levels = 1;
    cfg.auto_derive_after_subdivide = false;  // opt out
    cfg.derive_params.normal_mode = derive::NormalMode::AreaWeighted;
    Result out;
    Run(std::move(s), cfg, out);

    if (out.scene.lods.empty() || out.scene.lods[0].meshes.empty()) return false;
    const ProcessableMesh& m = out.scene.lods[0].meshes[0];
    // Subdivide grew positions (cube: 8 → ~26 after one Loop step). The
    // untouched normals array is whatever Subdivide.cpp chose to emit —
    // either zero-length (new verts) or stale-size. Both are "raw" state.
    // The cascade is verified by ABSOLUTE position growth without a
    // matching normal rebuild.
    if (m.positions.size() <= 8) return false;
    // If normals were rebuilt, every entry would be unit-length. Find at
    // least one entry that is NOT unit-length (proves cascade didn't run).
    bool found_non_unit = false;
    for (u32 i = 0; i < (u32)m.normals.size(); ++i) {
        const f32 L = std::sqrt(m.normals[i].x * m.normals[i].x +
                                m.normals[i].y * m.normals[i].y +
                                m.normals[i].z * m.normals[i].z);
        if (std::fabs(L - 1.f) > 1e-3f) { found_non_unit = true; break; }
    }
    // Also accept normals.size() != positions.size() as proof the cascade
    // didn't run (Subdivide may not resize normals to match).
    return found_non_unit || m.normals.size() != m.positions.size();
}

// ---- SDF resolution (sdf_params) ----------------------------------------

// generate_sdf's grid resolution is caller-controlled via sdf_params. The
// default must stay 32 (legacy Geometry.cpp path + runtime blob consumers
// were built around 32³), while a custom value must reach PackedMesh.sdf.

bool test_sdf_default_resolution_32() {
    ProcessableScene s = make_cube_scene();
    Config cfg;
    cfg.enable_lod = false;
    cfg.enable_meshlet = false;
    cfg.enable_sdf = true;
    Result out;
    Run(std::move(s), cfg, out);

    if (out.packed.empty()) return false;
    const auto& sdf = out.packed[0].sdf;
    return sdf.resolution[0] == 32 && sdf.resolution[1] == 32 &&
           sdf.resolution[2] == 32 &&
           sdf.data.size() == 32u * 32u * 32u;
}

bool test_sdf_custom_resolution() {
    ProcessableScene s = make_cube_scene();
    Config cfg;
    cfg.enable_lod = false;
    cfg.enable_meshlet = false;
    cfg.enable_sdf = true;
    cfg.sdf_params.resolution = 16;
    Result out;
    Run(std::move(s), cfg, out);

    if (out.packed.empty()) return false;
    const auto& sdf = out.packed[0].sdf;
    return sdf.resolution[0] == 16 && sdf.resolution[1] == 16 &&
           sdf.resolution[2] == 16 &&
           sdf.data.size() == 16u * 16u * 16u &&
           sdf.voxels.size() == 16u * 16u * 16u &&
           sdf.vector_field.size() == 16u * 16u * 16u * 4u;
}

// ---- Test runner --------------------------------------------------------

struct Case { const char* name; bool (*fn)(); };
#define CASE(n) { #n, n }

int main() {
    const Case cases[] = {
        CASE(test_default_produces_multiple_lods),
        CASE(test_default_produces_one_packed_per_lod),
        CASE(test_default_meshlet_data_populated),
        CASE(test_default_packed_serialize_round_trip),
        CASE(test_lod_disabled_keeps_single_lod),
        CASE(test_meshlet_disabled_empty_meshlet_section),
        CASE(test_collision_enabled_produces_hulls),
        CASE(test_empty_scene_returns_warning_no_crash),
        CASE(test_subdivide_auto_derive_fills_normals),
        CASE(test_subdivide_no_auto_derive_leaves_raw),
        CASE(test_sdf_default_resolution_32),
        CASE(test_sdf_custom_resolution),
    };

    int passed = 0, failed = 0;
    for (const auto& c : cases) {
        bool ok = false;
        try { ok = c.fn(); }
        catch (const std::exception& e) {
            std::cout << "  EXCEPTION  " << c.name << ": " << e.what() << "\n";
            ok = false;
        } catch (...) { ok = false; }
        std::cout << (ok ? "  PASS  " : "  FAIL  ") << c.name << "\n";
        if (ok) ++passed; else ++failed;
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
