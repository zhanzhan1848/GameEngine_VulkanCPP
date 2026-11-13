#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIEntityManager.h"
#include "Graphics/RHI/Components/RenderLayerComponent.h"
#include "Graphics/RHI/Components/MaterialComponent.h"
#include "Graphics/RHI/Components/GPUBufferComponent.h"

using namespace primal::graphics::rhi;
using namespace Engine::Test;

TestResult TestCreateAndDestroyEntity() {
    RHIEntityManager manager;
    manager.Clear();

    RHIEntityID entity = manager.CreateEntity();
    TEST_ASSERT(entity != INVALID_RHI_ENTITY_ID, "Entity ID should be valid");
    TEST_ASSERT(manager.IsAlive(entity), "Entity should be alive");

    manager.DestroyEntity(entity);
    TEST_ASSERT(!manager.IsAlive(entity), "Entity should be dead after destruction");
    
    RHIEntityID entity2 = manager.CreateEntity();
    // In current implementation, freeIndices are used, so it might recycle index.
    // If implementation changes to not recycle immediately, this assert might fail, 
    // but for now we expect recycling or at least valid ID.
    TEST_ASSERT(entity2 != INVALID_RHI_ENTITY_ID, "Entity 2 ID should be valid");
    
    return TestResult::Passed;
}

TestResult TestComponentManagement() {
    RHIEntityManager manager;
    manager.Clear();
    RHIEntityID entity = manager.CreateEntity();

    auto& layer = manager.AddComponent<RenderLayerComponent>(entity);
    layer.layerMask = 0x1;
    layer.priority = 10;

    TEST_ASSERT(manager.HasComponent<RenderLayerComponent>(entity), "Should have RenderLayerComponent");
    TEST_ASSERT(!manager.HasComponent<MaterialComponent>(entity), "Should not have MaterialComponent");

    auto* pLayer = manager.GetComponent<RenderLayerComponent>(entity);
    TEST_ASSERT(pLayer != nullptr, "GetComponent should return pointer");
    TEST_ASSERT(pLayer->priority == 10, "Component data should be preserved");

    manager.RemoveComponent<RenderLayerComponent>(entity);
    TEST_ASSERT(!manager.HasComponent<RenderLayerComponent>(entity), "Should not have RenderLayerComponent after removal");

    return TestResult::Passed;
}

TestResult TestMultipleEntities() {
    RHIEntityManager manager;
    manager.Clear();
    RHIEntityID e1 = manager.CreateEntity();
    RHIEntityID e2 = manager.CreateEntity();

    manager.AddComponent<RenderLayerComponent>(e1).priority = 1;
    manager.AddComponent<RenderLayerComponent>(e2).priority = 2;

    TEST_ASSERT(manager.GetComponent<RenderLayerComponent>(e1)->priority == 1, "Entity 1 priority mismatch");
    TEST_ASSERT(manager.GetComponent<RenderLayerComponent>(e2)->priority == 2, "Entity 2 priority mismatch");

    return TestResult::Passed;
}

TestResult TestEnsureCapacity() {
    RHIEntityManager manager;
    manager.Clear();
    
    // Create enough entities to trigger resize (assuming initial size is small, but even if 256, we can try)
    // Actually testing logic correctness rather than internal vector size.
    std::vector<RHIEntityID> entities;
    for(int i=0; i<300; ++i) {
        entities.push_back(manager.CreateEntity());
    }
    
    TEST_ASSERT(manager.IsAlive(entities[299]), "Last entity should be alive");
    
    // Check component on resized storage
    manager.AddComponent<RenderLayerComponent>(entities[299]).priority = 999;
    TEST_ASSERT(manager.GetComponent<RenderLayerComponent>(entities[299])->priority == 999, "Component on resized storage failed");

    return TestResult::Passed;
}

int main() {
    TestSuite suite("RHIEntityManager Tests");
    
    suite.AddTestCase({"CreateAndDestroyEntity", TestCreateAndDestroyEntity});
    suite.AddTestCase({"ComponentManagement", TestComponentManagement});
    suite.AddTestCase({"MultipleEntities", TestMultipleEntities});
    suite.AddTestCase({"EnsureCapacity", TestEnsureCapacity});
    
    suite.RunAllTests();
    
    return 0;
}
