#pragma once

#include "CommonHeaders.h"
#include "JobTypes.h"
#include "JobHandle.h"
#include "JobScheduler.h"
#include <queue>  // MSVC 不会间接提供 std::queue，需显式包含

namespace primal::jobsystem {

class JobSystem
{
public:
    static JobSystem* Get();
    
    static bool Initialize(const JobSchedulerConfig& config = JobSchedulerConfig::Default());
    static void Shutdown();
    static bool IsRunning();
    static u32 GetWorkerCount();
    static u32 GetActiveWorkerCount();
    
    template<typename Func>
    static JobHandle ParallelFor(u32 count, Func&& func, JobPriority priority = JobPriority::Normal)
    {
        assert(Get() != nullptr && "JobSystem not initialized");
        return Get()->_scheduler->SubmitParallel(count,
            [f = std::forward<Func>(func)](u32 i, u32) mutable { f(i); },
            priority);
    }
    
    template<typename Func>
    static JobHandle ParallelForWithThread(u32 count, Func&& func, JobPriority priority = JobPriority::Normal)
    {
        assert(Get() != nullptr && "JobSystem not initialized");
        return Get()->_scheduler->SubmitParallel(count, std::forward<Func>(func), priority);
    }
    
    template<typename Func>
    static JobHandle ParallelRange(u32 start, u32 end, Func&& func, JobPriority priority = JobPriority::Normal)
    {
        assert(Get() != nullptr && "JobSystem not initialized");
        u32 count = end > start ? end - start : 0;
        return Get()->_scheduler->SubmitParallel(count,
            [start, f = std::forward<Func>(func)](u32 i, u32 thread) mutable { f(start + i, thread); },
            priority);
    }
    
    template<typename Func>
    static JobHandle Schedule(Func&& func, 
                              JobPriority priority = JobPriority::Normal,
                              JobAffinity affinity = JobAffinity::None())
    {
        assert(Get() != nullptr && "JobSystem not initialized");
        return Get()->_scheduler->Submit(
            [f = std::forward<Func>(func)](u32) mutable { f(); },
            priority, affinity);
    }
    
    template<typename Func>
    static JobHandle ScheduleOnMainThread(Func&& func)
    {
        assert(Get() != nullptr && "JobSystem not initialized");
        auto tracker = std::make_shared<JobStateTracker>(1);
        
        Job job;
        job.function = [f = std::forward<Func>(func), tracker](u32) mutable
        {
            f();
            tracker->NotifyJobCompleted();
        };
        job.priority = JobPriority::High;
        
        // Must lock mutex when pushing from worker threads
        {
            std::lock_guard<std::mutex> lock(Get()->_main_thread_mutex);
            Get()->_main_thread_jobs.push(std::move(job));
        }
        
        return JobHandle(tracker);
    }
    
    template<typename Func>
    static JobHandle ScheduleBackground(Func&& func)
    {
        return Schedule(std::forward<Func>(func), JobPriority::Low);
    }
    
    static void WaitAll();
    static void Wait(const JobHandle& handle);
    static void ProcessMainThreadJobs();
    static bool IsMainThread();
    static bool IsWorkerThread();
    static u32 GetCurrentThreadIndex();
    
    JobScheduler* GetScheduler() { return _scheduler.get(); }
    
private:
    JobSystem();
    ~JobSystem();
    
    DISABLE_COPY(JobSystem);
    DISABLE_MOVE(JobSystem);
    
    static JobSystem* s_instance;
    
    std::unique_ptr<JobScheduler> _scheduler;
    std::queue<Job> _main_thread_jobs;
    std::mutex _main_thread_mutex;
};

#define g_JobSystem primal::jobsystem::JobSystem::Get()

} // namespace primal::jobsystem
