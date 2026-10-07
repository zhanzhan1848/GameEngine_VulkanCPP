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
//
// v2 (2026-09-19 SC 卡片 Lumen 对齐 Phase 1): the tests above pin bake_v1
// (rollback path); v2 adds coverage-driven surface-hugging cards:
//   - concave alcove + floating inner cube: inner surfaces get their own
//     local cards (v1's centered views cannot represent them), greedy stops
//     at coverage ≥ 85% with ≤ 12 cards (UE Max Lumen Mesh Cards)
//   - thin plate: thin-direction local cards survive in v2 (v1 culls them)
//   - params.max_cards caps the greedy
//   - legacy "CDBK" (v1) sections still parse; new sections are "CDB2" v2

#include "common/CardBake.h"
#include "common/CardSurfaceBake.h"
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
    OccupancyGrid grid;   // retained so v2 variants can re-bake the shell
    data baked;
    bool ok = false;
};

// Append a closed box [mn,mx] to the mesh (8 verts, 12 tris).
void add_box(mesh& m, f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1) {
    const u32 base = (u32)m.vertices.size();
    const f32 xs[2] = {x0, x1}, ys[2] = {y0, y1}, zs[2] = {z0, z1};
    for (u32 i = 0; i < 8; ++i) {
        vertex v{};
        v.position = math::v3{xs[i & 1], ys[(i >> 1) & 1], zs[(i >> 2) & 1]};
        m.vertices.push_back(v);
    }
    const u32 tris[36] = {
        0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,5, 0,5,1,
        2,6,7, 2,7,3,  0,3,7, 0,7,4,  1,5,6, 1,6,2,
    };
    for (u32 t : tris) m.indices.push_back(base + t);
}

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
        add_box(f.m, -hx, -hy, -hz, hx, hy, hz);
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
    bake_v1(grid, p, f.baked);
    f.grid = grid;
    f.ok = true;
    return f;
}

// Concave fixture: two stacked boxes ("snowman") — a 1×1×1 base with a
// 0.6³ cube flush on top. The top cube's side/underside faces sit inside a
// crevice that v1's centered whole-bbox views cannot represent (nearest
// geometry wins the depth test); v2 must give them local cards.
// Returns the raw bake inputs (caller picks v1 or v2). Owns the mesh — the
// grid views its voxel buffer, so the mesh must outlive every bake call.
struct RawBake {
    mesh m;
    OccupancyGrid grid;
    bool ok = false;
};

RawBake make_nested_boxes_grid() {
    RawBake r;
    // Cubic 1.4³ bbox → isotropic voxel steps (anisotropic shells fragment).
    add_box(r.m, -0.7f, 0.f,   -0.7f,  0.7f, 0.7f,  0.7f);   // base box
    add_box(r.m, -0.4f, 0.7f,  -0.4f,  0.4f, 1.4f,  0.4f);   // top box

    generate_sdf(r.m, kBakeRes);
    const mesh::sdf_data& sdf = r.m.sdf;
    if (sdf.voxels.size() != (size_t)sdf.resolution[0] * sdf.resolution[1] * sdf.resolution[2])
        return r;
    r.grid.voxels = sdf.voxels.data();
    r.grid.resolution = sdf.resolution[0];
    r.grid.grid_min.x = sdf.bounds_min[0];
    r.grid.grid_min.y = sdf.bounds_min[1];
    r.grid.grid_min.z = sdf.bounds_min[2];
    r.grid.voxel_size.x = (sdf.bounds_max[0] - sdf.bounds_min[0]) / (f32)r.grid.resolution;
    r.grid.voxel_size.y = (sdf.bounds_max[1] - sdf.bounds_min[1]) / (f32)r.grid.resolution;
    r.grid.voxel_size.z = (sdf.bounds_max[2] - sdf.bounds_min[2]) / (f32)r.grid.resolution;
    r.ok = true;
    return r;
}

// "Inner-local card" predicate: center near the top box (0,1.05,0) and a
// small max extent. The top box's faces sit ≤ 0.47 from its center with
// half-extents ≤ ~0.44; the base box's faces either sit ≥ 0.82−0.7 ≈ 0.82
// away (sides) or span the full 1.4 footprint (ring/bottom, half-extent
// ≈ 0.74); v1's centered whole-bbox views have extents ≈ 0.7+ — none
// satisfy both halves of the predicate.
bool is_inner_local_card(const card& c) {
    const f32 dx = c.center[0] - 0.f, dy = c.center[1] - 1.05f, dz = c.center[2] - 0.f;
    const f32 dist2 = dx * dx + dy * dy + dz * dz;
    const f32 max_ext = std::max(c.extent[0], std::max(c.extent[1], c.extent[2]));
    return dist2 <= 0.55f * 0.55f && max_ext < 0.5f;
}

u32 axis_of(const card& c) { return c.axis_direction & 0xFF; }

void dump_errors(const primal::utl::vector<ErrorReport>& errs) {
    for (const auto& e : errs)
        std::cout << "    [" << (int)e.severity << "] " << e.message << "\n";
}

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
        // New serialization is v2 ("CDB2" + version u32).
        if (view.card_bake_version != 2) ok = false;
        for (u32 i = 0; i < view.card_bake_card_count; ++i) {
            if (std::memcmp(&view.card_bake_cards[i], &f.baked.cards[i], sizeof(card)) != 0) { ok = false; return; }
        }
        // canonical elements_type must not carry the flag bits
        if (((u32)view.elements_type & 0x70u) != 0) ok = false;
    }, errs);
    if (!(ok && visited == 1)) dump_errors(errs);
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

// ---- v2: concave alcove gets inner-local cards + coverage guarantee --------

bool test_v2_alcove_inner_local_cards() {
    // Concavity (floating cube inside an alcove) is invisible to v1's
    // centered whole-bbox views. v2's coverage-driven local cards must give
    // the cube's surfaces their own representation, stop at ≥85% coverage,
    // and respect the per-mesh card cap (UE Max Lumen Mesh Cards = 12).
    RawBake r = make_nested_boxes_grid();
    if (!r.ok) return false;

    // The ragged SDF shell inflates the base box's capturable surface well
    // past the top cube's share, so the default 85% early-stop would always
    // finish before the cube's faces compete. Drive the greedy to gain
    // exhaustion and assert the cube got its own local cards anyway.
    Params p{};
    p.coverage_threshold = 1.0f;
    data out;
    bake_v2(r.grid, p, out);
    if (out.cards.empty()) { std::cout << "    no cards\n"; return false; }
    if (out.cards.size() > 12u) {
        std::cout << "    cards=" << out.cards.size() << " exceeds cap\n";
        return false;
    }
    // (Coverage is measured against capturable area — see bake_v2 notes. The
    // coverage-threshold early-stop itself is pinned by the plate test; here
    // the greedy runs to the card cap, whose 12 slots fit all 11 real
    // surface comps with margin.)

    u32 inner_local = 0;
    for (const card& c : out.cards) {
        if (is_inner_local_card(c)) ++inner_local;
        const f32 dx = c.center[0], dy = c.center[1] - 1.05f, dz = c.center[2];
        std::cout << "    card axis=" << axis_of(c)
                  << " dir=" << ((c.axis_direction >> 8) & 0xFF)
                  << " center=(" << c.center[0] << "," << c.center[1] << "," << c.center[2] << ")"
                  << " ext=(" << c.extent[0] << "," << c.extent[1] << "," << c.extent[2] << ")"
                  << " dist=" << std::sqrt(dx*dx + dy*dy + dz*dz) << "\n";
    }
    if (inner_local < 1) std::cout << "    inner_local=" << inner_local
                                   << " of " << out.cards.size() << "\n";
    return inner_local >= 1;                            // cube got its own cards
}

// ---- v2 vs v1 contrast on the same concave fixture -------------------------

bool test_v1_alcove_has_no_inner_local_cards() {
    RawBake r = make_nested_boxes_grid();
    if (!r.ok) return false;

    Params p{};
    data out;
    bake_v1(r.grid, p, out);
    if (out.cards.empty()) return false;
    // v1 centers every card on the whole-bbox center with whole-bbox
    // extents (≥ ~0.5) — no card can pass the inner-local predicate.
    for (const card& c : out.cards)
        if (is_inner_local_card(c)) return false;
    return true;
}

// ---- v2: thin-plate thin-direction views survive as local cards ------------

bool test_v2_plate_keeps_thin_views() {
    // v1 culls the ±X views of a 4×0.05×1 plate by the 5% rule. v2 keeps
    // surface-hugging local cards there (the end caps). The default 85%
    // coverage target stops the greedy after the big faces, so the test
    // raises the coverage threshold to force the thin views to be picked.
    Fixture f = bake_shape(2.0f, 0.025f, 0.5f);
    if (!f.ok) return false;

    Params p{};
    p.coverage_threshold = 0.99f;
    data out;
    bake_v2(f.grid, p, out);
    if (out.cards.size() < 5u || out.cards.size() > 12u) {
        std::cout << "    plate cards=" << out.cards.size()
                  << " coverage=" << out.coverage << "\n";
        return false;
    }
    if (out.coverage < 0.95f) {
        std::cout << "    plate coverage=" << out.coverage
                  << " cards=" << out.cards.size() << "\n";
        return false;
    }
    // The two big ±Y face cards must be there; the ±X end caps (the thin
    // views v1 drops) must survive as local cards.
    u32 axis1 = 0, axis0 = 0;
    for (const card& c : out.cards) {
        if (axis_of(c) == 1) ++axis1;
        if (axis_of(c) == 0) ++axis0;
    }
    return axis1 >= 2 && axis0 >= 1;
}

// ---- v2: params.max_cards caps the greedy ----------------------------------

bool test_v2_max_cards_cap() {
    RawBake r = make_nested_boxes_grid();
    if (!r.ok) return false;

    Params p{};
    p.max_cards = 3;
    data out;
    bake_v2(r.grid, p, out);
    if (out.cards.size() > 3u) return false;
    // With the cap far below the component count the coverage target must
    // NOT be reachable — a full-coverage claim would mean the cap is ignored.
    return out.coverage < 1.0f;
}

// ---- legacy v1 "CDBK" section still parses (old blobs) ----------------------

bool test_v1_card_bake_section_parses() {
    // Hand-assemble a one-mesh scene whose card-bake section uses the LEGACY
    // layout (magic | card_count | cards | surfel_count | surfels, no
    // version). WalkSceneBlob must read it as card_bake_version == 1.
    const card cards[2] = {};
    u8 buf[4096];
    utl::blob_stream_writer w{buf, sizeof(buf)};
    auto wu32 = [&](u32 v) { w.write(v); };
    auto wstr = [&](const char* s) { wu32((u32)std::strlen(s)); w.write(s, std::strlen(s)); };

    wstr("s");                       // scene name
    wu32(0);                         // material count
    wu32(1);                         // lod group count
    wu32(0);                         // lod name size
    wu32(1);                         // mesh count
    // mesh header
    wstr("legacy");
    wu32(0);                         // lod id
    wu32(0);                         // material idx
    wu32((u32)sizeof(elements::static_normal));  // element size
    wu32((u32)elements::elements_type::static_normal |
         (u32)elements::elements_type::card_bake_flag);
    wu32(4);                         // num vertices
    wu32(4);                         // index size (u32 here for simplicity)
    wu32(6);                         // num indices
    w.write(0.f);                    // lod threshold
    // position buffer (12 B × 4)
    w.write((const u8*)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                     "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 48);
    // element buffer
    for (u32 i = 0; i < 4 * sizeof(elements::static_normal); ++i) w.write((u8)0);
    // indices (6 × u32)
    for (u32 i = 0; i < 6; ++i) wu32(i);
    // MSHL
    wu32(0x4C48534D);
    wu32(0); wu32(0); wu32(0);       // meshlets / vertices / triangles
    // SDF (empty)
    wu32(0x20464453);
    u32 res[3] = {0, 0, 0};
    w.write((const u8*)res, sizeof(res));
    f32 bounds[6] = {0, 0, 0, 0, 0, 0};
    w.write((const u8*)bounds, sizeof(bounds));
    wu32(0); wu32(0); wu32(0);       // data / voxels / vector field sizes
    // LEGACY CDBK section (v1: no version u32)
    wu32(kMagicCdbk);
    wu32(2);
    w.write((const u8*)cards, sizeof(cards));
    wu32(0);                         // surfel count

    scene_data sd{};
    sd.buffer = buf;
    sd.buffer_size = (u32)w.offset();
    u32 visited = 0;
    bool ok = true;
    primal::utl::vector<ErrorReport> errs;
    WalkSceneBlob(sd, [&](u32, const PackedMeshView& view) {
        ++visited;
        if (view.card_bake_card_count != 2) { ok = false; return; }
        if (view.card_bake_version != 1) ok = false;
        if (std::memcmp(view.card_bake_cards, &cards[0], sizeof(cards)) != 0) ok = false;
    }, errs);
    if (!(ok && visited == 1)) dump_errors(errs);
    return ok && visited == 1;
}

// ============================================================================
// [SC per-mesh 计划 P1] CardSurfaceBake 单测：owner-only 表面页烘焙的覆盖、
// 深度/法线正确性、身份摘要稳定性与 sidecar 往返。
// ============================================================================

// 从烘焙好的 mesh 取平面视图（positions + u32 索引；elements 空 = tint 纯色）。
card_surface::MeshGeometryView geometry_view_of(const mesh& m,
                                                std::vector<f32>& pos_storage,
                                                std::vector<u32>& idx_storage)
{
    pos_storage.clear();
    idx_storage.clear();
    for (const vertex& v : m.vertices) {
        pos_storage.push_back(v.position.x);
        pos_storage.push_back(v.position.y);
        pos_storage.push_back(v.position.z);
    }
    idx_storage.assign(m.indices.begin(), m.indices.end());
    card_surface::MeshGeometryView g;
    g.positions = pos_storage.data();
    g.vertex_count = (u32)pos_storage.size() / 3;
    g.indices = reinterpret_cast<const u8*>(idx_storage.data());
    g.index_size = 4;
    g.index_count = (u32)idx_storage.size();
    g.elements = nullptr;
    g.element_stride = 0;
    return g;
}

// 轴对齐闭盒（正确三角形索引——既有 add_box 的 x0/x1 两面索引错误形成
// 对角面，SDF 壳不消费三角形所以未被既有测试暴露；表面烘焙忠实消费）。
mesh build_valid_box(f32 hx, f32 hy, f32 hz) {
    mesh m;
    const f32 xs[2] = {-hx, hx}, ys[2] = {-hy, hy}, zs[2] = {-hz, hz};
    for (u32 i = 0; i < 8; ++i) {
        vertex v{};
        v.position = math::v3{xs[i & 1], ys[(i >> 1) & 1], zs[(i >> 2) & 1]};
        m.vertices.push_back(v);
    }
    // 顶点：(i&1)=x, (i>>1)&1=y, (i>>2)&1=z；全部面外向绕序。
    const u32 tris[36] = {
        0,2,1, 0,3,2,    // z0（-Z 朝外）
        4,5,6, 4,6,7,    // z1（+Z 朝外）
        0,1,5, 0,5,4,    // y0（-Y 朝外）
        2,6,7, 2,7,3,    // y1（+Y 朝外）
        0,4,6, 0,6,2,    // x0（-X 朝外）
        1,3,7, 1,7,5,    // x1（+X 朝外）
    };
    for (u32 t : tris) m.indices.push_back(t);
    return m;
}

// 与 bake_shape 同管线（SDF 壳 → bake_v2 卡），但用正确三角化的网格。
Fixture surface_bake_fixture(f32 hx, f32 hy, f32 hz) {
    Fixture f;
    f.m = build_valid_box(hx, hy, hz);
    generate_sdf(f.m, kBakeRes);
    if (f.m.sdf.voxels.empty()) return f;
    f.grid.voxels = f.m.sdf.voxels.data();
    f.grid.resolution = f.m.sdf.resolution[0];
    f.grid.grid_min = {f.m.sdf.bounds_min[0], f.m.sdf.bounds_min[1], f.m.sdf.bounds_min[2]};
    f.grid.voxel_size = {(f.m.sdf.bounds_max[0] - f.m.sdf.bounds_min[0]) / f.grid.resolution,
                         (f.m.sdf.bounds_max[1] - f.m.sdf.bounds_min[1]) / f.grid.resolution,
                         (f.m.sdf.bounds_max[2] - f.m.sdf.bounds_min[2]) / f.grid.resolution};
    Params p{};
    bake_v2(f.grid, p, f.baked);
    f.ok = !f.baked.cards.empty();
    return f;
}

// f16 位型 → f32（页法线平面 = rg16f）。
f32 f16_to_f32(u16 h) {
    const u32 sign = (h & 0x8000u) << 16;
    const u32 exp = (h >> 10) & 0x1Fu;
    const u32 mant = h & 0x3FFu;
    u32 bits;
    if (exp == 0) bits = sign;
    else if (exp == 31) bits = sign | 0x7F800000u | (mant << 13);
    else bits = sign | ((exp - 15u + 127u) << 23) | (mant << 13);
    f32 out; std::memcpy(&out, &bits, 4); return out;
}

// oct 解码（与运行时 MathLib.glsl octDecode 同式），验证页法线用。
void oct_decode(f32 e0, f32 e1, f32 out[3]) {
    const f32 fx = e0 * 2.f - 1.f, fy = e1 * 2.f - 1.f;
    out[0] = fx; out[1] = fy; out[2] = 1.f - std::fabs(fx) - std::fabs(fy);
    const f32 t = std::max(0.f, -out[2]);
    out[0] += (out[0] >= 0.f) ? -t : t;
    out[1] += (out[1] >= 0.f) ? -t : t;
    const f32 len = std::sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]);
    if (len > 1e-8f) { out[0] /= len; out[1] /= len; out[2] /= len; }
}

bool test_surface_bake_box_coverage_depth_normal() {
    Fixture f = surface_bake_fixture(0.25f, 0.05f, 0.5f);
    if (!f.ok || f.baked.cards.empty()) return false;

    std::vector<f32> pos; std::vector<u32> idx;
    card_surface::MeshGeometryView geom = geometry_view_of(f.m, pos, idx);
    card_surface::RgbaImage tex{};   // 无纹理 → tint 纯色
    card_surface::BakeParams bp{};
    card_surface::MeshBakeResult res;
    if (!card_surface::bake_mesh(geom, f.baked, tex, bp, res)) return false;
    if (res.pages.size() != f.baked.cards.size()) return false;
    if (res.geometry_digest == 0 || res.layout_digest == 0) return false;

    for (size_t c = 0; c < f.baked.cards.size(); ++c) {
        const card& bc = f.baked.cards[c];
        const card_surface::CardPage& pg = res.pages[c];
        const u32 axis = bc.axis_direction & 0xFFu;
        const u32 dir = (bc.axis_direction >> 8) & 0xFFu;
        const u32 u_axis = (axis == 0) ? 1u : 0u;
        const u32 v_axis = (axis == 2) ? 1u : 2u;
        // 分辨率 = 运行时公式：max(e[u],e[v])·2·64 钳 [64,128] 页对齐 32。
        const u32 expect = [&]{
            const f32 proj = std::max(bc.extent[u_axis], bc.extent[v_axis]) * 2.f;
            u32 r = (u32)(proj * 64.f);
            r = std::max(r, 64u); r = std::min(r, 128u);
            return (r + 31u) / 32u * 32u;
        }();
        if (pg.resolution != expect) return false;
        if (pg.albedo.size() != size_t(pg.resolution) * pg.resolution * 4) return false;
        if (pg.depth.size() != size_t(pg.resolution) * pg.resolution) return false;

        const f32 t_max = 2.f * bc.extent[axis] + 0.5f;
        for (u32 t = 0; t < pg.resolution * pg.resolution; ++t) {
            const f32 d = pg.depth[t];
            const u8 a = pg.albedo[t * 4 + 3];
            if (a == 0) { if (d != -1.f || pg.triangle_ids[t]!=0xffffffffu) return false; continue; }
            if (d <= 0.05f || d > t_max) return false;      // capture 近/远平面约定
            // Independently reconstruct the point and check it lies on the ORIGINAL
            // indexed triangle, even after the bake BVH partitions its 12 faces.
            const u32 tri=pg.triangle_ids[t]; if(tri>=idx.size()/3) return false;
            double point[3]{bc.center[0],bc.center[1],bc.center[2]};
            point[u_axis]+=((double(t%pg.resolution)+.5)/pg.resolution-.5)*2*bc.extent[u_axis];
            point[v_axis]+=((double(t/pg.resolution)+.5)/pg.resolution-.5)*2*bc.extent[v_axis];
            point[axis]+=(dir?1:-1)*double(d);
            double e1[3],e2[3],v[3];
            for(u32 k=0;k<3;++k) {
                double origin=pos[idx[tri*3]*3+k];
                e1[k]=pos[idx[tri*3+1]*3+k]-origin;
                e2[k]=pos[idx[tri*3+2]*3+k]-origin; v[k]=point[k]-origin;
            }
            auto dot=[](const double* a,const double* b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
            double aa=dot(e1,e1),bb=dot(e2,e2),ab=dot(e1,e2),av=dot(e1,v),bv=dot(e2,v);
            double det=aa*bb-ab*ab; if(det<=0) return false;
            double u=(bb*av-ab*bv)/det,w=(aa*bv-ab*av)/det;
            if(u < -2e-5 || w < -2e-5 || u+w>1+2e-5) return false;
            for(u32 k=0;k<3;++k) if(std::abs(v[k]-u*e1[k]-w*e2[k])>2e-5) return false;
            if (a != 255) return false;                      // 有效 = 全覆盖
            // albedo = tint 纯色（无纹理、scale 1）
            if (pg.albedo[t*4] != 255 || pg.albedo[t*4+1] != 255 || pg.albedo[t*4+2] != 255) return false;
            // 法线：轴对齐盒面 → oct 解码 ≈ 面法线（dir 分量 ≈ ±1）；
            // rg16f = 两枚 f16 位型（非 unorm）。
            const u16 hx = u16(pg.normal[t*4]) | (u16(pg.normal[t*4+1]) << 8);
            const u16 hy = u16(pg.normal[t*4+2]) | (u16(pg.normal[t*4+3]) << 8);
            const f32 enc0 = f16_to_f32(hx), enc1 = f16_to_f32(hy);
            f32 n[3]; oct_decode(enc0, enc1, n);
            // 正确不变量：命中面几何法线（外向）轴向对齐，且与卡朝向相反
            //（capture 的朝眼翻转变换在合成期做；dot(n, f) ≤ 0）。
            const f32 face = (dir > 0) ? 1.f : -1.f;
            if (std::abs(n[axis]) < 0.98f || n[axis] * face > 0.01f) return false;
        }
        // 首卡应有实际覆盖（防全空页静默通过）
        if (c == 0 && pg.valid_texels == 0) return false;
    }
    return true;
}

bool test_surface_bake_capture_far_contract() {
    Fixture f=surface_bake_fixture(.25f,.05f,.5f);
    if(!f.ok) return false;
    auto cards=f.baked; cards.cards.resize(1);
    auto& c=cards.cards[0]; c.axis_direction=256; c.center[0]=-2; c.center[1]=c.center[2]=0;
    c.extent[0]=.25f; c.extent[1]=.05f; c.extent[2]=.5f;
    std::vector<f32> pos;std::vector<u32> idx;
    auto geom=geometry_view_of(f.m,pos,idx);
    card_surface::BakeParams params; card_surface::RgbaImage texture;
    card_surface::MeshBakeResult clipped,extended;
    if(!card_surface::bake_mesh(geom,cards,texture,params,clipped) || clipped.pages[0].valid_texels) return false;
    params.depth_limit_scale=3;
    if(!card_surface::bake_mesh(geom,cards,texture,params,extended) || !extended.pages[0].valid_texels) return false;
    card_surface::ScsHeader header; header.flags=1;
    if(card_surface::write_sidecar("/tmp/sc-invalid-far-contract.scsurf",header,{extended})) return false;
    return true;
}

bool test_surface_bake_owner_isolation_foreign_triangle_invisible() {
    // owner 契约（§3.2）：bake 输入只有 owner mesh 的几何。构造两份视图——
    // owner 平面 + 一个"外来"三角面（卡前 0.2m 处），bake 用 owner 视图 →
    // 外来面不出现在页里；用合并视图 → 立即出现（对照组，证明谓词有效）。
    std::vector<f32> pos;
    std::vector<u32> idx;
    auto push_tri = [&](f32 ax, f32 ay, f32 az, f32 bx, f32 by, f32 bz, f32 cx, f32 cy, f32 cz) {
        const u32 base = (u32)pos.size() / 3;
        const f32 vs[3][3] = {{ax,ay,az},{bx,by,bz},{cx,cy,cz}};
        for (auto& v : vs) { pos.push_back(v[0]); pos.push_back(v[1]); pos.push_back(v[2]); }
        idx.push_back(base); idx.push_back(base+1); idx.push_back(base+2);
    };
    // owner：z=0 平面正方形（两三角）
    push_tri(-1,-1,0,  1,-1,0,  1,1,0);
    push_tri(-1,-1,0,  1,1,0,  -1,1,0);
    // 外来：z=+0.2 小三角（位于 owner 面与卡平面之间——首命中语义下
    // 外来面遮住 owner；owner-only 视图里它根本不存在）
    push_tri(-0.1f,-0.1f,0.2f,  0.1f,-0.1f,0.2f,  0.f,0.1f,0.2f);

    std::vector<u32> owner_idx(idx.begin(), idx.begin() + 6);

    card_bake::data cards;
    card c{};
    // 卡平面 z=+0.4，朝 -Z（dir=0）：合并视图首命中 = 外来面（0.2），
    // owner-only 视图首命中 = owner 平面（0.4）。
    c.center[0] = 0; c.center[1] = 0; c.center[2] = 0.4f;
    c.axis_direction = 2u;               // -Z 面卡
    c.extent[0] = 1; c.extent[1] = 1; c.extent[2] = 0.5f;
    c.resolution = 64;
    cards.cards.push_back(c);

    card_surface::MeshGeometryView owner{};
    owner.positions = pos.data(); owner.vertex_count = (u32)pos.size()/3;
    owner.indices = reinterpret_cast<const u8*>(owner_idx.data());
    owner.index_size = 4; owner.index_count = (u32)owner_idx.size();
    card_surface::RgbaImage tex{};
    card_surface::BakeParams bp{};
    card_surface::MeshBakeResult owner_res;
    if (!card_surface::bake_mesh(owner, cards, tex, bp, owner_res)) return false;

    card_surface::MeshGeometryView merged{};
    merged.positions = pos.data(); merged.vertex_count = (u32)pos.size()/3;
    merged.indices = reinterpret_cast<const u8*>(idx.data());
    merged.index_size = 4; merged.index_count = (u32)idx.size();
    card_surface::MeshBakeResult merged_res;
    if (!card_surface::bake_mesh(merged, cards, tex, bp, merged_res)) return false;

    // owner 页任何有效 texel 的 depth ≥ 0.3（只有 owner 平面）；merged 页
    // 存在 depth < 0.3 的 texel（外来面遮住 owner）。
    const card_surface::CardPage& op = owner_res.pages[0];
    const card_surface::CardPage& mp = merged_res.pages[0];
    bool owner_has_surface = false, merged_has_foreign = false;
    for (u32 t = 0; t < op.resolution * op.resolution; ++t) {
        if (op.albedo[t*4+3]) {
            owner_has_surface = true;
            if (op.depth[t] < 0.3f) return false;   // owner 页无外来深度
        }
        if (mp.albedo[t*4+3] && mp.depth[t] < 0.3f) merged_has_foreign = true;
    }
    return owner_has_surface && merged_has_foreign;
}

bool test_surface_bake_digests_stability() {
    Fixture f = surface_bake_fixture(0.25f, 0.05f, 0.5f);
    if (!f.ok) return false;
    std::vector<f32> pos; std::vector<u32> idx;
    card_surface::MeshGeometryView geom = geometry_view_of(f.m, pos, idx);
    card_surface::RgbaImage tex{};
    card_surface::BakeParams bp{};
    card_surface::MeshBakeResult a, b;
    if (!card_surface::bake_mesh(geom, f.baked, tex, bp, a)) return false;
    if (!card_surface::bake_mesh(geom, f.baked, tex, bp, b)) return false;
    if (a.geometry_digest != b.geometry_digest) return false;
    if (a.layout_digest != b.layout_digest) return false;

    // 几何变化 → geometry_digest 变（挪一个顶点 0.01）；布局不变。
    std::vector<f32> pos2 = pos;
    pos2[1] += 0.01f;
    card_surface::MeshGeometryView geom2 = geom;
    geom2.positions = pos2.data();
    card_surface::MeshBakeResult c;
    if (!card_surface::bake_mesh(geom2, f.baked, tex, bp, c)) return false;
    if (c.geometry_digest == a.geometry_digest) return false;
    if (c.layout_digest != a.layout_digest) return false;

    // 卡片布局变化 → layout_digest 变（翻转一张卡的朝向）；几何不变。
    card_bake::data cards2 = f.baked;
    cards2.cards[0].axis_direction ^= (1u << 8);
    card_surface::MeshBakeResult d;
    if (!card_surface::bake_mesh(geom, cards2, tex, bp, d)) return false;
    if (d.layout_digest == a.layout_digest) return false;
    if (d.geometry_digest != a.geometry_digest) return false;
    // UV/normal bytes are part of the geometry source, not just positions.
    if (geom.elements && geom.element_stride) {
        std::vector<u8> elements(geom.elements, geom.elements + geom.vertex_count * geom.element_stride);
        elements[16] ^= 1;
        auto changed = geom; changed.elements = elements.data();
        card_surface::MeshBakeResult e;
        if (!card_surface::bake_mesh(changed, f.baked, tex, bp, e) || e.geometry_digest == a.geometry_digest) return false;
    }
    tex.width=tex.height=1; tex.valid=true; tex.rgba={50,100,150,255};
    card_surface::MeshBakeResult e, g;
    if (!card_surface::bake_mesh(geom, f.baked, tex, bp, e) || !e.material_digest) return false;
    tex.rgba[0]++;
    if (!card_surface::bake_mesh(geom, f.baked, tex, bp, g) || e.material_digest == g.material_digest) return false;
    bp.texels_per_unit=256;
    if (!card_surface::bake_mesh(geom, f.baked, tex, bp, g)) return false;
    bool denser=false;
    for (size_t i=0; i<e.pages.size(); ++i) denser |= g.pages[i].resolution > e.pages[i].resolution;
    if (!denser) return false;
    return true;
}

bool test_surface_texture_variant_contract() {
    namespace identity = primal::graphics::rhi::card_surface_identity;
    // Compare the shared LUT against the previous runtime scalar expression
    // for every byte, including low sRGB values, saturation and alpha.
    for (float scale : {0.f, .5f, 1.f, 1.5f}) {
        std::vector<u8> pixels(256 * 4);
        for (unsigned i=0;i<256;++i) {
            pixels[i*4]=pixels[i*4+1]=pixels[i*4+2]=u8(i);
            pixels[i*4+3]=u8(255-i);
        }
        identity::ScaleTextureRgb(pixels.data(), 256, scale);
        for (unsigned i=0;i<256;++i) {
            const float srgb=i/255.f;
            float linear=srgb<=.04045f ? srgb/12.92f : std::pow((srgb+.055f)/1.055f,2.4f);
            linear=std::clamp(linear*scale,0.f,1.f);
            const float encoded=linear<=.0031308f ? linear*12.92f : 1.055f*std::pow(linear,1.f/2.4f)-.055f;
            const auto expected=u8(std::clamp(encoded*255.f+.5f,0.f,255.f));
            if (pixels[i*4]!=expected || pixels[i*4+1]!=expected ||
                pixels[i*4+2]!=expected || pixels[i*4+3]!=255-i) return false;
        }
    }
    Fixture f=surface_bake_fixture(.25f,.05f,.5f);
    if (!f.ok) return false;
    std::vector<f32> pos; std::vector<u32> idx;
    auto geom=geometry_view_of(f.m,pos,idx);
    card_surface::RgbaImage tex{1,1,{128,200,255,255},true};
    const auto original=tex.rgba;
    card_surface::BakeParams bp;
    card_surface::MeshBakeResult base,variant;
    if (!card_surface::bake_mesh(geom,f.baked,tex,bp,base)) return false;
    identity::ScaleTextureRgb(tex.rgba.data(),1,.5f);
    if (!card_surface::bake_mesh(geom,f.baked,tex,bp,variant)) return false;
    if (base.geometry_digest!=variant.geometry_digest || base.layout_digest!=variant.layout_digest ||
        !variant.material_digest || base.material_digest==variant.material_digest) return false;
    const float tint[3]={1,1,1};
    if (base.material_digest!=identity::Material(identity::Texture(1,1,original.data()),tint,128,0) ||
        variant.material_digest!=identity::Material(identity::Texture(1,1,tex.rgba.data()),tint,128,0)) return false;
    bool darker=false;
    for (size_t c=0;c<base.pages.size();++c) {
        const auto& a=base.pages[c]; const auto& b=variant.pages[c];
        if (a.triangle_ids!=b.triangle_ids || a.depth!=b.depth || a.normal!=b.normal) return false;
        for (size_t t=0;t<a.depth.size();++t)
            if (a.albedo[t*4+3]>=128) darker|=b.albedo[t*4]<a.albedo[t*4];
    }
    if (!darker) return false;
    const char* path="/tmp/sc-material-variant-test.scsurf";
    card_surface::ScsHeader header,readHeader;
    if (!card_surface::write_sidecar(path,header,{base,variant}) ||
        !card_surface::read_sidecar_header(path,readHeader) || readHeader.mesh_count!=2) return false;
    std::vector<card_surface::ScsMeshRecord> records;
    std::vector<std::vector<card_surface::ScsCardPageEntry>> pages;
    if (!card_surface::read_sidecar_records(path,readHeader,records,pages) || records.size()!=2 ||
        records[0].material_digest!=base.material_digest || records[1].material_digest!=variant.material_digest ||
        records[0].geometry_digest!=records[1].geometry_digest) return false;
    std::remove(path);
    return true;
}

bool test_boundary_samples_attributes_and_identity() {
    // UV texel centers independently identify repeat/V-flip and linearization.
    f32 positions[]={0,0,0, 1,0,0, 0,0,1};
    u32 indices[]={0,1,2}; u16 native[]={0,1,2};
    u8 elements[72]{};
    const f32 uv[3][2]={{.25f,.75f},{.75f,.75f},{.25f,.25f}};
    for (u32 k=0;k<3;++k) {
        const u16 nx=32768, ny=65535;
        std::memcpy(elements+k*24+4,&nx,2); std::memcpy(elements+k*24+6,&ny,2);
        std::memcpy(elements+k*24+16,uv[k],8);
    }
    card_surface::MeshGeometryView g;
    g.positions=positions; g.vertex_count=3; g.indices=reinterpret_cast<u8*>(indices);
    g.index_size=4; g.index_count=3; g.elements=elements; g.element_stride=24;
    g.digest_indices=reinterpret_cast<u8*>(native); g.digest_index_size=2;
    data cards; card card{}; card.axis_direction=1; card.center[1]=1;
    card.extent[0]=card.extent[1]=card.extent[2]=1; cards.cards.push_back(card);
    card_surface::RgbaImage tex{2,2,{255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255},true};
    card_surface::BakeParams params; params.albedo_scale=.5f; params.roughness=70; params.metallic=90;
    card_surface::BoundaryBakeResult result;
    if (!card_surface::bake_boundary_samples(g,cards,tex,params,{0,0},1,16,result) || result.samples.size()!=4) return false;
    const u8 expected[4][3]={{128,0,0},{0,128,0},{64,64,64},{0,0,128}};
    for (u32 k=0;k<4;++k) {
        const auto& s=result.samples[k];
        if (s.triangle_id!=0 || s.position[1]!=0 || s.normal[1]<.999f || s.albedo[3]!=255 ||
            s.params[0]!=70 || s.params[1]!=90 || s.params[2] || s.params[3]) return false;
        for (u32 c=0;c<3;++c) if (s.albedo[c]!=expected[k][c]) return false;
    }
    if (result.samples[2].position[0]!=.5f || result.samples[2].position[2]!=.5f) return false;
    card_surface::MeshBakeResult page;
    if (!card_surface::bake_mesh(g,cards,tex,params,page) ||
        result.geometry_digest!=page.geometry_digest || result.layout_digest!=page.layout_digest ||
        result.material_digest!=page.material_digest) return false;
    // A separate material variant must not share the primary material identity.
    params.albedo_scale=1;
    card_surface::BoundaryBakeResult variant;
    if (!card_surface::bake_boundary_samples(g,cards,tex,params,{0},1,16,variant) ||
        variant.geometry_digest!=result.geometry_digest || variant.layout_digest!=result.layout_digest ||
        variant.material_digest==result.material_digest || variant.samples[0].albedo[0]!=255) return false;
    cards.cards[0].center[0]=.5f;
    if (!card_surface::bake_boundary_samples(g,cards,tex,params,{0},1,16,variant) ||
        variant.layout_digest==result.layout_digest || variant.geometry_digest!=result.geometry_digest) return false;
    positions[0]=.1f;
    if (!card_surface::bake_boundary_samples(g,cards,tex,params,{0},1,16,variant) ||
        variant.geometry_digest==result.geometry_digest) return false;
    return true;
}

bool test_boundary_samples_budget_and_owner_isolation() {
    // Duplicate geometry retains distinct original primitive IDs; no welding.
    f32 positions[]={0,0,0, 1,0,0, 0,0,1};
    u32 indices[]={0,1,2,0,1,2};
    card_surface::MeshGeometryView g;
    g.positions=positions; g.vertex_count=3; g.indices=reinterpret_cast<u8*>(indices);
    g.index_size=4; g.index_count=6;
    data cards; cards.cards.push_back({});
    card_surface::RgbaImage tex; card_surface::BakeParams params;
    card_surface::BoundaryBakeResult result;
    if (!card_surface::bake_boundary_samples(g,cards,tex,params,{1,0,1},1,8,result) || result.samples.size()!=8) return false;
    for (u32 i=0;i<8;++i) if (result.samples[i].triangle_id!=i/4 || result.samples[i].normal[1]!=-1) return false;
    // Failure clears an earlier successful result; never emits a partial mesh.
    if (card_surface::bake_boundary_samples(g,cards,tex,params,{0,1},1,7,result) || !result.samples.empty() || result.geometry_digest) return false;
    if (card_surface::bake_boundary_samples(g,cards,tex,params,{2},1,8,result)) return false;
    if (card_surface::bake_boundary_samples(g,cards,tex,params,{0},0,8,result)) return false;
    if (card_surface::bake_boundary_samples(g,cards,tex,params,{0},1e-30f,8,result)) return false;
    tex={2,2,{0},true};
    if (card_surface::bake_boundary_samples(g,cards,tex,params,{0},1,8,result)) return false;
    tex={}; indices[2]=3;
    if (card_surface::bake_boundary_samples(g,cards,tex,params,{0},1,8,result)) return false;
    indices[2]=1; // Degenerate primitive.
    if (card_surface::bake_boundary_samples(g,cards,tex,params,{0},1,8,result)) return false;
    indices[2]=2; const u32 infinity_bits=0x7f800000u;
    std::memcpy(&positions[0],&infinity_bits,4);
    if (card_surface::bake_boundary_samples(g,cards,tex,params,{0},1,8,result)) return false;
    positions[0]=0; const u32 nan_bits=0x7fc00001u;
    std::memcpy(&params.tint[0],&nan_bits,4);
    if (card_surface::bake_boundary_samples(g,cards,tex,params,{0},1,8,result)) return false;
    return true;
}

bool test_boundary_sidecar_roundtrip() {
    namespace boundary=primal::graphics::rhi::card_boundary;
    Fixture fixture=surface_bake_fixture(.25f,.05f,.5f);
    if (!fixture.ok) return false;
    std::vector<f32> pos; std::vector<u32> indices;
    const auto geom=geometry_view_of(fixture.m,pos,indices);
    card_surface::RgbaImage texture{1,1,{128,255,64,255},true};
    card_surface::BakeParams params;
    boundary::Mesh base,variant;
    if (!card_surface::bake_boundary_samples(geom,fixture.baked,texture,params,{0,1},.1f,128,base)) return false;
    params.albedo_scale=.5f;
    if (!card_surface::bake_boundary_samples(geom,fixture.baked,texture,params,{0,1},.1f,128,variant)) return false;
    std::vector<u8> bytes,reverse;
    if (!boundary::Encode({base,variant},bytes) || !boundary::Encode({variant,base},reverse) || bytes!=reverse) return false;
    const char* path="/tmp/sc-boundary-roundtrip.scboundary";
    if (!boundary::Write(path,{base,variant})) return false;
    std::vector<boundary::Mesh> loaded;
    if (!boundary::Read(path,loaded) || loaded.size()!=2) return false;
    for (const auto* original:{&base,&variant}) {
        const auto slot=boundary::FindMesh(loaded,original->geometry_digest,original->layout_digest,original->material_digest);
        if (slot>=loaded.size() || loaded[slot].triangle_count!=original->triangle_count ||
            loaded[slot].samples.size()!=original->samples.size() ||
            std::memcmp(loaded[slot].samples.data(),original->samples.data(),original->samples.size()*sizeof(boundary::Sample))) return false;
    }
    if (boundary::FindMesh(loaded,base.geometry_digest^1,base.layout_digest,base.material_digest)!=0xffffffffu ||
        boundary::FindMesh(loaded,base.geometry_digest,base.layout_digest^1,base.material_digest)!=0xffffffffu ||
        boundary::FindMesh(loaded,base.geometry_digest,base.layout_digest,base.material_digest^1)!=0xffffffffu) return false;
    if (boundary::Write(path,{base,base})) return false;
    base.samples[0].triangle_id=base.triangle_count;
    if (boundary::Write(path,{base}) || !boundary::Read(path,loaded) || loaded.size()!=2) return false;
    std::remove(path);
    return true;
}

bool test_surface_bake_sidecar_roundtrip() {
    Fixture f = surface_bake_fixture(0.25f, 0.05f, 0.5f);
    if (!f.ok) return false;
    std::vector<f32> pos; std::vector<u32> idx;
    card_surface::MeshGeometryView geom = geometry_view_of(f.m, pos, idx);
    card_surface::RgbaImage tex{};
    card_surface::BakeParams bp{};
    card_surface::MeshBakeResult res;
    if (!card_surface::bake_mesh(geom, f.baked, tex, bp, res)) return false;

    card_surface::ScsHeader header{};
    header.albedo_scale = 0.5f;
    const char* path = "/tmp/scpermesh_test.scsurf";
    if (!card_surface::write_sidecar(path, header, {res})) return false;

    card_surface::ScsHeader rh{};
    if (!card_surface::read_sidecar_header(path, rh)) return false;
    if (rh.magic != card_surface::kScsMagic || rh.version != card_surface::kScsVersion) return false;
    if (std::abs(rh.albedo_scale - 0.5f) > 1e-6f) return false;
    if (rh.mesh_count != 1) return false;

    std::vector<card_surface::ScsMeshRecord> meshes;
    std::vector<std::vector<card_surface::ScsCardPageEntry>> pages;
    if (!card_surface::read_sidecar_records(path, rh, meshes, pages)) return false;
    if (meshes.size() != 1 || pages.size() != 1) return false;
    if (meshes[0].geometry_digest != res.geometry_digest) return false;
    if (meshes[0].layout_digest != res.layout_digest) return false;
    if (meshes[0].card_count != (u32)res.pages.size()) return false;
    if (pages[0].size() != res.pages.size()) return false;
    for (size_t c = 0; c < res.pages.size(); ++c) {
        if (pages[0][c].card_index != res.pages[c].card_index) return false;
        if (pages[0][c].resolution != res.pages[c].resolution) return false;
        if (pages[0][c].albedo_offset == 0) return false;
        const auto& pg=res.pages[c];
        std::vector<u32> ids(pg.triangle_ids.size());
        FILE* file=std::fopen(path,"rb"); if(!file) return false;
        bool read=std::fseek(file,long(pages[0][c].triangle_offset),SEEK_SET)==0 &&
            std::fread(ids.data(),sizeof(u32),ids.size(),file)==ids.size();
        std::fclose(file);
        if(!read || ids!=pg.triangle_ids) return false;
        for(size_t t=0;t<ids.size();++t) {
            const bool valid=pg.albedo[t*4+3]>=128;
            if(valid ? ids[t]>=idx.size()/3 : ids[t]!=0xffffffffu) return false;
        }
    }
    return true;
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
        CASE(test_v2_alcove_inner_local_cards),
        CASE(test_v1_alcove_has_no_inner_local_cards),
        CASE(test_v2_plate_keeps_thin_views),
        CASE(test_v2_max_cards_cap),
        CASE(test_v1_card_bake_section_parses),
        CASE(test_surface_bake_box_coverage_depth_normal),
        CASE(test_surface_bake_owner_isolation_foreign_triangle_invisible),
        CASE(test_surface_bake_capture_far_contract),
        CASE(test_surface_bake_digests_stability),
        CASE(test_surface_bake_sidecar_roundtrip),
        CASE(test_surface_texture_variant_contract),
        CASE(test_boundary_samples_attributes_and_identity),
        CASE(test_boundary_samples_budget_and_owner_isolation),
        CASE(test_boundary_sidecar_roundtrip),
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
