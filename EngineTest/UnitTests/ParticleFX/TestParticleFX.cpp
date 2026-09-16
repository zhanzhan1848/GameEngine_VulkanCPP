#include "ParticleFXN1Tests.h"
#include "ParticleFXN2Tests.h"
#include "ParticleFXN3Tests.h"
#include "ParticleFXN4Tests.h"
#include "ParticleFXN6Tests.h"
#include "ParticleFXParityTests.h"
#include "ParticleFXEditorPreviewTests.h"
#include "ParticleFXMeshLocationTests.h"   // 依赖 N1Tests.h 的 make_program_emitter
#include "ParticleFXInstancePhysicsTests.h"
#include "ParticleFXPhysicsBindingsTests.h"
#include "ParticleFXTests.h"

int main() {
    primal::pfx::test::run_all_tests();      // N0
    primal::pfx::test::run_all_n1_tests();   // N1
    primal::pfx::test::run_all_mesh_location_tests();   // ShapeLocation mesh 发射源
    primal::pfx::test::run_all_instance_physics_tests(); // instance 声明 + PhysicsCollision
    primal::pfx::test::run_all_physics_bindings_tests(); // 逐粒子刚体绑定(P2/P3)
    primal::pfx::test::run_all_n2_tests();   // N2
    primal::pfx::test::run_all_n3_tests();   // N3
    primal::pfx::test::run_all_n4_tests();   // N4
    primal::pfx::test::run_all_n6_tests();   // N6
    primal::pfx::test::run_all_parity_tests(); // Niagara/POP 对齐批次
    primal::pfx::test::run_all_editor_preview_tests(); // 编辑器私有预览渲染隔离
    return 0;
}
