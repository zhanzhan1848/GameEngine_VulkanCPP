#include "TestFramework.h"
#include "Engine/JobSystem/WorkStealingQueue.h"
#include "Engine/JobSystem/JobScheduler.h"
#include "Engine/JobSystem/JobSystem.h"
#include <thread>
#include <vector>
#include <atomic>

using namespace Engine::Test;
using namespace primal::jobsystem;

namespace {

TestResult TestWorkStealingQueueBasicPushPop()
{
    WorkStealingQueue<u32> queue(64);
    
    TEST_ASSERT(queue.Empty(), "Queue should be empty initially");
    TEST_ASSERT_EQ(0u, queue.Size(), "Queue size should be 0");
    
    queue.Push(42);
    TEST_ASSERT(!queue.Empty(), "Queue should not be empty after push");
    TEST_ASSERT_EQ(1u, queue.Size(), "Queue size should be 1");
    
    auto result = queue.Pop();
    TEST_ASSERT(result.has_value(), "Pop should return a value");
    TEST_ASSERT_EQ(42u, result.value(), "Popped value should be 42");
    TEST_ASSERT(queue.Empty(), "Queue should be empty after pop");
    
    return TestResult::Passed;
}

TestResult TestWorkStealingQueueMultiplePushPop()
{
    WorkStealingQueue<u32> queue(128);
    
    for (u32 i = 0; i < 100; ++i)
    {
        queue.Push(i);
    }
    
    TEST_ASSERT_EQ(100u, queue.Size(), "Queue size should be 100");
    
    for (u32 i = 100; i > 0; --i)
    {
        auto result = queue.Pop();
        TEST_ASSERT(result.has_value(), "Pop should return a value");
        TEST_ASSERT_EQ(i - 1, result.value(), "Popped value should match");
    }
    
    TEST_ASSERT(queue.Empty(), "Queue should be empty");
    
    return TestResult::Passed;
}

TestResult TestWorkStealingQueueSteal()
{
    WorkStealingQueue<u32> queue(128);
    
    for (u32 i = 0; i < 50; ++i)
    {
        queue.Push(i);
    }
    
    for (u32 i = 0; i < 25; ++i)
    {
        auto result = queue.Steal();
        TEST_ASSERT(result.has_value(), "Steal should return a value");
        TEST_ASSERT_EQ(i, result.value(), "Stolen value should match FIFO order");
    }
    
    TEST_ASSERT_EQ(25u, queue.Size(), "Queue should have 25 items left");
    
    return TestResult::Passed;
}

TestResult TestWorkStealingQueueConcurrent()
{
    WorkStealingQueue<u32> queue(4096);
    std::atomic<u32> push_count{0};
    std::atomic<u32> pop_count{0};
    std::atomic<u32> steal_count{0};
    constexpr u32 num_items = 1000;
    
    std::thread pusher([&]() {
        for (u32 i = 0; i < num_items; ++i)
        {
            queue.Push(i);
            ++push_count;
        }
    });
    
    std::thread popper([&]() {
        while (pop_count.load() < num_items / 2)
        {
            auto result = queue.Pop();
            if (result.has_value())
            {
                ++pop_count;
            }
        }
    });
    
    std::thread stealer([&]() {
        while (steal_count.load() < num_items / 2)
        {
            auto result = queue.Steal();
            if (result.has_value())
            {
                ++steal_count;
            }
        }
    });
    
    pusher.join();
    popper.join();
    stealer.join();
    
    TEST_ASSERT_EQ(num_items, push_count.load(), "All items should be pushed");
    
    return TestResult::Passed;
}

TestResult TestJobSchedulerInitialize()
{
    JobSchedulerConfig config;
    config.core_thread_count = 2;
    config.max_thread_count = 4;
    
    JobScheduler scheduler(config);
    bool result = scheduler.Initialize();
    
    TEST_ASSERT(result, "Initialize should return true");
    TEST_ASSERT(scheduler.IsRunning(), "Scheduler should be running");
    TEST_ASSERT_EQ(2u, scheduler.GetWorkerCount(), "Should have 2 workers");
    
    scheduler.Shutdown();
    TEST_ASSERT(!scheduler.IsRunning(), "Scheduler should not be running after shutdown");
    
    return TestResult::Passed;
}

TestResult TestJobSchedulerSubmitAndWait()
{
    JobSchedulerConfig config;
    config.core_thread_count = 4;
    
    JobScheduler scheduler(config);
    TEST_ASSERT(scheduler.Initialize(), "Initialize should succeed");
    
    std::atomic<u32> counter{0};
    constexpr u32 num_jobs = 100;
    
    for (u32 i = 0; i < num_jobs; ++i)
    {
        scheduler.Submit([&counter](u32) {
            ++counter;
        }, JobPriority::Normal);
    }
    
    scheduler.WaitAll();
    
    TEST_ASSERT_EQ(num_jobs, counter.load(), "All jobs should have executed");
    
    scheduler.Shutdown();
    
    return TestResult::Passed;
}

TestResult TestJobSchedulerParallelFor()
{
    JobSchedulerConfig config;
    config.core_thread_count = 4;
    
    JobScheduler scheduler(config);
    TEST_ASSERT(scheduler.Initialize(), "Initialize should succeed");
    
    constexpr u32 num_iterations = 1000;
    std::atomic<u32> counter{0};
    
    auto handle = scheduler.SubmitParallel(num_iterations,
        [&counter]([[maybe_unused]] u32 index, [[maybe_unused]] u32 thread) {
            ++counter;
        });
    
    handle.Wait();
    
    TEST_ASSERT_EQ(num_iterations, counter.load(), "All iterations should execute");
    TEST_ASSERT(handle.IsComplete(), "Handle should show complete");
    TEST_ASSERT_FLOAT_EQ(1.0f, handle.GetProgress(), 0.001f, "Progress should be 100%");
    
    scheduler.Shutdown();
    
    return TestResult::Passed;
}

TestResult TestJobSystemSingleton()
{
    TEST_ASSERT(!JobSystem::IsRunning(), "JobSystem should not be running initially");
    
    bool init_result = JobSystem::Initialize(JobSchedulerConfig::Default());
    TEST_ASSERT(init_result, "Initialize should succeed");
    TEST_ASSERT(JobSystem::IsRunning(), "JobSystem should be running");
    TEST_ASSERT(JobSystem::GetWorkerCount() > 0, "Should have workers");
    TEST_ASSERT(JobSystem::Get() != nullptr, "Get should return instance");
    
    JobSystem::Shutdown();
    TEST_ASSERT(!JobSystem::IsRunning(), "JobSystem should not be running after shutdown");
    
    return TestResult::Passed;
}

TestResult TestJobSystemParallelFor()
{
    JobSystem::Initialize(JobSchedulerConfig::Default());
    
    constexpr u32 num_iterations = 500;
    std::vector<u32> results(num_iterations, 0);
    
    auto handle = JobSystem::ParallelFor(num_iterations, [&results](u32 i) {
        results[i] = i * 2;
    });
    
    handle.Wait();
    
    bool all_correct = true;
    for (u32 i = 0; i < num_iterations; ++i)
    {
        if (results[i] != i * 2)
        {
            all_correct = false;
            break;
        }
    }
    
    TEST_ASSERT(handle.IsComplete(), "Handle should be complete");
    TEST_ASSERT(all_correct, "All results should be correct");
    
    JobSystem::Shutdown();
    
    return TestResult::Passed;
}

TestResult TestJobSystemSchedule()
{
    JobSystem::Initialize(JobSchedulerConfig::Default());
    
    std::atomic<u32> counter{0};
    constexpr u32 num_jobs = 50;
    std::vector<JobHandle> handles;
    handles.reserve(num_jobs);
    
    for (u32 i = 0; i < num_jobs; ++i)
    {
        handles.push_back(JobSystem::Schedule([&counter]() {
            ++counter;
        }));
    }
    
    JobSystem::WaitAll();
    
    TEST_ASSERT_EQ(num_jobs, counter.load(), "All scheduled jobs should execute");
    
    for (const auto& handle : handles)
    {
        TEST_ASSERT(handle.IsComplete(), "All handles should be complete");
    }
    
    JobSystem::Shutdown();
    
    return TestResult::Passed;
}

TestResult TestJobSystemMainThreadSchedule()
{
    JobSystem::Initialize(JobSchedulerConfig::Default());
    
    std::atomic<bool> executed{false};
    
    auto handle = JobSystem::ScheduleOnMainThread([&executed]() {
        executed = true;
    });
    
    TEST_ASSERT(!executed.load(), "Job should not execute immediately");
    
    JobSystem::ProcessMainThreadJobs();
    
    TEST_ASSERT(executed.load(), "Job should have executed on main thread");
    TEST_ASSERT(handle.IsComplete(), "Handle should be complete");
    
    JobSystem::Shutdown();
    
    return TestResult::Passed;
}

TestResult TestJobHandleProgress()
{
    JobSchedulerConfig config;
    config.core_thread_count = 2;
    
    JobScheduler scheduler(config);
    scheduler.Initialize();
    
    constexpr u32 num_jobs = 100;
    auto handle = scheduler.SubmitParallel(num_jobs, [](u32, u32) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    
    handle.Wait();
    
    TEST_ASSERT_FLOAT_EQ(1.0f, handle.GetProgress(), 0.01f, "Progress should end at 1");
    TEST_ASSERT_EQ(num_jobs, handle.GetCompletedCount(), "Completed count should match");
    TEST_ASSERT_EQ(num_jobs, handle.GetTotalCount(), "Total count should match");
    
    scheduler.Shutdown();
    
    return TestResult::Passed;
}

TestResult TestJobPriority()
{
    JobSchedulerConfig config;
    config.core_thread_count = 2;
    
    JobScheduler scheduler(config);
    scheduler.Initialize();
    
    std::atomic<u32> counter{0};
    constexpr u32 num_jobs = 10;
    
    for (u32 i = 0; i < num_jobs; ++i)
    {
        scheduler.Submit([&counter]([[maybe_unused]] u32 thread) {
            ++counter;
        }, JobPriority::Normal);
    }
    
    scheduler.WaitAll();
    
    TEST_ASSERT_EQ(num_jobs, counter.load(), "All jobs should execute");
    
    scheduler.Shutdown();
    
    return TestResult::Passed;
}

}

int main()
{
    std::cout << "\n========================================\n";
    std::cout << "Job System Unit Tests\n";
    std::cout << "========================================\n";
    
    TestSuite suite("JobSystem Tests");
    
    suite.AddTestCase(TestCase("WorkStealingQueue Basic Push/Pop", TestWorkStealingQueueBasicPushPop));
    suite.AddTestCase(TestCase("WorkStealingQueue Multiple Push/Pop", TestWorkStealingQueueMultiplePushPop));
    suite.AddTestCase(TestCase("WorkStealingQueue Steal", TestWorkStealingQueueSteal));
    suite.AddTestCase(TestCase("WorkStealingQueue Concurrent", TestWorkStealingQueueConcurrent));
    suite.AddTestCase(TestCase("JobScheduler Initialize", TestJobSchedulerInitialize));
    suite.AddTestCase(TestCase("JobScheduler Submit and Wait", TestJobSchedulerSubmitAndWait));
    suite.AddTestCase(TestCase("JobScheduler ParallelFor", TestJobSchedulerParallelFor));
    suite.AddTestCase(TestCase("JobSystem Singleton", TestJobSystemSingleton));
    suite.AddTestCase(TestCase("JobSystem ParallelFor", TestJobSystemParallelFor));
    suite.AddTestCase(TestCase("JobSystem Schedule", TestJobSystemSchedule));
    suite.AddTestCase(TestCase("JobSystem MainThread Schedule", TestJobSystemMainThreadSchedule));
    suite.AddTestCase(TestCase("JobHandle Progress", TestJobHandleProgress));
    suite.AddTestCase(TestCase("Job Priority", TestJobPriority));
    
    auto stats = suite.RunAllTests();
    
    return (stats.failedTests > 0) ? 1 : 0;
}
