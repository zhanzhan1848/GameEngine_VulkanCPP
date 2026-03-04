#pragma once

#include "CommonHeaders.h"
#include "JobTypes.h"
#include <optional>
#include <mutex>
#include <deque>

namespace primal::jobsystem {

template<typename T>
class WorkStealingQueue
{
public:
    explicit WorkStealingQueue(u32 capacity = 1024);
    ~WorkStealingQueue();
    
    DISABLE_COPY(WorkStealingQueue);
    DISABLE_MOVE(WorkStealingQueue);
    
    void Push(T item);
    std::optional<T> Pop();      // LIFO from owner thread
    std::optional<T> Steal();    // FIFO from thief threads
    bool Empty() const;
    u32 Size() const;
    u32 Capacity() const;
    void Clear();
    
private:
    mutable std::mutex _mutex;
    std::deque<T> _deque;
    u32 _capacity;
};

// Job queue stores pointers to Job objects
// Jobs are managed externally (e.g., in a job pool)
using JobQueue = WorkStealingQueue<Job*>;

} // namespace primal::jobsystem
