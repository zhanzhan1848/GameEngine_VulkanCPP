#include "HZBTestUtils.h"
#include <chrono>
#include <iomanip>

namespace primal::graphics::nanite {

// Additional implementation methods if needed
void HZBTestUtils::RunComprehensiveHZBTests(
    HZBSystem& hzb_system,
    DepthHistoryManager& depth_manager,
    rhi::RHIDeviceBase* device)
{
    std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
    std::cout << "║     HZB System Comprehensive Test Suite      ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;

    auto start_time = std::chrono::high_resolution_clock::now();

    // Run all tests
    bool all_tests_passed = true;

    all_tests_passed &= TestHZBSystem(hzb_system, depth_manager, device);

    // Generate debug report
    GenerateHZBDebugReport(hzb_system, depth_manager, "HZB_Debug_Report.txt");

    // Performance analysis
    TestHZBGenerationPerformance(hzb_system, device);

    // Visualization
    VisualizeHZBLevels(hzb_system);

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << std::endl;
    std::cout << "=== Test Summary ===" << std::endl;
    std::cout << "Total Test Time: " << duration.count() << " ms" << std::endl;
    std::cout << "Overall Result: " << (all_tests_passed ? "✅ PASSED" : "❌ FAILED") << std::endl;
    std::cout << std::endl;

    // Expected performance metrics
    std::cout << "=== Expected Performance Metrics ===" << std::endl;
    std::cout << "HZB Generation Time:  < 1.0 ms @ 1080p" << std::endl;
    std::cout << "Occlusion Culling:   > 25% culling rate" << std::endl;
    std::cout << "Total GPU Savings:    > 20% time reduction" << std::endl;
    std::cout << "Memory Usage:         < 10 MB total" << std::endl;
    std::cout << std::endl;
}

} // namespace primal::graphics::nanite