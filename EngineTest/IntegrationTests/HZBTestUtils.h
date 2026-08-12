#pragma once

#include "Engine/Graphics/Nanite/HZBSystem.h"
#include "Engine/Graphics/Nanite/DepthHistoryManager.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include <iostream>
#include <fstream>

namespace primal::graphics::nanite {

/**
 * @brief HZB Testing and Debugging Utilities
 */
class HZBTestUtils {
public:
    /**
     * @brief Test HZB System functionality
     */
    static bool TestHZBSystem(HZBSystem& hzb_system, DepthHistoryManager& depth_manager, rhi::RHIDeviceBase* device) {
        std::cout << "=== HZB System Testing ===" << std::endl;

        // Test 1: System Initialization
        if (!TestInitialization(hzb_system, depth_manager)) {
            return false;
        }

        // Test 2: HZB Resource Validation
        if (!TestHZBResources(hzb_system)) {
            return false;
        }

        // Test 3: Depth History Management
        if (!TestDepthHistory(depth_manager)) {
            return false;
        }

        // Test 4: Configuration Queries
        if (!TestConfigurationQueries(hzb_system, depth_manager)) {
            return false;
        }

        std::cout << "=== HZB System Tests Completed Successfully ===" << std::endl;
        return true;
    }

    /**
     * @brief Generate HZB debug report
     */
    static void GenerateHZBDebugReport(HZBSystem& hzb_system, DepthHistoryManager& depth_manager, const std::string& filename) {
        std::ofstream report(filename);
        if (!report.is_open()) {
            std::cerr << "Failed to open report file: " << filename << std::endl;
            return;
        }

        report << "=== HZB System Debug Report ===" << std::endl;
        report << "Generated: " << GetCurrentTime() << std::endl << std::endl;

        // HZB System Information
        const auto& hzb_config = hzb_system.GetConfig();
        report << "HZB System Configuration:" << std::endl;
        report << "  Max Resolution: " << hzb_config.max_width << "x" << hzb_config.max_height << std::endl;
        report << "  Min Mip Size: " << hzb_config.min_mip_size << std::endl;
        report << "  Mip Levels: " << hzb_system.GetMipLevels() << std::endl;
        report << "  GPU Generation: " << (hzb_config.generate_on_gpu ? "Enabled" : "Disabled") << std::endl;
        report << "  Compression: " << (hzb_config.enable_compression ? "Enabled" : "Disabled") << std::endl;
        report << "  Ready: " << (hzb_system.IsReady() ? "Yes" : "No") << std::endl;
        report << std::endl;

        // Depth History Manager Information
        const auto& depth_config = depth_manager.GetConfig();
        report << "Depth History Manager Configuration:" << std::endl;
        report << "  Resolution: " << depth_config.width << "x" << depth_config.height << std::endl;
        report << "  Format: " << static_cast<int>(depth_config.format) << std::endl;
        report << "  Buffer Count: " << depth_config.buffer_count << std::endl;
        report << "  Initialized: " << (depth_manager.IsInitialized() ? "Yes" : "No") << std::endl;
        report << std::endl;

        // Mip Level Details
        report << "HZB Mip Level Details:" << std::endl;
        for (u32 mip = 0; mip < hzb_system.GetMipLevels(); ++mip) {
            auto dims = hzb_system.GetMipDimensions(mip);
            report << "  Mip " << mip << ": " << dims.x << "x" << dims.y << std::endl;
        }
        report << std::endl;

        // Memory Usage Estimates
        report << "Memory Usage Estimates:" << std::endl;
        u64 hzb_memory = EstimateHZBMemoryUsage(hzb_system);
        u64 depth_memory = EstimateDepthMemoryUsage(depth_manager);
        report << "  HZB Memory: " << (hzb_memory / 1024.0 / 1024.0) << " MB" << std::endl;
        report << "  Depth History Memory: " << (depth_memory / 1024.0 / 1024.0) << " MB" << std::endl;
        report << "  Total: " << ((hzb_memory + depth_memory) / 1024.0 / 1024.0) << " MB" << std::endl;

        report.close();
        std::cout << "HZB debug report written to: " << filename << std::endl;
    }

    /**
     * @brief Test HZB generation performance
     */
    static void TestHZBGenerationPerformance(HZBSystem& hzb_system, rhi::RHIDeviceBase* device) {
        std::cout << "=== HZB Generation Performance Test ===" << std::endl;

        // This would require actual depth buffer data and command buffers
        // For now, we can only test the configuration
        std::cout << "Performance testing requires runtime integration with actual rendering." << std::endl;
        std::cout << "Expected performance targets:" << std::endl;
        std::cout << "  HZB Generation: < 1ms @ 1080p" << std::endl;
        std::cout << "  Occlusion Culling: > 25% culling rate" << std::endl;
        std::cout << "  Total GPU Savings: > 20% time reduction" << std::endl;
    }

    /**
     * @brief Visual HZB validation (for debugging)
     */
    static void VisualizeHZBLevels(HZBSystem& hzb_system) {
        std::cout << "=== HZB Level Visualization ===" << std::endl;

        for (u32 mip = 0; mip < hzb_system.GetMipLevels(); ++mip) {
            auto dims = hzb_system.GetMipDimensions(mip);
            std::cout << "Mip " << mip << ": " << static_cast<u32>(dims.x) << "x" << static_cast<u32>(dims.y);

            // Calculate coverage percentage
            float coverage = CalculateMipCoverage(dims, hzb_system.GetConfig());
            std::cout << " (Coverage: " << (coverage * 100.0f) << "%)" << std::endl;
        }
    }

    /**
     * @brief Run comprehensive HZB tests
     */
    static void RunComprehensiveHZBTests(
        HZBSystem& hzb_system,
        DepthHistoryManager& depth_manager,
        rhi::RHIDeviceBase* device);

private:
    static bool TestInitialization(HZBSystem& hzb_system, DepthHistoryManager& depth_manager) {
        std::cout << "Test 1: System Initialization" << std::endl;

        bool success = true;

        if (!hzb_system.IsReady()) {
            std::cerr << "  ❌ HZB System not ready" << std::endl;
            success = false;
        } else {
            std::cout << "  ✅ HZB System ready" << std::endl;
        }

        if (!depth_manager.IsInitialized()) {
            std::cerr << "  ❌ Depth History Manager not initialized" << std::endl;
            success = false;
        } else {
            std::cout << "  ✅ Depth History Manager initialized" << std::endl;
        }

        return success;
    }

    static bool TestHZBResources(HZBSystem& hzb_system) {
        std::cout << "Test 2: HZB Resource Validation" << std::endl;

        bool success = true;

        // Check mip levels
        u32 mip_levels = hzb_system.GetMipLevels();
        if (mip_levels == 0) {
            std::cerr << "  ❌ No mip levels available" << std::endl;
            success = false;
        } else {
            std::cout << "  ✅ Mip levels: " << mip_levels << std::endl;
        }

        // Check texture handles
        auto hzb_texture = hzb_system.GetHZBTexture();
        if (hzb_texture == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "  ❌ Invalid HZB texture handle" << std::endl;
            success = false;
        } else {
            std::cout << "  ✅ HZB texture handle valid" << std::endl;
        }

        auto hzb_sampler = hzb_system.GetHZBSampler();
        if (hzb_sampler == rhi::handles::INVALID_SAMPLER) {
            std::cerr << "  ❌ Invalid HZB sampler handle" << std::endl;
            success = false;
        } else {
            std::cout << "  ✅ HZB sampler handle valid" << std::endl;
        }

        return success;
    }

    static bool TestDepthHistory(DepthHistoryManager& depth_manager) {
        std::cout << "Test 3: Depth History Management" << std::endl;

        bool success = true;

        // Test buffer count
        u32 buffer_count = depth_manager.GetBufferCount();
        if (buffer_count < 2) {
            std::cerr << "  ❌ Insufficient buffer count: " << buffer_count << std::endl;
            success = false;
        } else {
            std::cout << "  ✅ Buffer count: " << buffer_count << std::endl;
        }

        // Test previous frame availability (should be false for frame 0)
        if (depth_manager.IsPreviousFrameDepthAvailable(0)) {
            std::cout << "  ⚠️  Previous frame depth available (unexpected for frame 0)" << std::endl;
        } else {
            std::cout << "  ✅ Previous frame depth correctly unavailable for frame 0" << std::endl;
        }

        return success;
    }

    static bool TestConfigurationQueries(HZBSystem& hzb_system, DepthHistoryManager& depth_manager) {
        std::cout << "Test 4: Configuration Queries" << std::endl;

        bool success = true;

        // Test HZB configuration
        const auto& hzb_config = hzb_system.GetConfig();
        if (hzb_config.max_width == 0 || hzb_config.max_height == 0) {
            std::cerr << "  ❌ Invalid HZB resolution" << std::endl;
            success = false;
        } else {
            std::cout << "  ✅ HZB resolution: " << hzb_config.max_width << "x" << hzb_config.max_height << std::endl;
        }

        // Test depth configuration
        const auto& depth_config = depth_manager.GetConfig();
        if (depth_config.width == 0 || depth_config.height == 0) {
            std::cerr << "  ❌ Invalid depth resolution" << std::endl;
            success = false;
        } else {
            std::cout << "  ✅ Depth resolution: " << depth_config.width << "x" << depth_config.height << std::endl;
        }

        return success;
    }

    static u64 EstimateHZBMemoryUsage(HZBSystem& hzb_system) {
        const auto& config = hzb_system.GetConfig();
        u64 total_size = 0;

        // Calculate memory for each mip level
        u32 width = config.max_width;
        u32 height = config.max_height;

        for (u32 mip = 0; mip < hzb_system.GetMipLevels(); ++mip) {
            total_size += width * height * sizeof(float); // R32_Float format
            width = std::max(config.min_mip_size, width / 2);
            height = std::max(config.min_mip_size, height / 2);
        }

        return total_size;
    }

    static u64 EstimateDepthMemoryUsage(DepthHistoryManager& depth_manager) {
        const auto& config = depth_manager.GetConfig();
        return static_cast<u64>(config.width) * config.height * sizeof(float) * config.buffer_count;
    }

    static float CalculateMipCoverage(math::v2 mip_dims, const HZBSystem::Config& config) {
        float total_pixels = static_cast<float>(config.max_width * config.max_height);
        float mip_pixels = mip_dims.x * mip_dims.y;
        return mip_pixels / total_pixels;
    }

    static std::string GetCurrentTime() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        return std::ctime(&time);
    }
};

} // namespace primal::graphics::nanite