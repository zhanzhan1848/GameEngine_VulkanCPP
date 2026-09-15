#include "ParticleFXN1Tests.h"
#include "ParticleFXN2Tests.h"
#include "ParticleFXN3Tests.h"
#include "ParticleFXN4Tests.h"
#include "ParticleFXN6Tests.h"
#include "ParticleFXParityTests.h"
#include "ParticleFXTests.h"

int main() {
    primal::pfx::test::run_all_tests();      // N0
    primal::pfx::test::run_all_n1_tests();   // N1
    primal::pfx::test::run_all_n2_tests();   // N2
    primal::pfx::test::run_all_n3_tests();   // N3
    primal::pfx::test::run_all_n4_tests();   // N4
    primal::pfx::test::run_all_n6_tests();   // N6
    primal::pfx::test::run_all_parity_tests(); // Niagara/POP 对齐批次
    return 0;
}
