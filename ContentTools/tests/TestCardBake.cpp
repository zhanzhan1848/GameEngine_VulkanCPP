// T4.2 unit tests for the offline card bake (ContentTools/common/CardBake).
//
// Chain under test: generate_sdf (shell occupancy) → card_bake::bake on a
// synthetic 4×0.05×1 thin plate (closed box, 12 triangles):
//   - thin ±X views are culled by the 5% rule (same rule as the runtime
//     fallback, so baked/fallback semantics stay compatible)
//   - per-direction projections are tight: card extents recover the plate
//     footprint within ~1 voxel, far below the sphere-derived cube AABB the
//     runtime fallback uses
//   - resolution hint = tight projection × 64 texels/unit, clamped [16,256]
//   - surfels: axis-aligned unit normals, positions on the plate
//   - open single-sided geometry (a bare quad — Sponza-style architecture)
//     still bakes planar cards from the shell
//   - degenerate grids (null voxels, tiny resolution) yield no cards
//   - PackedMesh card_bake survives a Serialize → WalkSceneBlob round trip

#include "common/CardBake.h"
#include "common/SDF.h"
#include "Geometry.h"
#include "pipeline/SceneBlobWriter.h"
#include "pipeline/SceneBlobReader.h"
#include "common/PipelineTypes.h"
#include "common/ErrorReport.h"
#include "../Engine/Utilities/IOStream.h"  // utl::blob_stream_writer

#include <cmath>
#include <cstring>
#include <iostream>

using namespace primal;
using namespace primal::tools;
using namespace primal::tools::card_bake;
using namespace primal::tools::pipeline;

namespace {

constexpr u32 kMagicCdbk = 0x4B424443u;
constexpr u32 kBakeRes = 64;

#define CASE(n) { #n, n }

u32 read_u32(const u8* p) { u32 v; std::memcpy(&v, p, 4); return v; }

// Closed axis-aligned box [−hx,hx]×[−hy,hy]×[−hz,hz] (8 verts, 12 tris) or an
// open quad when quad_only — run through generate_sdf and bake its shell.
struct Fixture {
    mesh m;
    data baked;
    bool ok = false;
};

Fixture bake_shape(f32 hx, f32 hy, f32 hz, bool quad_only = false) {
    Fixture f;
    if (quad_only) {
        f.m.vertices.resize(4);
        f.m.vertices[0].position = math::v3{-hx, 0.f, -hz};
        f.m.vertices[1].position = math::v3{ hx, 0.f, -hz};
        f.m.vertices[2].position = math::v3{ hx, 0.f,  hz};
        f.m.vertices[3].position = math::v3{-hx, 0.f,  hz};
        const u32 tris[6] = {0,1,2, 0,2,3};
        for (u32 i : tris) f.m.indices.push_back(i);
    } else {
        f.m.vertices.resize(8);
        const math::v3 corners[8] = {
            {-hx,-hy,-hz}, { hx,-hy,-hz}, { hx, hy,-hz}, {-hx, hy,-hz},
            {-hx,-hy, hz}, { hx,-hy, hz}, { hx, hy, hz}, {-hx, hy, hz},
        };
        for (u32 i = 0; i < 8; ++i) f.m.vertices[i].position = corners[i];
        const u32 tris[36] = {
            0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,5, 0,5,1,
            2,6,7, 2,7,3,  0,3,7, 0,7,4,  1,5,6, 1,6,2,
        };
        for (u32 i : tris) f.m.indices.push_back(i);
    }

    generate_sdf(f.m, kBakeRes);
    const mesh::sdf_data& sdf = f.m.sdf;
    if (sdf.voxels.size() != (size_t)sdf.resolution[0] * sdf.resolution[1] * sdf.resolution[2])
        return f;

    OccupancyGrid grid;
    grid.voxels = sdf.voxels.data();
    grid.resolution = sdf.resolution[0];
    grid.grid_min = math::v3{sdf.bounds_min[0], sdf.bounds_min[1], sdf.bounds_min[2]};
    grid.voxel_size = math::v3{
        (sdf.bounds_max[0] - sdf.bounds_min[0]) / (f32)grid.resolution,
        (sdf.bounds_max[1] - sdf.bounds_min[1]) / (f32)grid.resolution,
        (sdf.bounds_max[2] - sdf.bounds_min[2]) / (f32)grid.resolution};
    Params p{};
    bake(grid, p, f.baked);
    f.ok = true;
    return f;
}

u32 axis_of(const card& c) { return c.axis_direction & 0xFF; }

// ---- thin plate: thin-direction culling ---------------------------------

bool test_plate_thin_direction_culled() {
    // 4×0.05×1: the ±X view sees the thin cross-section; the 5%-of-max rule
    // (same rule as the runtime fallback) must cull it. Long axis amplifies
    // the max projection so the cull sits at a comfortable margin.
    Fixture f = bake_shape(2.0f, 0.025f, 0.5f);
    if (!f.ok || f.baked.cards.empty()) return false;

    u32 axis_count[3] = {0, 0, 0};
    for (const card& c : f.baked.cards) {
        if (axis_of(c) > 2) return false;
        ++axis_count[axis_of(c)];
    }
    if (axis_count[0] != 0) return false;   // thin direction culled
    if (axis_count[1] != 2) return false;   // ±Y kept (4×1 face)
    if (axis_count[2] != 2) return false;   // ±Z kept (4×0.05 face)
    return f.baked.cards.size() == 4;
}

// ---- thin plate: tight projections ---------------------------------------

bool test_plate_projection_tight() {
    Fixture f = bake_shape(2.0f, 0.025f, 0.5f);
    if (!f.ok) return false;

    // Runtime fallback for this plate derives a CUBE from the bounding
    // sphere radius |(2, 0.025, 0.5)| ≈ 2.06 → 4.12² projections.
    const f32 sphere_r = std::sqrt(2.0f * 2.0f + 0.025f * 0.025f + 0.5f * 0.5f);

    for (const card& c : f.baked.cards) {
        if (axis_of(c) != 1) continue;   // ±Y cards: plane = X×Z
        // Plane sides recover the plate footprint within ~1 voxel (SDF grid
        // is 64³ over the 20%-padded bounds → step.x ≈ 0.0875).
        if (c.extent[0] < 1.95f || c.extent[0] > 2.45f) return false;
        if (c.extent[2] < 0.45f || c.extent[2] > 0.75f) return false;
        // Depth extent hugs the thin axis (fallback would carry ~2.06).
        if (c.extent[1] > 0.13f) return false;
        // Tight projection area well below the fallback cube projection.
        const f32 baked_area = c.extent[0] * c.extent[2] * 4.0f;
        const f32 fallback_area = sphere_r * sphere_r * 4.0f;
        if (baked_area >= fallback_area * 0.6f) return false;
        // Center ≈ plate center (grid padding cancels in the mapping).
        if (std::abs(c.center[0]) > 0.15f) return false;
        if (std::abs(c.center[2]) > 0.15f) return false;
        if (std::abs(c.center[1]) > 0.05f) return false;
        return true;
    }
    return false;
}

// ---- resolution hint formula ----------------------------------------------

bool test_resolution_hint_formula() {
    Fixture f = bake_shape(2.0f, 0.025f, 0.5f);
    if (!f.ok) return false;
    for (const card& c : f.baked.cards) {
        const u32 u = (axis_of(c) == 0) ? 1u : 0u;
        const u32 v = (axis_of(c) == 2) ? 1u : 2u;
        const f32 proj_size = 2.0f * std::max(c.extent[u], c.extent[v]);
        const u32 expect = (u32)std::min(256.f, std::max(16.f, proj_size * 64.f));
        if (c.resolution != expect) return false;
        if (c.resolution < 16u || c.resolution > 256u) return false;
    }
    return true;
}

// ---- surfels ---------------------------------------------------------------

bool test_surfels_on_plate() {
    Fixture f = bake_shape(2.0f, 0.025f, 0.5f);
    if (!f.ok || f.baked.surfels.empty()) return false;

    for (const surfel& s : f.baked.surfels) {
        // Unit axis-aligned normal.
        const f32 len = std::sqrt(s.normal[0]*s.normal[0] + s.normal[1]*s.normal[1] + s.normal[2]*s.normal[2]);
        if (std::abs(len - 1.0f) > 1e-4f) return false;
        const bool axis_aligned =
            (s.normal[0] != 0.f) + (s.normal[1] != 0.f) + (s.normal[2] != 0.f) == 1;
        if (!axis_aligned) return false;
        // Positions within the plate bounds (plus shell-band slack).
        if (std::abs(s.pos[0]) > 2.3f || std::abs(s.pos[1]) > 0.2f || std::abs(s.pos[2]) > 0.7f) return false;
        if (!(s.area > 0.f)) return false;
    }
    return true;
}

// ---- open single-sided geometry bakes planar cards -------------------------

bool test_open_quad_bakes_planar_cards() {
    // Sponza-style architecture is single-sided: a solid (winding) test
    // would find nothing, but the SDF shell marks the surface either way.
    Fixture f = bake_shape(1.0f, 0.f, 0.5f, /*quad_only=*/true);
    if (!f.ok || f.baked.cards.empty()) return false;

    for (const card& c : f.baked.cards) {
        if (axis_of(c) != 1) continue;   // ±Y cards look straight at the quad
        if (c.extent[0] < 0.9f || c.extent[0] > 1.3f) return false;
        if (c.extent[2] < 0.4f || c.extent[2] > 0.8f) return false;
        // Depth extent stays a thin shell around the plane.
        if (c.extent[1] > 0.08f) return false;
        return true;
    }
    return false;
}

// ---- degenerate grids -------------------------------------------------------

bool test_degenerate_grid_yields_no_cards() {
    Params p{};
    data out;

    OccupancyGrid null_grid;
    bake(null_grid, p, out);
    if (!out.cards.empty()) return false;

    u8 tiny[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    OccupancyGrid small_grid;
    small_grid.voxels = tiny;
    small_grid.resolution = 2;   // < 4 → rejected
    small_grid.grid_min = math::v3{0.f, 0.f, 0.f};
    small_grid.voxel_size = math::v3{1.f, 1.f, 1.f};
    bake(small_grid, p, out);
    return out.cards.empty();
}

// ---- blob round trip through SceneBlobWriter / WalkSceneBlob ---------------

bool test_card_bake_blob_round_trip() {
    Fixture f = bake_shape(2.0f, 0.025f, 0.5f);
    if (!f.ok || f.baked.cards.empty()) return false;

    PackedMesh pm;
    pm.name = "plate";
    pm.lod_id = 0;
    pm.material_idx = 0;
    pm.elements_type = elements::elements_type::static_normal;
    pm.position_buffer.resize(12 * 8);
    for (u32 i = 0; i < 8; ++i) {
        const math::v3& p = f.m.vertices[i].position;
        f32 xyz[3] = {p.x, p.y, p.z};
        std::memcpy(pm.position_buffer.data() + i * 12, xyz, 12);
    }
    pm.element_buffer.resize((u32)sizeof(elements::static_normal) * 8, 0);
    for (u32 i : {0u,1u,2u, 0u,2u,3u, 4u,5u,6u, 4u,6u,7u}) pm.indices.push_back(i);
    pm.card_bake.cards = f.baked.cards;
    pm.card_bake.surfels = f.baked.surfels;

    utl::vector<PackedMesh> meshes;
    meshes.push_back(pm);
    const size_t sz = GetSceneSize("s", {}, meshes);
    utl::vector<u8> buf(sz);
    utl::blob_stream_writer blob{buf.data(), sz};
    SerializeScene("s", {}, meshes, blob);

    scene_data sd{};
    sd.buffer = buf.data();
    sd.buffer_size = (u32)sz;

    u32 visited = 0;
    bool ok = true;
    primal::utl::vector<ErrorReport> errs;
    WalkSceneBlob(sd, [&](u32, const PackedMeshView& view) {
        ++visited;
        if (view.card_bake_card_count != (u32)f.baked.cards.size()) { ok = false; return; }
        if (view.card_bake_surfel_count != (u32)f.baked.surfels.size()) { ok = false; return; }
        for (u32 i = 0; i < view.card_bake_card_count; ++i) {
            if (std::memcmp(&view.card_bake_cards[i], &f.baked.cards[i], sizeof(card)) != 0) { ok = false; return; }
        }
        // canonical elements_type must not carry the flag bits
        if (((u32)view.elements_type & 0x70u) != 0) ok = false;
    }, errs);
    return ok && visited == 1;
}

// ---- meshes without bake data keep the legacy byte layout -------------------

bool test_no_card_bake_section_when_empty() {
    PackedMesh pm;
    pm.name = "plain";
    pm.lod_id = 0;
    pm.material_idx = 0;
    pm.elements_type = elements::elements_type::static_normal;
    pm.position_buffer.resize(12 * 4);
    pm.element_buffer.resize((u32)sizeof(elements::static_normal) * 4, 0);
    for (u32 i : {0u,1u,2u, 0u,2u,3u}) pm.indices.push_back(i);

    utl::vector<PackedMesh> meshes;
    meshes.push_back(pm);
    const size_t sz = GetSceneSize("s", {}, meshes);
    utl::vector<u8> buf(sz);
    utl::blob_stream_writer blob{buf.data(), sz};
    SerializeScene("s", {}, meshes, blob);

    // No "CDBK" magic anywhere in the serialized bytes.
    for (size_t i = 0; i + 4 <= sz; ++i) {
        if (read_u32(buf.data() + i) == kMagicCdbk) return false;
    }

    scene_data sd{};
    sd.buffer = buf.data();
    sd.buffer_size = (u32)sz;
    u32 visited = 0;
    primal::utl::vector<ErrorReport> errs;
    WalkSceneBlob(sd, [&](u32, const PackedMeshView& view) {
        ++visited;
        if (view.card_bake_card_count != 0 || view.card_bake_surfel_count != 0) { /* legacy blob: no bake */ }
    }, errs);
    return visited == 1;
}

}  // namespace

int main() {
    struct Case { const char* name; bool(*fn)(); };
    const Case cases[] = {
        CASE(test_plate_thin_direction_culled),
        CASE(test_plate_projection_tight),
        CASE(test_resolution_hint_formula),
        CASE(test_surfels_on_plate),
        CASE(test_open_quad_bakes_planar_cards),
        CASE(test_degenerate_grid_yields_no_cards),
        CASE(test_card_bake_blob_round_trip),
        CASE(test_no_card_bake_section_when_empty),
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
