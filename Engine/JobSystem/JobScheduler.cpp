#include "JobScheduler.h"
#include <algorithm>

namespace primal::jobsystem {

thread_local u32 JobScheduler::t_thread_index = u32_invalid_id;

JobScheduler::JobScheduler(const JobSchedulerConfig& config)
    : _config(config)
    , _main_thread_queue(std::make_unique<JobQueue>(config.max_jobs_per_queue))
    , _main_thread_id(std::this_thread::get_id())
{
    std::random_device rd;
    _rng.seed(rd());
}

JobScheduler::~JobScheduler()
{
    if (_is_running.load())
    {
        Shutdown();
    }
}

bool JobScheduler::Initialize()
{
    if (_is_running.load())
    {
        return false;
    }
    
    // Determine thread counts
    _core_thread_count = _config.core_thread_count;
    if (_core_thread_count == 0)
    {
        _core_thread_count = std::thread::hardware_concurrency();
        if (_core_thread_count == 0)
        {
            _core_thread_count = 4; // Fallback
        }
    }
    
    u32 max_threads = _config.max_thread_count;
    if (max_threads == 0)
    {
        max_threads = _core_thread_count * 2;
    }
    max_threads = std::max(max_threads, _core_thread_count);
    
    _worker_count = _core_thread_count;
    
    // Create worker threads and queues
    _workers.resize(_worker_count);
    _queues.resize(_worker_count);
    
    for (u32 i = 0; i < _worker_count; ++i)
    {
        auto worker = std::make_unique<WorkerThread>();
        worker->index = i;
        worker->queue = std::make_unique<JobQueue>(_config.max_jobs_per_queue);
        _queues[i] = worker->queue.get();
        _workers[i] = std::move(worker);
    }
    
    // Start worker threads
    _is_running.store(true);
    _is_shutting_down.store(false);

#ifndef __EMSCRIPTEN__
    // WASM build runs without pthreads (-s USE_PTHREADS=0); std::thread
    // constructor throws system_error(ENOSYS) and -fno-exceptions turns it
    // into abort(). JobSystem is initialized but no workers spawn —
    // submitted jobs queue and run inline via ProcessMainThreadJobs.
    // TestDawnForwardRenderer doesn't submit any jobs, so this is safe.
    for (u32 i = 0; i < _worker_count; ++i)
    {
        _workers[i]->is_running.store(true);
        _workers[i]->thread = std::thread(&JobScheduler::WorkerLoop, this, i);
    }
#endif

    return true;
}

void JobScheduler::Shutdown()
{
    if (!_is_running.load())
    {
        return;
    }
    
    _is_shutting_down.store(true);
    
    // Wake up all workers
    for (auto& worker : _workers)
    {
        worker->is_running.store(false);
    }
    
    // Wait for all workers to finish
    for (auto& worker : _workers)
    {
        if (worker->thread.joinable())
        {
            worker->thread.join();
        }
    }
    
    _workers.clear();
    _queues.clear();
    _worker_count = 0;
    
    _is_running.store(false);
    _is_shutting_down.store(false);
}

u32 JobScheduler::GetActiveWorkerCount() const
{
    u32 count = 0;
    for (const auto& worker : _workers)
    {
        if (worker && worker->is_executing.load())
        {
            ++count;
        }
    }
    return count;
}

Job* JobScheduler::AllocateJob()
{
    std::lock_guard<std::mutex> lock(_job_pool_mutex);
    _job_pool.emplace_back();
    return &_job_pool.back();
}

void JobScheduler::FreeJob(Job* job)
{
    // Jobs are not individually freed - deque grows as needed
    // and is cleared when scheduler shuts down
    (void)job;
}

JobHandle JobScheduler::Submit(JobFunction&& function,
                               JobPriority priority,
                               JobAffinity affinity)
{
    auto tracker = std::make_shared<JobStateTracker>(1);
    
    Job* job = AllocateJob();
    job->function = [func = std::move(function), tracker](u32 thread_index) mutable
    {
        func(thread_index);
        tracker->NotifyJobCompleted();
    };
    job->priority = priority;
    job->affinity = affinity;
    
    ++_pending_job_count;
    
    u32 preferred_thread = affinity.HasAffinity() ? affinity.preferred_thread : u32_invalid_id;
    EnqueueJob(job, preferred_thread);
    
    return JobHandle(tracker);
}

JobHandle JobScheduler::SubmitParallel(u32 count,
                                       std::function<void(u32 index, u32 thread_index)>&& function,
                                       JobPriority priority)
{
    if (count == 0)
    {
        return JobHandle(std::make_shared<JobStateTracker>(0));
    }
    
    auto tracker = std::make_shared<JobStateTracker>(count);
    
    for (u32 i = 0; i < count; ++i)
    {
        Job* job = AllocateJob();
        job->function = [i, func = function, tracker](u32 thread_index) mutable
        {
            func(i, thread_index);
            tracker->NotifyJobCompleted();
        };
        job->priority = priority;
        
        ++_pending_job_count;
        EnqueueJob(job);
    }
    
    return JobHandle(tracker);
}

void JobScheduler::Wait(const JobHandle& handle)
{
    if (!handle.IsValid())
    {
        return;
    }
    
    // If we're a worker thread, help with execution while waiting
    if (IsWorkerThread())
    {
        while (!handle.IsComplete())
        {
            TryExecuteJob(t_thread_index);
        }
    }
    else
    {
        handle.Wait();
    }
}

void JobScheduler::WaitAll()
{
    while (_pending_job_count.load() > 0)
    {
        if (IsWorkerThread())
        {
            TryExecuteJob(t_thread_index);
        }
        else
        {
            std::unique_lock<std::mutex> lock(_wait_mutex);
            _wait_cv.wait_for(lock, std::chrono::milliseconds(1), [this] {
                return _pending_job_count.load() == 0;
            });
        }
    }
}

u32 JobScheduler::GetCurrentThreadIndex() const
{
    return t_thread_index;
}

bool JobScheduler::IsWorkerThread() const
{
    return t_thread_index != u32_invalid_id;
}

bool JobScheduler::IsMainThread() const
{
    return std::this_thread::get_id() == _main_thread_id;
}

void JobScheduler::ProcessMainThreadJobs()
{
    assert(IsMainThread() && "ProcessMainThreadJobs must be called from main thread");
    
    Job* job;
    while (_main_thread_queue->Pop().has_value())
    {
        auto opt_job = _main_thread_queue->Pop();
        if (opt_job.has_value())
        {
            job = opt_job.value();
            ExecuteJob(job, u32_invalid_id);
        }
    }
}

bool JobScheduler::StealWork(u32 thief_index, Job*& out_job)
{
    if (!_config.enable_work_stealing || _worker_count <= 1)
    {
        return false;
    }
    
    // Try stealing from random victim
    u32 victim_index;
    {
        std::lock_guard<std::mutex> lock(_rng_mutex);
        std::uniform_int_distribution<u32> dist(0, _worker_count - 1);
        victim_index = dist(_rng);
    }
    
    if (victim_index == thief_index)
    {
        victim_index = (victim_index + 1) % _worker_count;
    }
    
    auto stolen = _queues[victim_index]->Steal();
    if (stolen.has_value())
    {
        out_job = stolen.value();
        return true;
    }
    
    return false;
}

bool JobScheduler::GetLocalJob(u32 thread_index, Job*& out_job)
{
    if (thread_index >= _worker_count)
    {
        return false;
    }
    
    auto job = _queues[thread_index]->Pop();
    if (job.has_value())
    {
        out_job = job.value();
        return out_job != nullptr;
    }
    
    return false;
}

void JobScheduler::WorkerLoop(u32 thread_index)
{
    t_thread_index = thread_index;
    
    while (_workers[thread_index]->is_running.load())
    {
        if (!TryExecuteJob(thread_index))
        {
            // No work available, yield
            std::this_thread::yield();
        }
    }
}

bool JobScheduler::TryExecuteJob(u32 thread_index)
{
    Job* job;
    
    // Try to get a local job first
    if (thread_index < _worker_count && GetLocalJob(thread_index, job))
    {
        ExecuteJob(job, thread_index);
        return true;
    }
    
    // Try to steal from other queues
    if (StealWork(thread_index, job))
    {
        ExecuteJob(job, thread_index);
        return true;
    }
    
    return false;
}

bool JobScheduler::TryGetJob(u32 thread_index, Job*& out_job)
{
    // Try local queue first
    if (GetLocalJob(thread_index, out_job))
    {
        return true;
    }
    
    // Try stealing
    return StealWork(thread_index, out_job);
}

void JobScheduler::ExecuteJob(Job* job, u32 thread_index)
{
    if (thread_index < _worker_count)
    {
        _workers[thread_index]->is_executing.store(true);
    }
    
    // Execute job (no exceptions - if job crashes, it crashes)
    job->function(thread_index);
    
    if (thread_index < _worker_count)
    {
        _workers[thread_index]->is_executing.store(false);
    }
    
    // Decrement pending count and notify waiters
    u32 prev_count = _pending_job_count.fetch_sub(1);
    if (prev_count == 1)
    {
        _wait_cv.notify_all();
    }
}

void JobScheduler::EnqueueJob(Job* job, u32 preferred_thread)
{
    if (job == nullptr)
    {
        return;
    }
    
    // Handle strict affinity
    if (job->affinity.strict && job->affinity.HasAffinity())
    {
        u32 target = job->affinity.preferred_thread;
        if (target < _worker_count)
        {
            _queues[target]->Push(job);
            return;
        }
    }
    
    // Handle non-strict affinity with fallback
    if (job->affinity.HasAffinity() && job->affinity.preferred_thread < _worker_count)
    {
        _queues[job->affinity.preferred_thread]->Push(job);
        return;
    }
    
    // Round-robin distribution
    u32 target = _next_queue.fetch_add(1) % _worker_count;
    _queues[target]->Push(job);
}

} // namespace primal::jobsystem
