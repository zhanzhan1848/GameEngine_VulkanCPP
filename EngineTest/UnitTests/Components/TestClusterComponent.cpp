#include "../TestFramework.h"
#include "Engine/Common/PrimitiveTypes.h"
#include "Engine/Common/Id.h"
#include "Engine/Components/Cluster.h"
#include "Engine/Components/Entity.h"
#include "Engine/Components/Transform.h"
#include "Engine/Components/Script.h"
#include "Engine/Graphics/Nanite/NaniteResourceManager.h"
#include <iostream>

using namespace Engine::Test;
using namespace primal::cluster;
using namespace primal::game_entity;
using namespace primal::graphics::nanite;
using namespace primal::id;
using Engine::Test::TestResult;
using Engine::Test::TestSuite;
using Engine::Test::TestCase;
using Engine::Test::TestStats;

entity create_one_game_entity(primal::math::v3 position = {0, 0, 0}, primal::math::v3 rotation = {0, 0, 0})
{
    primal::transform::init_info transform_info{};

    simd_quatf quat_y = simd_quaternion(rotation.y, simd_make_float3(0, 1, 0)); // yaw
    simd_quatf quat_x = simd_quaternion(rotation.x, simd_make_float3(1, 0, 0)); // pitch
    simd_quatf quat_z = simd_quaternion(rotation.z, simd_make_float3(0, 0, 0));
    // roll
    simd_quatf quat = simd_mul(quat_z, simd_mul(quat_x, quat_y));

    primal::math::v4 rot_quat{
        quat.vector.x,
        quat.vector.y,
        quat.vector.z,
    };

    primal::script::init_info script_info{};

    primal::game_entity::entity_info entity_info{};
    entity_info.transform = &transform_info;
    entity_info.script = &script_info;

    primal::game_entity::entity ntt{ primal::game_entity::create(entity_info) };
    assert(ntt.is_valid());
    return ntt;
}

void remove_game_entity(primal::game_entity::entity_id id)
{
    primal::game_entity::remove(id);
}

TestResult TestEntityDestroyDoesNotImmediatelyFreeResources() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr id_type test_geometry_id = 500;

    auto* resource = manager.GetOrCreateResource(test_geometry_id);
    TEST_ASSERT_NOT_NULL(resource, "Resource should be created");
    TEST_ASSERT_EQ(1u, resource->ref_count.load(), "Initial ref count should be 1");

    entity e1 = create_one_game_entity();
    TEST_ASSERT(e1.is_valid(), "Entity should be valid");

    init_info info{};
    info.geometry_content_id = test_geometry_id;

    component c1 = create(info, e1);
    TEST_ASSERT(c1 != invalid_id, "Component should be created");

    auto* resourceAfterCreate = manager.GetOrCreateResource(test_geometry_id);
    TEST_ASSERT_EQ(resource, resourceAfterCreate, "Should reuse existing resource");
    TEST_ASSERT_EQ(2u, resource->ref_count.load(), "Ref count should be 2 after component create");

    remove(c1);
    TEST_ASSERT_EQ(1u, resource->ref_count.load(), 
        "Ref count should be 1 after component remove (resource still alive)");

    auto* resourceAfterRemove = manager.GetOrCreateResource(test_geometry_id);
    TEST_ASSERT_NOT_NULL(resourceAfterRemove, 
        "Resource should still exist after component removal");

    manager.ReleaseGeometryRef(test_geometry_id);
    auto* resourceAfterFinalRelease = manager.GetOrCreateResource(test_geometry_id);
    TEST_ASSERT_EQ(resourceAfterFinalRelease, nullptr, 
        "Resource should be destroyed after final release");

    remove_game_entity(e1.get_id());
    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestMultipleEntitiesShareOneResource() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr id_type shared_geometry_id = 600;
    constexpr u32 num_entities = 100;

    auto* sharedResource = manager.GetOrCreateResource(shared_geometry_id);
    TEST_ASSERT_NOT_NULL(sharedResource, "Shared resource should be created");
    TEST_ASSERT_EQ(1u, sharedResource->ref_count.load(), "Initial ref count should be 1");

    primal::utl::vector<entity> entities;
    primal::utl::vector<component> components;

    for (u32 i = 0; i < num_entities; ++i) {
        entity e = create_one_game_entity();
        TEST_ASSERT(e.is_valid(), "Entity should be valid");

        init_info info{};
        info.geometry_content_id = shared_geometry_id;

        component c = create(info, e);
        TEST_ASSERT(c != invalid_id, "Component should be created");

        entities.push_back(e);
        components.push_back(c);
    }

    TEST_ASSERT_EQ(1u + num_entities, sharedResource->ref_count.load(), 
        "Ref count should reflect all entity references");

    for (u32 i = 0; i < num_entities / 2; ++i)
 {
        remove(components[i]);
    }

    TEST_ASSERT_EQ(1u + num_entities - (num_entities / 2), sharedResource->ref_count.load(),
        "Ref count should decrease after partial component removal");

    for (u32 i = num_entities / 2; i < num_entities; ++i)
 {
        remove(components[i]);
    }

    TEST_ASSERT_EQ(1u, sharedResource->ref_count.load(),
        "Ref count should be 1 after all component removals");

    manager.ReleaseGeometryRef(shared_geometry_id);
    auto* resourceAfterFinalRelease = manager.GetOrCreateResource(shared_geometry_id);
    TEST_ASSERT_EQ(resourceAfterFinalRelease, nullptr,
        "Resource should be destroyed after final release");

    for (auto& e : entities) {
        remove_game_entity(e.get_id());
    }

    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestComponentGeometryBindingUpdate() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr id_type geometry_id_1 = 700;
    constexpr id_type geometry_id_2 = 701;

    auto* resource1 = manager.GetOrCreateResource(geometry_id_1);
    TEST_ASSERT_NOT_NULL(resource1, "Resource 1 should be created");
    TEST_ASSERT_EQ(1u, resource1->ref_count.load(), "Resource 1 ref count should be 1");

    auto* resource2 = manager.GetOrCreateResource(geometry_id_2);
    TEST_ASSERT_NOT_NULL(resource2, "Resource 2 should be created");
    TEST_ASSERT_EQ(1u, resource2->ref_count.load(), "Resource 2 ref count should be 1");

    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.geometry_content_id = geometry_id_1;

    component c = create(info, e);
    TEST_ASSERT(c != invalid_id, "Component should be created");

    TEST_ASSERT_EQ(2u, resource1->ref_count.load(), 
        "Resource 1 ref count should be 2 after component create");
    TEST_ASSERT_EQ(1u, resource2->ref_count.load(), 
        "Resource 2 ref count should remain 1");

    set_geometry(c, geometry_id_2);

    TEST_ASSERT_EQ(1u, resource1->ref_count.load(), 
        "Resource 1 ref count should be 1 after geometry update");
    TEST_ASSERT_EQ(2u, resource2->ref_count.load(), 
        "Resource 2 ref count should be 2 after geometry update");

    const component_cache* cache = get(c);
    TEST_ASSERT_NOT_NULL(cache, "Cache should not be null");
    TEST_ASSERT_EQ(geometry_id_2, cache->geometry_content_id, 
        "Cache should reflect new geometry ID");

    remove(c);
    TEST_ASSERT_EQ(1u, resource2->ref_count.load(), 
        "Resource 2 ref count should be 1 after component removal");

    remove_game_entity(e.get_id());
    manager.ReleaseGeometryRef(geometry_id_1);
    manager.ReleaseGeometryRef(geometry_id_2);
    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestInvalidComponentOperations() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr component invalid_c = invalid_id;

    set_geometry(invalid_c, 0);
    set_lod_policy(invalid_c,  0.5f, -1);
    set_visibility_flags(invalid_c,  0xFFFFFFFF);

    const component_cache* cache = get(invalid_c);
    TEST_ASSERT_EQ(cache, nullptr, "Invalid component should return null cache");

    remove(invalid_c);

    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestLODPolicyUpdate() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr id_type test_geometry_id = 800;

    auto* resource = manager.GetOrCreateResource(test_geometry_id);
    TEST_ASSERT_NOT_NULL(resource, "Resource should be created");

    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.geometry_content_id = test_geometry_id;
    info.lod_bias = 0.0f;
    info.forced_lod = -1;

    component c = create(info, e);
    TEST_ASSERT(c != invalid_id, "Component should be created");

    const component_cache* cache = get(c);
    TEST_ASSERT_NOT_NULL(cache, "Cache should not be null");
    TEST_ASSERT_EQ(0.0f, cache->lod_bias, "Initial LOD bias should be 0.0");
    TEST_ASSERT_EQ(-1, cache->forced_lod, "Initial forced LOD should be -1");

    set_lod_policy(c, 2.5f, 3);

    cache = get(c);
    TEST_ASSERT_NOT_NULL(cache, "Cache should not be null after update");
    TEST_ASSERT_EQ(2.5f, cache->lod_bias, "LOD bias should be updated to 2.5");
    TEST_ASSERT_EQ(3, cache->forced_lod, "Forced LOD should be updated to 3");
    TEST_ASSERT(cache->flags & 0x02, "LOD update flag should be set");

    set_lod_policy(c, -0.5f, 0);
    cache = get(c);
    TEST_ASSERT_EQ(-00.5f, cache->lod_bias, "LOD bias should handle negative values");
    TEST_ASSERT_EQ(0, cache->forced_lod, "Forced LOD should handle 0");

    remove(c);
    manager.ReleaseGeometryRef(test_geometry_id);
    remove_game_entity(e.get_id());
    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestVisibilityFlagsUpdate() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr id_type test_geometry_id = 801;

    auto* resource = manager.GetOrCreateResource(test_geometry_id);
    TEST_ASSERT_NOT_NULL(resource, "Resource should be created");

    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.geometry_content_id = test_geometry_id;
    info.visibility_flags = 0xFFFFFFFF;

    component c = create(info, e);
    TEST_ASSERT(c != invalid_id, "Component should be created");

    const component_cache* cache = get(c);
    TEST_ASSERT_NOT_NULL(cache, "Cache should not be null");
    TEST_ASSERT_EQ(0xFFFFFFFF, cache->visibility_flags, "Initial visibility flags should be 0xFFFFFFFF");

    set_visibility_flags(c, 0x00000001);
    cache = get(c);
    TEST_ASSERT_NOT_NULL(cache, "Cache should not be null after update");
    TEST_ASSERT_EQ(0x00000001, cache->visibility_flags, "Visibility flags should be updated");

    set_visibility_flags(c, 0x00000000);
    cache = get(c);
    TEST_ASSERT_EQ(0x00000000, cache->visibility_flags, "Visibility flags should handle 0");

    set_visibility_flags(c, 0xABCDEF12);
    cache = get(c);
    TEST_ASSERT_EQ(0xABCDEF12, cache->visibility_flags, "Visibility flags should handle arbitrary values");

    remove(c);
    manager.ReleaseGeometryRef(test_geometry_id);
    remove_game_entity(e.get_id());
    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestBatchUpdate() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr u32 num_components = 10;
    constexpr id_type base_geometry_id = 900;

    primal::utl::vector<entity> entities;
    primal::utl::vector<component> components;

    for (u32 i = 0; i < num_components; ++i) {
        entity e = create_one_game_entity();
        TEST_ASSERT(e.is_valid(), "Entity should be valid");

        init_info info{};
        info.geometry_content_id = base_geometry_id + i;
        info.lod_bias = 0.0f;
        info.forced_lod = -1;

        component c = create(info, e);
        TEST_ASSERT(c != invalid_id, "Component should be created");

        entities.push_back(e);
        components.push_back(c);
    }

    component_cache caches[num_components];
    for (u32 i = 0; i < num_components; ++i)    {
        caches[i] = *get(components[i]);
        caches[i].id = components[i];
        caches[i].lod_bias = i * 0.5f;
        caches[i].forced_lod = i % 4;
        caches[i].flags = 0x02;
    }

    update(caches, num_components);

    for (u32 i = 0; i < num_components; ++i)
    {
        const component_cache* cache = get(components[i]);
        TEST_ASSERT_NOT_NULL(cache, "Cache should not be null");
        TEST_ASSERT_EQ(i * 0.5f, cache->lod_bias, "LOD bias should be updated via batch update");
    }

    for (u32 i = 0; i < num_components; ++i)
    {
        remove(components[i]);
        manager.ReleaseGeometryRef(base_geometry_id + i);
        remove_game_entity(entities[i].get_id());
    }

    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestGetReturnsCorrectCacheData() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr id_type geometry_id = 980;

    auto* resource = manager.GetOrCreateResource(geometry_id);
    TEST_ASSERT_NOT_NULL(resource, "Resource should be created");

    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.geometry_content_id = geometry_id;
    info.lod_bias = 1.5f;
    info.forced_lod = 2;
    info.visibility_flags = 0x12345678;

    component c = create(info, e);
    TEST_ASSERT(c != invalid_id, "Component should be created");

    const component_cache* cache = get(c);
    TEST_ASSERT_NOT_NULL(cache, "Cache should not be null");
    TEST_ASSERT_EQ(geometry_id, cache->geometry_content_id, "Cache should have correct geometry ID");
    TEST_ASSERT_EQ(1.5f, cache->lod_bias, "Cache should have correct LOD bias");
    TEST_ASSERT_EQ(2, cache->forced_lod, "Cache should have correct forced LOD");
    TEST_ASSERT_EQ(0x12345678u, cache->visibility_flags, "Cache should have correct visibility flags");
    TEST_ASSERT(cache->exists, "Cache should exist");
    TEST_ASSERT_EQ(0u, cache->flags, "Cache flags should be 0 initially");

    set_lod_policy(c, 3.0f, 1);
    cache = get(c);
    TEST_ASSERT_EQ(3.0f, cache->lod_bias, "Cache should reflect LOD policy update");
    TEST_ASSERT_EQ(1, cache->forced_lod, "Cache should reflect forced LOD update");

    set_visibility_flags(c, 0x87654321);
    cache = get(c);
    TEST_ASSERT_EQ(0x87654321u, cache->visibility_flags, "Cache should reflect visibility flags update");

    remove(c);
    cache = get(c);
    TEST_ASSERT(cache == nullptr, "get() should return nullptr after component removal");
    
    manager.ReleaseGeometryRef(geometry_id);
    remove_game_entity(e.get_id());
    manager.Shutdown();
    
    return TestResult::Passed;
}

TestResult TestResourceDestructionWhenRefCountReachesZero() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr id_type geometry_id = 850;

    auto* resource = manager.GetOrCreateResource(geometry_id);
    TEST_ASSERT_NOT_NULL(resource, "Resource should be created");
    TEST_ASSERT_EQ(1u, resource->ref_count.load(), "Initial ref count should be 1");

    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.geometry_content_id = geometry_id;

    component c = create(info, e);
    TEST_ASSERT(c != invalid_id, "Component should be created");

    auto* resourceAfterCreate = manager.GetOrCreateResource(geometry_id);
    TEST_ASSERT_EQ(resource, resourceAfterCreate, "Should reuse existing resource");
    TEST_ASSERT_EQ(2u, resource->ref_count.load(), "Ref count should be 2 after component create");

    remove(c);
    TEST_ASSERT_EQ(1u, resource->ref_count.load(), 
        "Ref count should be 1 after component remove (resource still alive)");

    auto* resourceAfterRemove = manager.GetOrCreateResource(geometry_id);
    TEST_ASSERT_NOT_NULL(resourceAfterRemove, 
        "Resource should still exist after component remove");

    manager.ReleaseGeometryRef(geometry_id);
    auto* resourceAfterFinalRelease = manager.GetOrCreateResource(geometry_id);
    TEST_ASSERT_EQ(resourceAfterFinalRelease, nullptr, 
        "Resource should return nullptr after final release");
    
    remove_game_entity(e.get_id());
    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestDuplicateCreateOperations() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr id_type geometry_id = 860;

    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.geometry_content_id = geometry_id;

    component c1 = create(info, e);
    TEST_ASSERT(c1 != invalid_id, "First component should be created");

    component c2 = create(info, e);
    TEST_ASSERT(c2 != invalid_id, "Second create should return existing component");
    TEST_ASSERT_EQ(c1, c2, "Duplicate create should return same component");

    auto* resource = manager.GetOrCreateResource(geometry_id);
    TEST_ASSERT_NOT_NULL(resource, "Resource should exist");
    TEST_ASSERT_EQ(1u, resource->ref_count.load(), "Ref count should remain 1 (component creation doesn't increase ref count)");

    remove(c1);
    manager.ReleaseGeometryRef(geometry_id);
    remove_game_entity(e.get_id());
    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestDuplicateRemoveOperations() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr id_type geometry_id = 870;

    auto* resource = manager.GetOrCreateResource(geometry_id);
    TEST_ASSERT_NOT_NULL(resource, "Resource should be created");

    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.geometry_content_id = geometry_id;

    component c = create(info, e);
    TEST_ASSERT(c != invalid_id, "Component should be created");
    TEST_ASSERT_EQ(2u, resource->ref_count.load(), "Ref count should be 2");

    remove(c);
    TEST_ASSERT_EQ(1u, resource->ref_count.load(), "Ref count should be 1 after first remove");

    remove(c);
    TEST_ASSERT_EQ(1u, resource->ref_count.load(), "Duplicate remove should not affect ref count");

    manager.ReleaseGeometryRef(geometry_id);
    remove_game_entity(e.get_id());
    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestResourceRecreationAfterDestruction() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr id_type geometry_id = 880;

    auto* resource1 = manager.GetOrCreateResource(geometry_id);
    TEST_ASSERT_NOT_NULL(resource1, "Resource 1 should be created");
    TEST_ASSERT_EQ(1u, resource1->ref_count.load(), "Resource 1 ref count should be 1");

    manager.ReleaseGeometryRef(geometry_id);
    
    auto* resourceAfterDestruction = manager.GetOrCreateResource(geometry_id);
    TEST_ASSERT_EQ(resourceAfterDestruction, nullptr, 
        "GetOrCreateResource should return nullptr after destruction");
    
    manager.Shutdown();

    auto* resource2 = manager.GetOrCreateResource(geometry_id);
    TEST_ASSERT_NOT_NULL(resource2, "Resource 2 should be created after shutdown");
    TEST_ASSERT_EQ(1u, resource2->ref_count.load(), "Recreated resource ref count should be 1");

    manager.ReleaseGeometryRef(geometry_id);
    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestPerformanceLargeScaleCreate() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr u32 num_entities = 1000;
    constexpr id_type shared_geometry_id = 950;

    auto* resource = manager.GetOrCreateResource(shared_geometry_id);
    TEST_ASSERT_NOT_NULL(resource, "Resource should be created");

    auto start = std::chrono::high_resolution_clock::now();

    primal::utl::vector<entity> entities;
    primal::utl::vector<component> components;

    for (u32 i = 0; i < num_entities; ++i) {
        entity e = create_one_game_entity();
        TEST_ASSERT(e.is_valid(), "Entity should be valid");

        init_info info{};
        info.geometry_content_id = shared_geometry_id;

        component c = create(info, e);
        TEST_ASSERT(c != invalid_id, "Component should be created");

        entities.push_back(e);
        components.push_back(c);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    TEST_ASSERT(duration.count() < 100, "1000 creates should take < 100ms");
    std::cout << "    [PERF] 1000 creates took " << duration.count() << "ms" << std::endl;

    TEST_ASSERT_EQ(1u + num_entities, resource->ref_count.load(), "All components should reference resource");

    for (u32 i = 0; i < num_entities; ++i)    {
        remove(components[i]);
        remove_game_entity(entities[i].get_id());
    }

    manager.ReleaseGeometryRef(shared_geometry_id);
    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestPerformanceLargeScaleRemove() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr u32 num_entities = 1000;
    constexpr id_type shared_geometry_id = 951;

    auto* resource = manager.GetOrCreateResource(shared_geometry_id);
    TEST_ASSERT_NOT_NULL(resource, "Resource should be created");

    primal::utl::vector<entity> entities;
    primal::utl::vector<component> components;

    for (u32 i = 0; i < num_entities; ++i)    {
        entity e = create_one_game_entity();
        init_info info{};
        info.geometry_content_id = shared_geometry_id;
        component c = create(info, e);
        entities.push_back(e);
        components.push_back(c);
    }

    auto start = std::chrono::high_resolution_clock::now();

    for (u32 i = 0; i < num_entities; ++i)
    {
        remove(components[i]);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    TEST_ASSERT(duration.count() < 50, "1000 removes should take < 50ms");
    std::cout << "    [PERF] 1000 removes took " << duration.count() << "ms" << std::endl;

    TEST_ASSERT_EQ(1u, resource->ref_count.load(), "All components should be removed");

    for (u32 i = 0; i < num_entities; ++i)
    {
        manager.ReleaseGeometryRef(shared_geometry_id);
        remove_game_entity(entities[i].get_id());
    }
    manager.Shutdown();

    return TestResult::Passed;
}

TestResult TestPerformanceGeometryBindingSwitch() {
    auto& manager = NaniteResourceManager::Get();
    manager.Shutdown();

    constexpr u32 num_switches = 1000;
    constexpr id_type geometry_id_1 = 960;
    constexpr id_type geometry_id_2 = 961;

    auto* resource1 = manager.GetOrCreateResource(geometry_id_1);
    auto* resource2 = manager.GetOrCreateResource(geometry_id_2);
    TEST_ASSERT_NOT_NULL(resource1, "Resource 1 should be created");
    TEST_ASSERT_NOT_NULL(resource2, "Resource 2 should be created");

    entity e = create_one_game_entity();
    TEST_ASSERT(e.is_valid(), "Entity should be valid");

    init_info info{};
    info.geometry_content_id = geometry_id_1;

    component c = create(info, e);
    TEST_ASSERT(c != invalid_id, "Component should be created");

    auto start = std::chrono::high_resolution_clock::now();

    for (u32 i = 0; i < num_switches; ++i)    {
        id_type target_id = (i % 2 == 0) ? geometry_id_2 : geometry_id_1;
        set_geometry(c, target_id);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    TEST_ASSERT(duration.count() < 50, "1000 geometry switches should take < 50ms");
    std::cout << "    [PERF] 1000 geometry switches took " << duration.count() << "ms" << std::endl;

    remove(c);
    manager.ReleaseGeometryRef(geometry_id_1);
    manager.ReleaseGeometryRef(geometry_id_2);
    remove_game_entity(e.get_id());
    manager.Shutdown();

    return TestResult::Passed;
}

int main() {
    TestSuite suite("ClusterComponent Tests");

    suite.AddTestCase(TestCase("EntityDestroyDoesNotImmediatelyFreeResources", 
        TestEntityDestroyDoesNotImmediatelyFreeResources));
    suite.AddTestCase(TestCase("MultipleEntitiesShareOneResource", 
        TestMultipleEntitiesShareOneResource));
    suite.AddTestCase(TestCase("ComponentGeometryBindingUpdate", 
        TestComponentGeometryBindingUpdate));
    suite.AddTestCase(TestCase("InvalidComponentOperations", 
        TestInvalidComponentOperations));

    suite.AddTestCase(TestCase("LODPolicyUpdate", 
        TestLODPolicyUpdate));
    suite.AddTestCase(TestCase("VisibilityFlagsUpdate", 
        TestVisibilityFlagsUpdate));
    suite.AddTestCase(TestCase("BatchUpdate", 
        TestBatchUpdate));
    suite.AddTestCase(TestCase("GetReturnsCorrectCacheData", 
        TestGetReturnsCorrectCacheData));

    suite.AddTestCase(TestCase("ResourceDestructionWhenRefCountReachesZero", 
        TestResourceDestructionWhenRefCountReachesZero));
    suite.AddTestCase(TestCase("DuplicateCreateOperations", 
        TestDuplicateCreateOperations));
    suite.AddTestCase(TestCase("DuplicateRemoveOperations", 
        TestDuplicateRemoveOperations));
    suite.AddTestCase(TestCase("ResourceRecreationAfterDestruction", 
        TestResourceRecreationAfterDestruction));

    suite.AddTestCase(TestCase("PerformanceLargeScaleCreate", 
        TestPerformanceLargeScaleCreate));
    suite.AddTestCase(TestCase("PerformanceLargeScaleRemove", 
        TestPerformanceLargeScaleRemove));
    suite.AddTestCase(TestCase("PerformanceGeometryBindingSwitch", 
        TestPerformanceGeometryBindingSwitch));

    TestStats stats = suite.RunAllTests();

    std::cout << "\n=== ClusterComponent Tests ===" << std::endl;
    if (stats.failedTests == 0) {
        std::cout << "✅ All ClusterComponent tests passed!" << std::endl;
        std::cout << "   Total tests: " << stats.totalTests << std::endl;
        std::cout << "   Total time: " << stats.totalTime << "ms" << std::endl;
    } else {
        std::cout << "❌ " << stats.failedTests << " test(s) failed." << std::endl;
    }

    return stats.failedTests == 0 ? 1 : 0;
};
