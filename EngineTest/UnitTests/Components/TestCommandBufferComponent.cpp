#include "EngineTest/UnitTests/TestFramework.h"
#include "Components/CommandBuffer.h"
#include "EngineAPI/CommandBufferComponent.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "EngineAPI/GameEntity.h"

using namespace primal;
using namespace Engine::Test;

class TestCommandBufferComponent : public Engine::Test::TestSuite {
public:
    TestCommandBufferComponent() : TestSuite("CommandBufferComponentTests") {
        AddTestCase(TestCase("Create and Access", [this]() { return TestCreate(); }));
    }

    Engine::Test::TestResult TestCreate() {
        game_entity::entity entity{ game_entity::entity_id{ 200 } };
        command_buffer::info info;
        info.handle = (graphics::rhi::CommandBufferHandle)12345;
        info.sync_handle = (graphics::rhi::SyncHandle)67890;
        info.queue_type = graphics::rhi::CommandQueueType::Graphics;

        auto c = command_buffer::create(info, entity);
        TEST_ASSERT(c.is_valid(), "Component should be valid");

        // Verify data
        auto handle = command_buffer::get_handle(c);
        TEST_ASSERT(handle == info.handle, "Handle mismatch");
        
        auto sync = command_buffer::get_sync_handle(c);
        TEST_ASSERT(sync == info.sync_handle, "Sync handle mismatch");
        
        auto type = command_buffer::get_queue_type(c);
        TEST_ASSERT(type == info.queue_type, "Queue type mismatch");

        command_buffer::remove(c);
        TEST_ASSERT(!command_buffer::is_valid(c), "Component should be invalid after removal");

        return Engine::Test::TestResult::Passed;
    }
};

int main() {
    TestCommandBufferComponent suite;
    auto stats = suite.RunAllTests();
    return stats.failedTests > 0 ? 1 : 0;
}
