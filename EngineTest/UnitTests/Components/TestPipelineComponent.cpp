#include "EngineTest/UnitTests/TestFramework.h"
#include "Engine/Components/Pipeline.h"
#include "Engine/EngineAPI/PipelineComponent.h"
#include "Engine/EngineAPI/GameEntity.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include <iostream>

using namespace primal;

class TestPipelineComponent : public Engine::Test::TestSuite {
public:
    TestPipelineComponent() : TestSuite("PipelineComponentTests") {
        AddTestCase(Engine::Test::TestCase("Create and Access", [this]() { return TestCreate(); }));
    }

    Engine::Test::TestResult TestCreate() {
        game_entity::entity entity{ game_entity::entity_id{ 100 } };
        
        pipeline::info info;
        info.handle = (graphics::rhi::PipelineHandle)12345;
        info.type = graphics::rhi::PipelineBindPoint::Graphics;
        
        auto c = pipeline::create(info, entity);
        
        if (!c.is_valid()) {
            std::cout << "Component validity check failed (object)" << std::endl;
            return Engine::Test::TestResult::Failed;
        }
        if (!pipeline::is_valid(c)) {
            std::cout << "Component validity check failed (system)" << std::endl;
            return Engine::Test::TestResult::Failed;
        }
        
        auto handle = pipeline::get_pipeline_handle(c);
        if (handle != (graphics::rhi::PipelineHandle)12345) {
            std::cout << "Handle mismatch: expected " << 12345 << ", got " << handle << std::endl;
            return Engine::Test::TestResult::Failed;
        }
        
        auto type = pipeline::get_pipeline_type(c);
        if (type != graphics::rhi::PipelineBindPoint::Graphics) {
            std::cout << "Type mismatch" << std::endl;
            return Engine::Test::TestResult::Failed;
        }
        
        // Update handle
        pipeline::set_pipeline_handle(c, (graphics::rhi::PipelineHandle)67890);
        handle = pipeline::get_pipeline_handle(c);
        if (handle != (graphics::rhi::PipelineHandle)67890) {
            std::cout << "Updated handle mismatch" << std::endl;
            return Engine::Test::TestResult::Failed;
        }
        
        pipeline::remove(c);
        if (pipeline::is_valid(c)) {
            std::cout << "Component should be invalid after removal" << std::endl;
            return Engine::Test::TestResult::Failed;
        }
        
        return Engine::Test::TestResult::Passed;
    }
};

int main() {
    TestPipelineComponent suite;
    auto stats = suite.RunAllTests();
    return stats.failedTests > 0 ? 1 : 0;
}
