// M9 unit tests for SceneBlobReader.
//
// Verifies the reader correctly skips the pack_data scene wrapper
// (scene_name + materials + lod_group_count + per-lod wrapper) before
// walking pack_mesh_data records. This is the format ImportFbx/ImportObjAPI
// produce, and what ContentToolsCLI --pipeline / --validate consume.
//
// Smoke regression for the M9.4 bug where the reader treated the wrapper
// as part of the first mesh header, producing "truncated_element" errors.

#include "pipeline/SceneBlobReader.h"
#include "pipeline/SceneBlobWriter.h"
#include "pipeline/Validator.h"
#include "common/ProcessableMesh.h"
#include "common/PipelineTypes.h"
#include "common/ErrorReport.h"
#include "Geometry.h"
#include "../Engine/Utilities/IOStream.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace primal;
using namespace primal::tools;
using namespace primal::math;
using namespace primal::tools::pipeline;

namespace {

// Minimal 8-vert cube mesh with positions/normals/tangents/UV.
ProcessableMesh make_cube(const char* name) {
    ProcessableMesh m;
    m.name = name;
    m.material_idx = 0;
    for (s8 x = -1; x <= 1; x += 2) {
      for (s8 y = -1; y <= 1; y += 2) {
        for (s8 z = -1; z <= 1; z += 2) {
            v3 p{(f32)x, (f32)y, (f32)z};
            m.positions.emplace_back(p);
            m.normals.emplace_back(v3{(f32)x, (f32)y, (f32)z});
            m.tangents.emplace_back(v4{1.f, 0.f, 0.f, 1.f});
        }
      }
    }
    UVSet uvs;
    uvs.purpose = UVSetPurpose::Texture;
    for (u32 i = 0; i < 8; ++i) uvs.coords.emplace_back(v2{0.25f, 0.5f});
    m.uv_sets.emplace_back(std::move(uvs));
    static const u32 idx[36] = {
        0,1,2, 0,2,3, 4,6,5, 4,7,6,
        0,4,5, 0,5,1, 2,6,7, 2,7,3,
        0,3,7, 0,7,4, 1,5,6, 1,6,2,
    };
    for (u32 i : idx) m.indices.emplace_back(i);
    return m;
}

// Serialize a synthetic scene wrapper + N mesh blobs into a flat buffer.
// Mirror of pack_data structure: scene_name + materials[] + lod_groups[].
struct MaterialInfo {
    std::string name, diffuse, normal;
};

void write_string(utl::blob_stream_writer& blob, const std::string& s) {
    blob.write((u32)s.size());
    blob.write(s.c_str(), s.size());
}

// Compute size of the scene wrapper the same way Serialize writes it.
// build_scene_blob uses this to preallocate the buffer (must match the write
// path exactly or the writer overruns the buffer).
size_t scene_blob_size(const std::string& scene_name,
                       const std::vector<MaterialInfo>& materials,
                       const std::vector<std::vector<PackedMesh>>& lod_groups) {
    size_t sz = 4 + scene_name.size();
    sz += 4;
    for (const auto& mat : materials) {
        sz += 4 + mat.name.size();
        sz += 4 + mat.diffuse.size();
        sz += 4 + mat.normal.size();
    }
    sz += 4;
    for (size_t lg = 0; lg < lod_groups.size(); ++lg) {
        const std::string lod_name = "lod_" + std::to_string(lg);
        sz += 4 + lod_name.size();     // u32 size + name bytes
        sz += 4;                       // mesh_count
        for (const auto& pm : lod_groups[lg]) sz += GetPackedMeshSize(pm);
    }
    return sz;
}

std::vector<u8> build_scene_blob(const std::string& scene_name,
                                 const std::vector<MaterialInfo>& materials,
                                 const std::vector<std::vector<PackedMesh>>& lod_groups) {
    size_t sz = scene_blob_size(scene_name, materials, lod_groups);

    std::vector<u8> buf(sz);
    utl::blob_stream_writer blob{buf.data(), sz};

    write_string(blob, scene_name);
    blob.write((u32)materials.size());
    for (const auto& mat : materials) {
        write_string(blob, mat.name);
        write_string(blob, mat.diffuse);
        write_string(blob, mat.normal);
    }
    blob.write((u32)lod_groups.size());
    for (size_t lg = 0; lg < lod_groups.size(); ++lg) {
        const std::string lod_name = "lod_" + std::to_string(lg);
        write_string(blob, lod_name);
        blob.write((u32)lod_groups[lg].size());
        for (const auto& pm : lod_groups[lg]) Serialize(pm, blob);
    }

    return buf;
}

scene_data make_scene_data(const std::vector<u8>& buf) {
    scene_data sd{};
    sd.buffer = const_cast<u8*>(buf.data());  // reader does not mutate
    sd.buffer_size = (u32)buf.size();
    return sd;
}

// Pretty-print errors from a vector for debugging.
void dump_errors(const primal::utl::vector<ErrorReport>& errs) {
    for (const auto& e : errs) {
        std::cout << "    [" << (e.severity == Severity::Error ? "E" : "W") << "] "
                  << e.code << ": " << e.message << "\n";
    }
}

}  // namespace

// ---- Scene wrapper skipped correctly ------------------------------------

bool test_single_mesh_single_lod_walks_one_mesh() {
    primal::utl::vector<ErrorReport> errs;
    ProcessableMesh m = make_cube("only_mesh");
    PackedMesh pm = BuildPackedMesh(m, {}, 0, 0.5f, errs);

    auto buf = build_scene_blob("scene", {{"mat_a","diff.png","norm.png"}}, {{pm}});
    scene_data sd = make_scene_data(buf);

    errs.clear();  // reset to isolate validate-phase errors only
    ValidationReport rpt;
    auto ok = ValidateSceneBlob(sd, rpt, errs);
    bool pass = ok && rpt.mesh_count == 1 && errs.empty();
    if (!pass) {
        std::cout << "    ok=" << ok << " mesh_count=" << rpt.mesh_count
                  << " errs=" << errs.size() << "\n";
        dump_errors(errs);
    }
    return pass;
}

bool test_multi_mesh_in_single_lod() {
    primal::utl::vector<ErrorReport> errs;
    ProcessableMesh a = make_cube("mesh_a");
    ProcessableMesh b = make_cube("mesh_b");
    PackedMesh pa = BuildPackedMesh(a, {}, 0, 0.5f, errs);
    PackedMesh pb = BuildPackedMesh(b, {}, 0, 0.5f, errs);

    auto buf = build_scene_blob("scene", {{"mat_a","d.png","n.png"}}, {{pa, pb}});
    scene_data sd = make_scene_data(buf);

    errs.clear();
    ValidationReport rpt;
    auto ok = ValidateSceneBlob(sd, rpt, errs);
    bool pass = ok && rpt.mesh_count == 2 && errs.empty();
    if (!pass) dump_errors(errs);
    return pass;
}

bool test_multi_lod_groups_with_multiple_meshes_each() {
    primal::utl::vector<ErrorReport> errs;
    PackedMesh lod0_a = BuildPackedMesh(make_cube("l0a"), {}, 0, 0.0f, errs);
    PackedMesh lod0_b = BuildPackedMesh(make_cube("l0b"), {}, 0, 0.0f, errs);
    PackedMesh lod1_a = BuildPackedMesh(make_cube("l1a"), {}, 1, 0.5f, errs);
    PackedMesh lod1_b = BuildPackedMesh(make_cube("l1b"), {}, 1, 0.5f, errs);
    PackedMesh lod2   = BuildPackedMesh(make_cube("l2"),  {}, 2, 0.9f, errs);

    auto buf = build_scene_blob("multi_lod", {{"m0","d","n"},{"m1","d2","n2"}}, {
        {lod0_a, lod0_b},
        {lod1_a, lod1_b},
        {lod2},
    });
    scene_data sd = make_scene_data(buf);

    errs.clear();
    ValidationReport rpt;
    auto ok = ValidateSceneBlob(sd, rpt, errs);
    bool pass = ok && rpt.mesh_count == 5 && rpt.meshlet_count == 0 && errs.empty();
    if (!pass) dump_errors(errs);
    return pass;
}

bool test_long_material_strings_handled() {
    primal::utl::vector<ErrorReport> errs;
    PackedMesh pm = BuildPackedMesh(make_cube("m"), {}, 0, 0.5f, errs);
    MaterialInfo mat{
        "very_long_material_name_with_underscores_and_numbers_123",
        "path/to/textures/diffuse_with_long_name.png",
        "path/to/textures/normal_with_long_name.png",
    };
    auto buf = build_scene_blob("scene", {mat}, {{pm}});
    scene_data sd = make_scene_data(buf);

    errs.clear();
    ValidationReport rpt;
    auto ok = ValidateSceneBlob(sd, rpt, errs);
    bool pass = ok && rpt.mesh_count == 1 && errs.empty();
    if (!pass) dump_errors(errs);
    return pass;
}

bool test_empty_scene_name_and_empty_material_strings() {
    primal::utl::vector<ErrorReport> errs;
    PackedMesh pm = BuildPackedMesh(make_cube("m"), {}, 0, 0.5f, errs);
    MaterialInfo mat{"", "", ""};
    auto buf = build_scene_blob("", {mat}, {{pm}});
    scene_data sd = make_scene_data(buf);

    errs.clear();
    ValidationReport rpt;
    auto ok = ValidateSceneBlob(sd, rpt, errs);
    bool pass = ok && rpt.mesh_count == 1 && errs.empty();
    if (!pass) dump_errors(errs);
    return pass;
}

bool test_empty_blob_returns_zero_meshes() {
    primal::utl::vector<ErrorReport> errs;
    scene_data sd{};
    ValidationReport rpt;
    auto ok = ValidateSceneBlob(sd, rpt, errs);
    // Empty buffer emits one warning (not error) and visits 0 meshes.
    // ValidateSceneBlob should return true: no error severity.
    return ok && rpt.mesh_count == 0;
}

bool test_truncated_buffer_emits_error_not_crash() {
    primal::utl::vector<ErrorReport> errs;
    PackedMesh pm = BuildPackedMesh(make_cube("m"), {}, 0, 0.5f, errs);
    auto buf = build_scene_blob("scene", {{"m","d","n"}}, {{pm}});

    // Cut the buffer in half — should produce a clean error, not a crash.
    buf.resize(buf.size() / 2);
    scene_data sd = make_scene_data(buf);

    errs.clear();
    ValidationReport rpt;
    ValidateSceneBlob(sd, rpt, errs);
    bool pass = !errs.empty();
    if (!pass) dump_errors(errs);
    return pass;
}

bool test_visitor_receives_packed_mesh_views_in_order() {
    primal::utl::vector<ErrorReport> errs;
    PackedMesh pa = BuildPackedMesh(make_cube("alpha"), {}, 0, 0.5f, errs);
    PackedMesh pb = BuildPackedMesh(make_cube("beta"),  {}, 0, 0.5f, errs);
    PackedMesh pc = BuildPackedMesh(make_cube("gamma"), {}, 0, 0.5f, errs);

    auto buf = build_scene_blob("scene", {{"m","d","n"}}, {{pa, pb, pc}});
    scene_data sd = make_scene_data(buf);

    std::vector<std::pair<u32, std::string>> seen;
    WalkSceneBlob(sd, [&](u32 idx, const PackedMeshView& view) {
        seen.emplace_back(idx, view.name);
    }, errs);

    bool pass = seen.size() == 3
        && seen[0] == std::make_pair(0u, std::string("alpha"))
        && seen[1] == std::make_pair(1u, std::string("beta"))
        && seen[2] == std::make_pair(2u, std::string("gamma"));
    if (!pass) dump_errors(errs);
    return pass;
}

// ---- Test runner --------------------------------------------------------

struct Case { const char* name; bool (*fn)(); };
#define CASE(n) { #n, n }

int main() {
    const Case cases[] = {
        CASE(test_single_mesh_single_lod_walks_one_mesh),
        CASE(test_multi_mesh_in_single_lod),
        CASE(test_multi_lod_groups_with_multiple_meshes_each),
        CASE(test_long_material_strings_handled),
        CASE(test_empty_scene_name_and_empty_material_strings),
        CASE(test_empty_blob_returns_zero_meshes),
        CASE(test_truncated_buffer_emits_error_not_crash),
        CASE(test_visitor_receives_packed_mesh_views_in_order),
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
