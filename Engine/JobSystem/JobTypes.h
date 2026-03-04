#pragma once

#include "CommonHeaders.h"

namespace primal::jobsystem {

// Job identifier type
using job_id = u32;
constexpr job_id invalid_job_id{ u32_invalid_id };

// Job priority levels - higher value = higher priority
enum class JobPriority : u8
{
    Low = 0,
    Normal = 1,
    High = 2,
    Count = 3
};

// Job execution state
enum class JobState : u8
{
    Pending = 0,      // Job is queued waiting to be executed
    Running = 1,      // Job is currently being executed
    Completed = 2,    // Job has finished execution
    Failed = 3,       // Job execution failed
    Cancelled = 4,    // Job was cancelled before execution
    Count = 5
};

// Thread affinity hints for job scheduling
// Used to optimize cache locality when jobs share data
struct JobAffinity
{
    u32 preferred_thread{ u32_invalid_id };  // Preferred thread index (u32_invalid_id = no preference)
    bool strict{ false };                     // If true, job MUST run on preferred thread
    
    constexpr JobAffinity() = default;
    constexpr explicit JobAffinity(u32 thread, bool is_strict = false)
        : preferred_thread(thread), strict(is_strict) {}
    
    constexpr bool HasAffinity() const { return preferred_thread != u32_invalid_id; }
    
    static constexpr JobAffinity None() { return JobAffinity{}; }
    static constexpr JobAffinity Thread(u32 thread, bool strict = false) { 
        return JobAffinity{ thread, strict }; 
    }
};

// Job function type - receives thread index as parameter
using JobFunction = std::function<void(u32 thread_index)>;

// Job data structure for internal use
struct Job
{
    JobFunction function;
    JobPriority priority{ JobPriority::Normal };
    JobAffinity affinity;
    std::atomic<u32>* completion_counter{ nullptr };  // Optional counter to decrement on completion
    u32 parent_job_id{ invalid_job_id };
    const char* name{ nullptr };  // Debug name
    
    Job() = default;
    Job(JobFunction&& func, JobPriority prio = JobPriority::Normal)
        : function(std::move(func)), priority(prio) {}
};

// Configuration for JobScheduler
struct JobSchedulerConfig
{
    u32 core_thread_count{ 0 };      // 0 = auto-detect (hardware_concurrency)
    u32 max_thread_count{ 0 };       // 0 = core_thread_count * 2
    bool enable_profiling{ false };  // Enable job timing/profiling
    bool enable_work_stealing{ true }; // Enable work stealing between threads
    u32 max_jobs_per_queue{ 4096 };  // Maximum jobs per thread queue
    
    static JobSchedulerConfig Default()
    {
        return JobSchedulerConfig{};
    }
    
    static JobSchedulerConfig HighPerformance()
    {
        JobSchedulerConfig config;
        config.core_thread_count = std::thread::hardware_concurrency();
        config.max_thread_count = config.core_thread_count;
        config.enable_work_stealing = true;
        return config;
    }
    
    static JobSchedulerConfig PowerSaver()
    {
        JobSchedulerConfig config;
        config.core_thread_count = std::max(2u, std::thread::hardware_concurrency() / 2);
        config.max_thread_count = config.core_thread_count;
        config.enable_work_stealing = true;
        return config;
    }
};

} // namespace primal::jobsystem
