#include "CommonHeaders.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RHI/Core/RHIGeometry.h"
#include "TestFramework.h"
#include <iostream>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace Engine::Test;

// --- Helper for mocking RenderMesh ---
namespace primal::graphics {
    class RenderMeshTestHelper {
    public:
        static RenderMesh* CreateMockMesh(primal::id::id_type entityId, const AABB& localAABB) {
            RenderMesh* mesh = new RenderMesh();
            mesh->entityId_ = entityId;
            mesh->localAABB_ = localAABB;
            mesh->Register();
            return mesh;
        }

        static void DestroyMockMesh(RenderMesh* mesh) {
            if (mesh) {
                mesh->Unregister();
                delete mesh;
            }
        }
    };
}

// --- Test Cases ---

TestResult TestRenderProxyCreation() {
    RenderProxy proxy;
    
    TEST_ASSERT(proxy.meshId == primal::id::invalid_id, "Default meshId should be invalid");
    TEST_ASSERT(proxy.materialId == primal::id::invalid_id, "Default materialId should be invalid");
    TEST_ASSERT(proxy.entityId == primal::id::invalid_id, "Default entityId should be invalid");
    
    math::v3 translation{1.0f, 2.0f, 3.0f};
    proxy.transform = math::CreateTranslationMatrix(translation);
    
    return TestResult::Passed;
}

TestResult TestRenderProxyValues() {
    primal::id::id_type entity = 100;
    primal::id::id_type mesh = 200;
    primal::id::id_type material = 300;
    
    RenderProxy proxy(entity, mesh, material);
    
    TEST_ASSERT(proxy.entityId == entity, "Entity ID mismatch");
    TEST_ASSERT(proxy.meshId == mesh, "Mesh ID mismatch");
    TEST_ASSERT(proxy.materialId == material, "Material ID mismatch");
    
    return TestResult::Passed;
}

TestResult TestRenderProxyFactory() {
    primal::id::id_type entity = 101;
    primal::id::id_type mesh = 201;
    primal::id::id_type material = 301;
    
    RenderProxy proxy = RenderProxy::Create(entity, mesh, material);
    
    TEST_ASSERT(proxy.entityId == entity, "Factory Entity ID mismatch");
    TEST_ASSERT(proxy.meshId == mesh, "Factory Mesh ID mismatch");
    TEST_ASSERT(proxy.materialId == material, "Factory Material ID mismatch");
    
    return TestResult::Passed;
}

TestResult TestRenderProxyUpdateTransform() {
    RenderProxy proxy;
    
    // Verify initial transform is identity/zero translation
    
    math::v3 newTranslation{10.f, 20.f, 30.f};
    math::m4x4 newTransform = math::CreateTranslationMatrix(newTranslation);
    
    proxy.UpdateTransform(newTransform);
    
    // Check if translation component matches
    TEST_ASSERT(proxy.transform.columns[3][0] == 10.f, "Transform X mismatch");
    TEST_ASSERT(proxy.transform.columns[3][1] == 20.f, "Transform Y mismatch");
    TEST_ASSERT(proxy.transform.columns[3][2] == 30.f, "Transform Z mismatch");
    
    return TestResult::Passed;
}

TestResult TestRenderProxySetMaterial() {
    RenderProxy proxy;
    primal::id::id_type newMaterial = 500;
    
    TEST_ASSERT(proxy.materialId == primal::id::invalid_id, "Initial material should be invalid");
    
    proxy.SetMaterial(newMaterial);
    TEST_ASSERT(proxy.materialId == newMaterial, "Material ID update failed");
    
    return TestResult::Passed;
}

TestResult TestRenderProxyAABB() {
    // 1. Test Default AABB (No Mesh)
    {
        RenderProxy proxy = RenderProxy::Create(1, 999, 1); // Mesh 999 doesn't exist
        // Default AABB is usually small around origin, transformed by identity
        TEST_ASSERT(proxy.worldAABB.IsValid(), "Default AABB should be valid");
        
        // Update transform and check if AABB moves
        math::v3 translation{100.f, 0.f, 0.f};
        proxy.UpdateTransform(math::CreateTranslationMatrix(translation));
        
        math::v3 center = proxy.worldAABB.Center();
        TEST_ASSERT(std::abs(center.x - 100.f) < 0.2f, "Default AABB should move with transform");
    }

    // 2. Test AABB with Mock Mesh
    {
        primal::id::id_type meshId = 888;
        // Create a mesh with known AABB: [-1, -1, -1] to [1, 1, 1]
        AABB localAABB(math::v3{-1.f, -1.f, -1.f}, math::v3{1.f, 1.f, 1.f});
        RenderMesh* mockMesh = RenderMeshTestHelper::CreateMockMesh(meshId, localAABB);

        RenderProxy proxy = RenderProxy::Create(2, meshId, 1);
        
        // Initial check (Identity transform)
        TEST_ASSERT(proxy.worldAABB.min.x == -1.f && proxy.worldAABB.max.x == 1.f, "Initial World AABB should match Local AABB for Identity transform");

        // Scale by 2 and Translate by (10, 0, 0)
        // Matrix: Scale(2) -> Translate(10)
        // Point (1, 1, 1) -> (2, 2, 2) -> (12, 2, 2)
        // Point (-1, -1, -1) -> (-2, -2, -2) -> (8, -2, -2)
        
        // Manually construct identity-like matrix
        math::m4x4 scale;
        memset(&scale, 0, sizeof(scale));
        scale.columns[0][0] = 2.f;
        scale.columns[1][1] = 2.f;
        scale.columns[2][2] = 2.f;
        scale.columns[3][3] = 1.f;
        
        // math::m4x4 translation = math::CreateTranslationMatrix(math::v3{10.f, 0.f, 0.f});
        
        // Construct combined matrix manually
        math::m4x4 combined;
        memset(&combined, 0, sizeof(combined));
        combined.columns[0][0] = 2.f;
        combined.columns[1][1] = 2.f;
        combined.columns[2][2] = 2.f;
        combined.columns[3] = math::v4{10.f, 0.f, 0.f, 1.f};

        proxy.UpdateTransform(combined);
        
        // Check new AABB
        // Min should be roughly (8, -2, -2)
        // Max should be roughly (12, 2, 2)
        
        // Allow some epsilon for floating point
        TEST_ASSERT(std::abs(proxy.worldAABB.min.x - 8.f) < 0.001f, "AABB Min X incorrect after transform");
        TEST_ASSERT(std::abs(proxy.worldAABB.max.x - 12.f) < 0.001f, "AABB Max X incorrect after transform");

        RenderMeshTestHelper::DestroyMockMesh(mockMesh);
    }
    
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("RenderProxy Tests");
    TEST_CASE((*suite), "TestRenderProxyCreation", TestRenderProxyCreation);
    TEST_CASE((*suite), "TestRenderProxyValues", TestRenderProxyValues);
    TEST_CASE((*suite), "TestRenderProxyFactory", TestRenderProxyFactory);
    TEST_CASE((*suite), "TestRenderProxyUpdateTransform", TestRenderProxyUpdateTransform);
    TEST_CASE((*suite), "TestRenderProxySetMaterial", TestRenderProxySetMaterial);
    TEST_CASE((*suite), "TestRenderProxyAABB", TestRenderProxyAABB);
    
    TestRunner::RegisterTestSuite(suite);
    TestStats stats = TestRunner::RunAllSuites();
    
    return stats.failedTests > 0 ? 1 : 0;
}
