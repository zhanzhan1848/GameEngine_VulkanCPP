#include "JobSystem.h"
#include <queue>

namespace primal::jobsystem {

JobSystem* JobSystem::s_instance = nullptr;

JobSystem::JobSystem()
    : _scheduler(std::make_unique<JobScheduler>())
{
}

JobSystem::~JobSystem()
{
    if (_scheduler && _scheduler->IsRunning())
    {
        _scheduler->Shutdown();
    }
}

JobSystem* JobSystem::Get()
{
    return s_instance;
}

bool JobSystem::Initialize(const JobSchedulerConfig& config)
{
    if (s_instance)
    {
        return true; // Already initialized
    }
    
    s_instance = new JobSystem();
    s_instance->_scheduler = std::make_unique<JobScheduler>(config);
    
    if (!s_instance->_scheduler->Initialize())
    {
        delete s_instance;
        s_instance = nullptr;
        return false;
    }
    
    return true;
}

void JobSystem::Shutdown()
{
    if (s_instance)
    {
        s_instance->_scheduler->Shutdown();
        delete s_instance;
        s_instance = nullptr;
    }
}

bool JobSystem::IsRunning()
{
    return s_instance && s_instance->_scheduler && s_instance->_scheduler->IsRunning();
}

u32 JobSystem::GetWorkerCount()
{
    if (s_instance && s_instance->_scheduler)
    {
        return s_instance->_scheduler->GetWorkerCount();
    }
    return 0;
}

u32 JobSystem::GetActiveWorkerCount()
{
    if (s_instance && s_instance->_scheduler)
    {
        return s_instance->_scheduler->GetActiveWorkerCount();
    }
    return 0;
}

void JobSystem::WaitAll()
{
    if (s_instance && s_instance->_scheduler)
    {
        s_instance->_scheduler->WaitAll();
    }
}

void JobSystem::Wait(const JobHandle& handle)
{
    if (s_instance && s_instance->_scheduler)
    {
        s_instance->_scheduler->Wait(handle);
    }
}

void JobSystem::ProcessMainThreadJobs()
{
    if (!s_instance)
    {
        return;
    }
    
    std::lock_guard<std::mutex> lock(s_instance->_main_thread_mutex);
    
    while (!s_instance->_main_thread_jobs.empty())
    {
        Job job = std::move(s_instance->_main_thread_jobs.front());
        s_instance->_main_thread_jobs.pop();
        
        // Execute job (no exceptions)
        job.function(u32_invalid_id);
    }
}

bool JobSystem::IsMainThread()
{
    if (s_instance && s_instance->_scheduler)
    {
        return s_instance->_scheduler->IsMainThread();
    }
    return false;
}

bool JobSystem::IsWorkerThread()
{
    if (s_instance && s_instance->_scheduler)
    {
        return s_instance->_scheduler->IsWorkerThread();
    }
    return false;
}

u32 JobSystem::GetCurrentThreadIndex()
{
    if (s_instance && s_instance->_scheduler)
    {
        return s_instance->_scheduler->GetCurrentThreadIndex();
    }
    return u32_invalid_id;
}

} // namespace primal::jobsystem
