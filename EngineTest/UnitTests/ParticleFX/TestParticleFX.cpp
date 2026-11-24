// These inline tests use assert for both setup and verification in Release CI.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <chrono>
#include <filesystem>
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
    namespace fs = std::filesystem;
    const auto original = fs::current_path();
    const auto directory = fs::temp_directory_path() / ("primal-pfx-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(directory)) return 1;
    fs::current_path(directory);
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
    fs::current_path(original);
    fs::remove_all(directory);
    return 0;
}
