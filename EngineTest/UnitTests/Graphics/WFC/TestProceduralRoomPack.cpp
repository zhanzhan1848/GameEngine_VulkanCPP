#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/AutoSocketClassifier.h"
#include "Engine/Graphics/WFC/ProceduralRoomPack.h"
#include "Engine/Graphics/RHI/Core/RHIMeshAsset.h"

#include <set>
#include <string>

using namespace primal::graphics::wfc;
using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;

TestResult TestTileCountIs12() {
    TEST_ASSERT_EQ(12u, ProceduralRoomPack::kTileCount, "must be 12 tiles");
    return TestResult::Passed;
}

TestResult TestTileDefsCoverMatrix() {
    bool seen_size[3]  = {false, false, false};
    bool seen_doors[4] = {false, false, false, false};
    for (u32 i = 0; i < ProceduralRoomPack::kTileCount; ++i) {
        const auto& def = ProceduralRoomPack::TileDefs()[i];
        u32 size_idx = (def.footprint_cells - 3) / 2;
        TEST_ASSERT(size_idx < 3u, "size must be 3/5/7");
        seen_size[size_idx] = true;
        TEST_ASSERT(def.door_mask < 16u, "door_mask must fit 4 bits");
        seen_doors[0] |= (def.door_mask == 0x1);
        seen_doors[1] |= (def.door_mask == 0x3);
        seen_doors[2] |= (def.door_mask == 0xC);
        seen_doors[3] |= (def.door_mask == 0xF);
    }
    for (u32 i = 0; i < 3; ++i) TEST_ASSERT(seen_size[i],  "all sizes present");
    for (u32 i = 0; i < 4; ++i) TEST_ASSERT(seen_doors[i], "all door configs present");
    return TestResult::Passed;
}

TestResult TestTileNamesAreUnique() {
    std::set<std::string> names;
    for (u32 i = 0; i < ProceduralRoomPack::kTileCount; ++i) {
        names.insert(ProceduralRoomPack::TileDefs()[i].name);
    }
    TEST_ASSERT_EQ(12u, names.size(), "all names unique");
    return TestResult::Passed;
}

// All 12 tiles must produce a non-empty mesh with consistent vert/index counts.
// Each tile has floor + ceiling (4v/6i each) + 4 walls. A solid wall is 4v/6i;
// a door wall is 12v/18i (3 sub-quads). Counts must match the formula.
TestResult TestGenerateTileMeshProducesGeometry() {
    for (u32 i = 0; i < ProceduralRoomPack::kTileCount; ++i) {
        RHIMeshAsset mesh;
        ProceduralRoomPack::GenerateTileMesh(i, mesh);

        TEST_ASSERT(mesh.num_vertices > 0, "tile must have verts");
        TEST_ASSERT(mesh.num_indices  > 0, "tile must have indices");
        TEST_ASSERT_EQ(4u, mesh.index_size, "indices must be u32");

        const u32 door_mask = ProceduralRoomPack::TileDefs()[i].door_mask;
        u32 door_count = 0;
        for (u32 bit = 0; bit < 4u; ++bit) if (door_mask & (1u << bit)) ++door_count;
        const u32 solid_count = 4u - door_count;
        const u32 expected_v = 4u + 4u + door_count * 12u + solid_count * 4u;
        const u32 expected_i = 6u + 6u + door_count * 18u + solid_count * 6u;
        TEST_ASSERT_EQ(expected_v, mesh.num_vertices, "vert count must match door_count");
        TEST_ASSERT_EQ(expected_i, mesh.num_indices,  "index count must match door_count");

        // Buffer sizes must match the declared counts.
        TEST_ASSERT_EQ(mesh.num_vertices * 12u,
                       static_cast<u32>(mesh.position_buffer.size()),
                       "position_buffer byte size");
        TEST_ASSERT_EQ(mesh.num_indices * 4u,
                       static_cast<u32>(mesh.index_buffer.size()),
                       "index_buffer byte size");
    }
    return TestResult::Passed;
}

// Tile 0 (door_mask=0x1, +X door only): +X face must show a door opening —
// corners solid, center cells opening. Verified against the classifier's
// face basis (origin +X, u_axis=-Z, v_axis=+Y) and tile geometry
// (door y∈[-0.5,+0.1], z∈[-0.25,+0.25]).
TestResult TestGenerateTileMeshDoorOnPosXFace() {
    RHIMeshAsset mesh;
    ProceduralRoomPack::GenerateTileMesh(0, mesh);

    const m4x4 identity = matrix_identity_float4x4;
    const SocketEncoding sig = AutoSocketClassifier::ClassifyFace(
        mesh, WFCFace::PosX, identity);

    // Four corners of +X face must be solid (outside the door opening).
    TEST_ASSERT((sig & (SocketEncoding{1} << 0))  != 0, "(i=0,j=0) corner solid");
    TEST_ASSERT((sig & (SocketEncoding{1} << 7))  != 0, "(i=7,j=0) corner solid");
    TEST_ASSERT((sig & (SocketEncoding{1} << 56)) != 0, "(i=0,j=7) corner solid");
    TEST_ASSERT((sig & (SocketEncoding{1} << 63)) != 0, "(i=7,j=7) corner solid");

    // Center cells (i=3..4, j=3..4) fall inside the door opening and must read 0.
    TEST_ASSERT((sig & (SocketEncoding{1} << (3 + 3*8))) == 0, "(i=3,j=3) door opening");
    TEST_ASSERT((sig & (SocketEncoding{1} << (4 + 4*8))) == 0, "(i=4,j=4) door opening");

    // Signature is neither fully solid nor fully open — proves the door is detected.
    TEST_ASSERT(sig != 0,                      "+X face not fully open");
    TEST_ASSERT(sig != 0xFFFFFFFFFFFFFFFFULL,  "+X face not fully solid");
    return TestResult::Passed;
}

// Tile 0 has no door on -X (door_mask=0x1 only sets +X). The -X face must be
// a fully solid wall — all 64 cells set. Ensures GenerateTileMesh doesn't
// accidentally cut a door on the wrong wall.
TestResult TestGenerateTileMeshNoDoorOnNegXFace() {
    RHIMeshAsset mesh;
    ProceduralRoomPack::GenerateTileMesh(0, mesh);

    const m4x4 identity = matrix_identity_float4x4;
    const SocketEncoding sig = AutoSocketClassifier::ClassifyFace(
        mesh, WFCFace::NegX, identity);
    TEST_ASSERT_EQ(0xFFFFFFFFFFFFFFFFULL, sig, "-X face must be fully solid");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("ProceduralRoomPack");
    TEST_CASE(suite, "TileCountIs12",                   TestTileCountIs12);
    TEST_CASE(suite, "TileDefsCoverMatrix",             TestTileDefsCoverMatrix);
    TEST_CASE(suite, "TileNamesAreUnique",              TestTileNamesAreUnique);
    TEST_CASE(suite, "GenerateTileMeshProducesGeometry",TestGenerateTileMeshProducesGeometry);
    TEST_CASE(suite, "GenerateTileMeshDoorOnPosXFace",  TestGenerateTileMeshDoorOnPosXFace);
    TEST_CASE(suite, "GenerateTileMeshNoDoorOnNegXFace",TestGenerateTileMeshNoDoorOnNegXFace);
    suite.RunAllTests();
    return 0;
}
