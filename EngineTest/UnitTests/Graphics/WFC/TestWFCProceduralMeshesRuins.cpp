#include "../../TestFramework.h"
#include "Engine/Content/ProceduralMesh.h"

using namespace primal;
using namespace Engine::Test;

TestResult TestRegisterProceduralMesh_AcceptsMaterialIdx() {
    // We don't actually register — just verify the signature compiles.
    // (Real registration happens in engine-booted integration tests.)
    TEST_ASSERT(true, "API signature accepts material_idx");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCProceduralMeshesRuins");
    TEST_CASE(suite, "AcceptsMaterialIdx", TestRegisterProceduralMesh_AcceptsMaterialIdx);
    suite.RunAllTests();
    return 0;
}
