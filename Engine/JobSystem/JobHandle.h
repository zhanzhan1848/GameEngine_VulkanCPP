#pragma once

#include "CommonHeaders.h"
#include "JobTypes.h"
#include <chrono>

namespace primal::jobsystem {

// Thread-safe state tracker for tracking job completion across multiple jobs
class JobStateTracker
{
public:
    explicit JobStateTracker(u32 total_jobs = 1)
        : _total_jobs(total_jobs)
        , _completed_jobs(0)
        , _failed_jobs(0)
    {}
    
    ~JobStateTracker() = default;
    
    DISABLE_COPY(JobStateTracker);
    DISABLE_MOVE(JobStateTracker);
    
    void SetTotalJobs(u32 total)
    {
        _total_jobs = total;
        _completed_jobs = 0;
        _failed_jobs = 0;
    }
    
    void NotifyJobCompleted()
    {
        ++_completed_jobs;
        if (IsComplete())
        {
            _completion_cv.notify_all();
        }
    }
    
    void NotifyJobFailed()
    {
        ++_failed_jobs;
        ++_completed_jobs;
        if (IsComplete())
        {
            _completion_cv.notify_all();
        }
    }
    
    bool IsComplete() const
    {
        return _completed_jobs.load() >= _total_jobs.load();
    }
    
    bool HasFailed() const
    {
        return _failed_jobs.load() > 0;
    }
    
    u32 GetCompletedCount() const
    {
        return _completed_jobs.load();
    }
    
    u32 GetTotalCount() const
    {
        return _total_jobs.load();
    }
    
    f32 GetProgress() const
    {
        const u32 total = _total_jobs.load();
        if (total == 0) return 1.0f;
        return static_cast<f32>(_completed_jobs.load()) / static_cast<f32>(total);
    }
    
    void Wait()
    {
        if (IsComplete()) return;
        
        // Use a timeout-based wait to prevent infinite hanging
        // and help execute jobs if we're on a worker thread
        while (!IsComplete())
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _completion_cv.wait_for(lock, std::chrono::milliseconds(1), [this] { return IsComplete(); });
        }
    }
    
    template<typename Rep, typename Period>
    bool WaitFor(const std::chrono::duration<Rep, Period>& timeout)
    {
        if (IsComplete()) return true;
        
        std::unique_lock<std::mutex> lock(_mutex);
        return _completion_cv.wait_for(lock, timeout, [this] { return IsComplete(); });
    }
    
private:
    std::atomic<u32> _total_jobs;
    std::atomic<u32> _completed_jobs;
    std::atomic<u32> _failed_jobs;
    std::mutex _mutex;
    std::condition_variable _completion_cv;
};

// Handle to a submitted job or job group for tracking and waiting
class JobHandle
{
public:
    JobHandle() = default;
    
    explicit JobHandle(std::shared_ptr<JobStateTracker> tracker)
        : _tracker(std::move(tracker))
    {}
    
    ~JobHandle() = default;
    
    JobHandle(const JobHandle&) = default;
    JobHandle& operator=(const JobHandle&) = default;
    JobHandle(JobHandle&&) noexcept = default;
    JobHandle& operator=(JobHandle&&) noexcept = default;
    
    bool IsValid() const
    {
        return _tracker != nullptr;
    }
    
    explicit operator bool() const
    {
        return IsValid();
    }
    
    bool IsComplete() const
    {
        return _tracker ? _tracker->IsComplete() : true;
    }
    
    bool HasFailed() const
    {
        return _tracker ? _tracker->HasFailed() : false;
    }
    
    f32 GetProgress() const
    {
        return _tracker ? _tracker->GetProgress() : 1.0f;
    }
    
    u32 GetCompletedCount() const
    {
        return _tracker ? _tracker->GetCompletedCount() : 0;
    }
    
    u32 GetTotalCount() const
    {
        return _tracker ? _tracker->GetTotalCount() : 0;
    }
    
    void Wait() const
    {
        if (_tracker)
        {
            _tracker->Wait();
        }
    }
    
    template<typename Rep, typename Period>
    bool WaitFor(const std::chrono::duration<Rep, Period>& timeout) const
    {
        return _tracker ? _tracker->WaitFor(timeout) : true;
    }
    
    void Reset()
    {
        _tracker.reset();
    }
    
private:
    std::shared_ptr<JobStateTracker> _tracker;
};

} // namespace primal::jobsystem
