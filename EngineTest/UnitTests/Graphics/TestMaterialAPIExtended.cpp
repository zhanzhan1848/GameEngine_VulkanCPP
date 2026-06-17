// Unit tests for extended MaterialAPI (P1 texture get/set + P2.9 batch update).
// Tests the new C ABI functions exposed by EngineDLL/MaterialAPI.cpp:
//   - GetMaterialAlbedoTexture / SetMaterialAlbedoTexture
//   - GetMaterialNormalTexture / SetMaterialNormalTexture
//   - GetMaterialOrmTexture / SetMaterialOrmTexture
//   - GetEntityCountWithMaterial / GetEntityIdWithMaterial
//   - ApplyMaterialParamBatch
//
// Entity lifecycle uses the existing EntityAPI C exports (CreateGameEntity /
// RemoveGameEntity) so this test links only against EngineDLL.

#include "TestFramework.h"
#include "CommonHeaders.h"
#include <iostream>
#include <cstring>
#include <cmath>

using namespace Engine::Test;

// --- C-facing struct declarations (must match MaterialAPI.cpp / EntityAPI.cpp) ---

struct material_param_update_c {
    u64 entity_id;
    u32 param_id;
    u32 _pad;
    f32 value;
};

// EntityAPI descriptor (matches game_entity_descriptor in EntityAPI.cpp)
struct transform_component_c {
    f32 position[3];
    f32 rotation[3];
    f32 scale[3];
};

struct mesh_component_c {
    u64 geometry_content_id;  // id::id_type
    u64 material_ids[8];
    u32 material_count;
};

struct game_entity_descriptor_c {
    transform_component_c transform;
    mesh_component_c mesh;
};

extern "C" {
    // Entity lifecycle (from EntityAPI.cpp)
    u32  CreateGameEntity(game_entity_descriptor_c* e);
    void RemoveGameEntity(u32 id);
    u32  IsEntityAlive(u32 id);

    // Material lifecycle (from MaterialAPI.cpp)
    void AddEntityMaterial(u32 entity_id, void* desc);
    void RemoveEntityMaterial(u32 entity_id);
    u32  HasEntityMaterial(u32 entity_id);

    // Material getters (from MaterialAPI.cpp)
    u32  GetMaterialTechnique(u32 entity_id);
    f32  GetMaterialRoughness(u32 entity_id);
    f32  GetMaterialMetallic(u32 entity_id);
    f32  GetMaterialAlphaCutoff(u32 entity_id);
    void GetMaterialBaseColor(u32 entity_id, f32* out_rgba);

    // Material texture getters (P1)
    u64  GetMaterialAlbedoTexture(u32 entity_id);
    u64  GetMaterialNormalTexture(u32 entity_id);
    u64  GetMaterialOrmTexture(u32 entity_id);

    // Material texture setters (P1)
    void SetMaterialAlbedoTexture(u32 entity_id, u64 texture_id);
    void SetMaterialNormalTexture(u32 entity_id, u64 texture_id);
    void SetMaterialOrmTexture(u32 entity_id, u64 texture_id);

    // Entity enumeration (P2.9)
    u32  GetEntityCountWithMaterial();
    u32  GetEntityIdWithMaterial(u32 index);

    // Batch update (P2.9)
    void ApplyMaterialParamBatch(const material_param_update_c* updates, u32 count);
}

// --- Helpers ---

// material_descriptor matching the DLL's material_descriptor struct.
// id::id_type is u32, so texture fields must be u32 to match the DLL's struct layout.
struct material_descriptor_dll {
    u32       technique;
    f32       base_color[4];
    f32       roughness;
    f32       metallic;
    f32       alpha_cutoff;
    u32       albedo_texture;
    u32       normal_texture;
    u32       orm_texture;
};

static u32 create_entity() {
    game_entity_descriptor_c desc{};
    desc.transform.position[0] = 0.f; desc.transform.position[1] = 0.f; desc.transform.position[2] = 0.f;
    desc.transform.rotation[0] = 0.f; desc.transform.rotation[1] = 0.f; desc.transform.rotation[2] = 0.f;
    desc.transform.scale[0] = 1.f; desc.transform.scale[1] = 1.f; desc.transform.scale[2] = 1.f;
    return CreateGameEntity(&desc);
}

static u32 create_entity_with_material(u32 albedo = 100, u32 normal = 200, u32 orm = 300) {
    u32 eid = create_entity();
    material_descriptor_dll mdesc{};
    mdesc.technique = 0;
    mdesc.base_color[0] = 1.f; mdesc.base_color[1] = 1.f;
    mdesc.base_color[2] = 1.f; mdesc.base_color[3] = 1.f;
    mdesc.roughness = 0.5f;
    mdesc.metallic = 0.0f;
    mdesc.alpha_cutoff = 0.5f;
    mdesc.albedo_texture = albedo;
    mdesc.normal_texture = normal;
    mdesc.orm_texture = orm;
    AddEntityMaterial(eid, &mdesc);
    return eid;
}

static void cleanup_entity(u32 eid) {
    if (eid && HasEntityMaterial(eid)) {
        RemoveEntityMaterial(eid);
    }
    if (eid) {
        RemoveGameEntity(eid);
    }
}

// --- Test 1: Set/Get albedo texture roundtrip ---

TestResult TestAlbedoTextureRoundtrip() {
    u32 eid = create_entity_with_material();

    SetMaterialAlbedoTexture(eid, 12345);
    u64 result = GetMaterialAlbedoTexture(eid);
    TEST_ASSERT_EQ(result, 12345ull, "Albedo texture should round-trip");

    cleanup_entity(eid);
    return TestResult::Passed;
}

// --- Test 2: Set/Get normal texture roundtrip ---

TestResult TestNormalTextureRoundtrip() {
    u32 eid = create_entity_with_material();

    SetMaterialNormalTexture(eid, 67890);
    u64 result = GetMaterialNormalTexture(eid);
    TEST_ASSERT_EQ(result, 67890ull, "Normal texture should round-trip");

    cleanup_entity(eid);
    return TestResult::Passed;
}

// --- Test 3: Set/Get orm texture roundtrip ---

TestResult TestOrmTextureRoundtrip() {
    u32 eid = create_entity_with_material();

    SetMaterialOrmTexture(eid, 99999);
    u64 result = GetMaterialOrmTexture(eid);
    TEST_ASSERT_EQ(result, 99999ull, "ORM texture should round-trip");

    cleanup_entity(eid);
    return TestResult::Passed;
}

// --- Test 4: Initial texture values from init_info ---

TestResult TestInitialTextureValues() {
    u32 eid = create_entity_with_material(11, 22, 33);

    TEST_ASSERT_EQ(GetMaterialAlbedoTexture(eid), 11ull, "Initial albedo should match init_info");
    TEST_ASSERT_EQ(GetMaterialNormalTexture(eid), 22ull, "Initial normal should match init_info");
    TEST_ASSERT_EQ(GetMaterialOrmTexture(eid), 33ull, "Initial orm should match init_info");

    cleanup_entity(eid);
    return TestResult::Passed;
}

// --- Test 5: GetMaterialAlbedoTexture on entity without material returns 0 ---

TestResult TestTextureGetterNoMaterial() {
    u32 eid = create_entity();

    TEST_ASSERT_EQ(GetMaterialAlbedoTexture(eid), 0ull, "Should return 0 for entity without material");
    TEST_ASSERT_EQ(GetMaterialNormalTexture(eid), 0ull, "Should return 0 for entity without material");
    TEST_ASSERT_EQ(GetMaterialOrmTexture(eid), 0ull, "Should return 0 for entity without material");

    cleanup_entity(eid);
    return TestResult::Passed;
}

// --- Test 6: Entity enumeration - count ---

TestResult TestEntityCountWithMaterial() {
    u32 before = GetEntityCountWithMaterial();

    u32 e1 = create_entity_with_material();
    u32 e2 = create_entity_with_material();

    u32 after = GetEntityCountWithMaterial();
    TEST_ASSERT_EQ(after, before + 2, "Count should increase by 2 after adding 2 materials");

    cleanup_entity(e1);
    cleanup_entity(e2);

    u32 final_count = GetEntityCountWithMaterial();
    TEST_ASSERT_EQ(final_count, before, "Count should return to baseline after cleanup");

    return TestResult::Passed;
}

// --- Test 7: Entity enumeration - get by index ---

TestResult TestEntityIdWithMaterial() {
    u32 e1 = create_entity_with_material();
    u32 e2 = create_entity_with_material();

    u32 count = GetEntityCountWithMaterial();
    bool found1 = false, found2 = false;
    for (u32 i = 0; i < count; ++i)
    {
        u32 id = GetEntityIdWithMaterial(i);
        if (id == e1) found1 = true;
        if (id == e2) found2 = true;
    }

    TEST_ASSERT(found1, "Should find entity e1 in enumeration");
    TEST_ASSERT(found2, "Should find entity e2 in enumeration");

    // Out-of-range index should return 0
    TEST_ASSERT_EQ(GetEntityIdWithMaterial(count + 100), 0u, "Out-of-range index should return 0");

    cleanup_entity(e1);
    cleanup_entity(e2);
    return TestResult::Passed;
}

// --- Test 8: Batch update - roughness and metallic ---

TestResult TestBatchRoughnessMetallic() {
    u32 eid = create_entity_with_material();

    material_param_update_c updates[2];
    updates[0].entity_id = eid;
    updates[0].param_id = 1; // roughness
    updates[0].value = 0.75f;
    updates[1].entity_id = eid;
    updates[1].param_id = 2; // metallic
    updates[1].value = 0.33f;

    ApplyMaterialParamBatch(updates, 2);

    TEST_ASSERT(std::abs(GetMaterialRoughness(eid) - 0.75f) < 1e-5f, "Batch roughness mismatch");
    TEST_ASSERT(std::abs(GetMaterialMetallic(eid) - 0.33f) < 1e-5f, "Batch metallic mismatch");

    cleanup_entity(eid);
    return TestResult::Passed;
}

// --- Test 9: Batch update - base_color channels ---

TestResult TestBatchBaseColor() {
    u32 eid = create_entity_with_material();

    material_param_update_c updates[4];
    for (u32 i = 0; i < 4; ++i)
    {
        updates[i].entity_id = eid;
        updates[i].param_id = 4 + i; // base_color[0..3]
        updates[i].value = 0.1f * (i + 1);
    }

    ApplyMaterialParamBatch(updates, 4);

    f32 rgba[4];
    GetMaterialBaseColor(eid, rgba);
    TEST_ASSERT(std::abs(rgba[0] - 0.1f) < 1e-5f, "Batch base_color[0] mismatch");
    TEST_ASSERT(std::abs(rgba[1] - 0.2f) < 1e-5f, "Batch base_color[1] mismatch");
    TEST_ASSERT(std::abs(rgba[2] - 0.3f) < 1e-5f, "Batch base_color[2] mismatch");
    TEST_ASSERT(std::abs(rgba[3] - 0.4f) < 1e-5f, "Batch base_color[3] mismatch");

    cleanup_entity(eid);
    return TestResult::Passed;
}

// --- Test 10: Batch update - null pointer / 0 count is safe ---

TestResult TestBatchNullSafe() {
    // Should not crash
    ApplyMaterialParamBatch(nullptr, 0);
    ApplyMaterialParamBatch(nullptr, 10);

    material_param_update_c updates[1];
    updates[0].entity_id = 0xFFFFFFFF; // invalid entity
    updates[0].param_id = 1;
    updates[0].value = 0.5f;
    ApplyMaterialParamBatch(updates, 1); // invalid entity, should be no-op

    return TestResult::Passed;
}

// --- Test 11: Batch update - alpha_cutoff ---

TestResult TestBatchAlphaCutoff() {
    u32 eid = create_entity_with_material();

    material_param_update_c updates[1];
    updates[0].entity_id = eid;
    updates[0].param_id = 3; // alpha_cutoff
    updates[0].value = 0.123f;

    ApplyMaterialParamBatch(updates, 1);

    TEST_ASSERT(std::abs(GetMaterialAlphaCutoff(eid) - 0.123f) < 1e-5f, "Batch alpha_cutoff mismatch");

    cleanup_entity(eid);
    return TestResult::Passed;
}

// --- Test 12: Texture setters on entity without material are no-ops ---

TestResult TestTextureSetterNoMaterial() {
    u32 eid = create_entity();

    // Should not crash
    SetMaterialAlbedoTexture(eid, 42);
    SetMaterialNormalTexture(eid, 42);
    SetMaterialOrmTexture(eid, 42);

    cleanup_entity(eid);
    return TestResult::Passed;
}

// --- Test 13: Batch update across multiple entities ---

TestResult TestBatchMultiEntity() {
    u32 e1 = create_entity_with_material();
    u32 e2 = create_entity_with_material();

    material_param_update_c updates[3];
    updates[0].entity_id = e1;
    updates[0].param_id = 1; // roughness
    updates[0].value = 0.11f;
    updates[1].entity_id = e2;
    updates[1].param_id = 1; // roughness
    updates[1].value = 0.22f;
    updates[2].entity_id = e1;
    updates[2].param_id = 2; // metallic
    updates[2].value = 0.88f;

    ApplyMaterialParamBatch(updates, 3);

    TEST_ASSERT(std::abs(GetMaterialRoughness(e1) - 0.11f) < 1e-5f, "e1 roughness mismatch");
    TEST_ASSERT(std::abs(GetMaterialMetallic(e1) - 0.88f) < 1e-5f, "e1 metallic mismatch");
    TEST_ASSERT(std::abs(GetMaterialRoughness(e2) - 0.22f) < 1e-5f, "e2 roughness mismatch");

    cleanup_entity(e1);
    cleanup_entity(e2);
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("Material API Extended Tests");
    TEST_CASE((*suite), "AlbedoTextureRoundtrip", TestAlbedoTextureRoundtrip);
    TEST_CASE((*suite), "NormalTextureRoundtrip", TestNormalTextureRoundtrip);
    TEST_CASE((*suite), "OrmTextureRoundtrip", TestOrmTextureRoundtrip);
    TEST_CASE((*suite), "InitialTextureValues", TestInitialTextureValues);
    TEST_CASE((*suite), "TextureGetterNoMaterial", TestTextureGetterNoMaterial);
    TEST_CASE((*suite), "EntityCountWithMaterial", TestEntityCountWithMaterial);
    TEST_CASE((*suite), "EntityIdWithMaterial", TestEntityIdWithMaterial);
    TEST_CASE((*suite), "BatchRoughnessMetallic", TestBatchRoughnessMetallic);
    TEST_CASE((*suite), "BatchBaseColor", TestBatchBaseColor);
    TEST_CASE((*suite), "BatchNullSafe", TestBatchNullSafe);
    TEST_CASE((*suite), "BatchAlphaCutoff", TestBatchAlphaCutoff);
    TEST_CASE((*suite), "TextureSetterNoMaterial", TestTextureSetterNoMaterial);
    TEST_CASE((*suite), "BatchMultiEntity", TestBatchMultiEntity);

    TestRunner::RegisterTestSuite(suite);
    TestStats stats = TestRunner::RunAllSuites();

    return stats.failedTests > 0 ? 1 : 0;
}
