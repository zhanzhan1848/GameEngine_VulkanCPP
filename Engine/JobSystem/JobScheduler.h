#pragma once

#include "CommonHeaders.h"
#include "JobTypes.h"
#include "JobHandle.h"
#include "WorkStealingQueue.h"
#include <deque>
#include <deque>
#include <vector>
#include <random>

namespace primal::jobsystem {

class JobScheduler
{
public:
    explicit JobScheduler(const JobSchedulerConfig& config = JobSchedulerConfig::Default());
    ~JobScheduler();
    
    DISABLE_COPY(JobScheduler);
    DISABLE_MOVE(JobScheduler);
    
    bool Initialize();
    void Shutdown();
    bool IsRunning() const { return _is_running.load(); }
    u32 GetWorkerCount() const { return _worker_count; }
    u32 GetActiveWorkerCount() const;
    
    JobHandle Submit(JobFunction&& function, 
                     JobPriority priority = JobPriority::Normal,
                     JobAffinity affinity = JobAffinity::None());
    
    JobHandle SubmitParallel(u32 count,
                            std::function<void(u32 index, u32 thread_index)>&& function,
                            JobPriority priority = JobPriority::Normal);
    
    void Wait(const JobHandle& handle);
    
    template<typename Rep, typename Period>
    bool WaitFor(const JobHandle& handle, const std::chrono::duration<Rep, Period>& timeout)
    {
        return handle.WaitFor(timeout);
    }
    
    void WaitAll();
    u32 GetCurrentThreadIndex() const;
    bool IsWorkerThread() const;
    bool IsMainThread() const;
    void ProcessMainThreadJobs();
    bool StealWork(u32 thief_index, Job*& out_job);
    bool GetLocalJob(u32 thread_index, Job*& out_job);
    
private:
    struct WorkerThread
    {
        std::thread thread;
        u32 index{ 0 };
        std::atomic<bool> is_running{ false };
        std::atomic<bool> is_executing{ false };
        std::unique_ptr<JobQueue> queue;
    };
    
    void WorkerLoop(u32 thread_index);
    bool TryExecuteJob(u32 thread_index);
    bool TryGetJob(u32 thread_index, Job*& out_job);
    void ExecuteJob(Job* job, u32 thread_index);
    
    Job* AllocateJob();
    void FreeJob(Job* job);
    void EnqueueJob(Job* job, u32 preferred_thread = u32_invalid_id);
    
    JobSchedulerConfig _config;
    std::atomic<bool> _is_running{ false };
    std::atomic<bool> _is_shutting_down{ false };
    u32 _worker_count{ 0 };
    u32 _core_thread_count{ 0 };
    
    std::vector<std::unique_ptr<WorkerThread>> _workers;
    std::vector<JobQueue*> _queues;
    
    std::unique_ptr<JobQueue> _main_thread_queue;
    std::thread::id _main_thread_id;
    
    // Job pool for memory management
    std::deque<Job> _job_pool;
    std::mutex _job_pool_mutex;
    
    static thread_local u32 t_thread_index;
    
    std::mt19937 _rng;
    std::mutex _rng_mutex;
    
    std::mutex _wait_mutex;
    std::condition_variable _wait_cv;
    std::atomic<u32> _pending_job_count{ 0 };
    
    std::mutex _shutdown_mutex;
    std::condition_variable _shutdown_cv;
    
    std::atomic<u32> _next_queue{ 0 };  // For round-robin job distribution
};

} // namespace primal::jobsystem
