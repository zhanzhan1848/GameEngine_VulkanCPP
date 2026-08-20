// stale-test port: GPUBufferComponent evolved from a device-backed helper
// (Create/Map/Unmap/Upload against an RHIDeviceBase mock) into a plain data
// struct holding vertex/index buffer handles. The old device-interaction tests
// no longer have an API to exercise; the suite below validates the current
// struct semantics (defaults, IsValid, HasIndexBuffer) instead.
#include "EngineTest/UnitTests/TestFramework.h"
#include "Engine/Graphics/RHI/Components/GPUBufferComponent.h"

using namespace primal::graphics::rhi;
using namespace Engine::Test;

class TestGPUBufferComponent : public TestSuite {
public:
    TestGPUBufferComponent() : TestSuite("GPUBufferComponentTests") {
        AddTestCase(TestCase("Constructor Initialization", [this]() { return TestConstructor(); }));
        AddTestCase(TestCase("Validity Check", [this]() { return TestValidity(); }));
        AddTestCase(TestCase("Index Buffer Check", [this]() { return TestIndexBuffer(); }));
    }

private:
    TestResult TestConstructor() {
        GPUBufferComponent defaultComp;
        TEST_ASSERT(defaultComp.vertexBuffer == handles::INVALID_RESOURCE, "Default vertexBuffer should be INVALID_RESOURCE");
        TEST_ASSERT(defaultComp.indexBuffer == handles::INVALID_RESOURCE, "Default indexBuffer should be INVALID_RESOURCE");
        TEST_ASSERT(defaultComp.vertexCount == 0, "Default vertexCount should be 0");
        TEST_ASSERT(defaultComp.indexCount == 0, "Default indexCount should be 0");
        TEST_ASSERT(defaultComp.offset == 0, "Default offset should be 0");
        TEST_ASSERT(defaultComp.indexOffset == 0, "Default indexOffset should be 0");
        TEST_ASSERT(defaultComp.indexType == DataFormat::R32_UInt, "Default indexType should be R32_UInt");
        return TestResult::Passed;
    }

    TestResult TestValidity() {
        GPUBufferComponent comp;
        TEST_ASSERT(!comp.IsValid(), "Empty component should be invalid");
        TEST_ASSERT(!comp.HasIndexBuffer(), "Empty component should have no index buffer");

        // stale-test port: handles are plain u64 now; assign a fake non-invalid handle.
        comp.vertexBuffer = 1;
        comp.vertexCount = 100;
        TEST_ASSERT(comp.IsValid(), "Component with buffer handle and count should be valid");
        TEST_ASSERT(!comp.HasIndexBuffer(), "Index buffer should still be absent");

        comp.indexBuffer = 2;
        comp.indexCount = 50;
        TEST_ASSERT(comp.HasIndexBuffer(), "Component with index handle and count should report index buffer");
        return TestResult::Passed;
    }

    TestResult TestIndexBuffer() {
        GPUBufferComponent comp;
        comp.vertexBuffer = 1;
        comp.vertexCount = 3;
        comp.indexBuffer = 2;
        comp.indexOffset = 64;
        comp.indexCount = 12;
        comp.indexType = DataFormat::R16_UInt;

        TEST_ASSERT(comp.indexType == DataFormat::R16_UInt, "indexType should round-trip");
        TEST_ASSERT(comp.indexOffset == 64, "indexOffset should round-trip");
        TEST_ASSERT(comp.HasIndexBuffer(), "Index buffer should be reported");

        // Handle reset restores invalid state
        comp.indexBuffer = handles::INVALID_RESOURCE;
        TEST_ASSERT(!comp.HasIndexBuffer(), "Invalidated index handle disables index buffer");
        return TestResult::Passed;
    }
};

int main() {
    TestGPUBufferComponent suite;
    TestStats stats = suite.RunAllTests();
    return stats.failedTests > 0 ? 1 : 0;
}
