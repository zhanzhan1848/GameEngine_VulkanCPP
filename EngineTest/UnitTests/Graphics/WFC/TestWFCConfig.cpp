#include "../../TestFramework.h"
#include "Engine/Graphics/WFC/WFCConfig.h"
#include "Engine/Graphics/WFC/WFCCategory.h"
#include "Engine/Graphics/PCG/PCGReflection.h"

#include <string>

using namespace primal::graphics::wfc;
using namespace primal::graphics::pcg;
using namespace Engine::Test;

TestResult TestWFCConfig_Default_Values() {
    WFCConfig config;
    TEST_ASSERT_EQ(16, config.grid_size.x, "Default grid_size.x");
    TEST_ASSERT_EQ(16, config.grid_size.y, "Default grid_size.y");
    TEST_ASSERT_EQ(16, config.grid_size.z, "Default grid_size.z");
    TEST_ASSERT_EQ(64u, config.max_cells_per_frame, "Default max_cells_per_frame");
    TEST_ASSERT_EQ(4u, config.max_ms_per_frame, "Default max_ms_per_frame");
    TEST_ASSERT(config.mode == WFCConfig::Mode::Independent3D, "Default mode");
    return TestResult::Passed;
}

TestResult TestWFCConfig_GetParamDescriptors_Returns_Expected_Count() {
    WFCConfig config;
    PCGParamDescriptor descs[16];
    u32 count = WFCConfig::GetParamDescriptors(descs, 16);
    TEST_ASSERT(count >= 6, "Should have at least 6 params (grid_x/y/z, mode, max_cells, max_ms)");
    return TestResult::Passed;
}

TestResult TestWFCConfig_GetParamDescriptors_Reflects_Offsets() {
    WFCConfig config;
    config.grid_size.x = 99;
    config.max_cells_per_frame = 77;

    PCGParamDescriptor descs[16];
    u32 count = WFCConfig::GetParamDescriptors(descs, 16);

    bool found = false;
    for (u32 i = 0; i < count; ++i) {
        if (std::string(descs[i].name) == "grid_size.x") {
            const u32* reflected = reinterpret_cast<const u32*>(
                reinterpret_cast<const u8*>(&config) + descs[i].offset);
            TEST_ASSERT_EQ(99u, *reflected, "grid_size.x offset should point to value 99");
            found = true;
            break;
        }
    }
    TEST_ASSERT(found, "grid_size.x descriptor should exist");
    return TestResult::Passed;
}

TestResult TestWFCConfig_DefaultCategoryMask_AllCategories() {
    WFCConfig cfg;
    // Default = all bits set — every category is eligible.
    TEST_ASSERT_EQ(~0ULL, cfg.active_category_mask, "default = all categories");
    return TestResult::Passed;
}

TestResult TestWFCConfig_CategoryMask_Roundtrip() {
    WFCConfig cfg;
    cfg.active_category_mask = CategoryMaskFor(WFCCategory::Ruins);
    TEST_ASSERT_EQ(CategoryMaskFor(WFCCategory::Ruins),
                   cfg.active_category_mask, "Ruins-only mask survives roundtrip");
    return TestResult::Passed;
}

int main() {
    TestSuite suite("WFCConfig");
    TEST_CASE(suite, "Default_Values", TestWFCConfig_Default_Values);
    TEST_CASE(suite, "GetParamDescriptors_Returns_Expected_Count", TestWFCConfig_GetParamDescriptors_Returns_Expected_Count);
    TEST_CASE(suite, "GetParamDescriptors_Reflects_Offsets", TestWFCConfig_GetParamDescriptors_Reflects_Offsets);
    TEST_CASE(suite, "DefaultCategoryMask_AllCategories", TestWFCConfig_DefaultCategoryMask_AllCategories);
    TEST_CASE(suite, "CategoryMask_Roundtrip",            TestWFCConfig_CategoryMask_Roundtrip);
    suite.RunAllTests();
    return 0;
}
