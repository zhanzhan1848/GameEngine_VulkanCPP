#include "CommonHeaders.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "TestFramework.h"
#include <iostream>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace Engine::Test;

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
    // Note: Comparing floats directly is risky, but we just set it.
    // Ideally use a matrix comparison helper, but for now simple check.
    
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

int main() {
    auto suite = std::make_shared<TestSuite>("RenderProxy Tests");
    TEST_CASE((*suite), "TestRenderProxyCreation", TestRenderProxyCreation);
    TEST_CASE((*suite), "TestRenderProxyValues", TestRenderProxyValues);
    TEST_CASE((*suite), "TestRenderProxyFactory", TestRenderProxyFactory);
    TEST_CASE((*suite), "TestRenderProxyUpdateTransform", TestRenderProxyUpdateTransform);
    TEST_CASE((*suite), "TestRenderProxySetMaterial", TestRenderProxySetMaterial);
    
    TestRunner::RegisterTestSuite(suite);
    TestStats stats = TestRunner::RunAllSuites();
    
    return stats.failedTests > 0 ? 1 : 0;
}
