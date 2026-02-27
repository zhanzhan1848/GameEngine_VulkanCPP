#include "WorkStealingQueue.h"

namespace primal::jobsystem {

// ============================================================================
// Simple Mutex-Protected Work-Stealing Queue
// Correctness over performance - suitable for game engine job systems
// ============================================================================

template<typename T>
WorkStealingQueue<T>::WorkStealingQueue(u32 capacity)
    : _capacity(capacity)
{
}

template<typename T>
WorkStealingQueue<T>::~WorkStealingQueue()
{
}

template<typename T>
void WorkStealingQueue<T>::Push(T item)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_deque.size() < _capacity)
    {
        _deque.push_back(item);
    }
}

template<typename T>
std::optional<T> WorkStealingQueue<T>::Pop()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_deque.empty())
    {
        return std::nullopt;
    }
    T item = _deque.back();
    _deque.pop_back();
    return item;
}

template<typename T>
std::optional<T> WorkStealingQueue<T>::Steal()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_deque.empty())
    {
        return std::nullopt;
    }
    T item = _deque.front();
    _deque.pop_front();
    return item;
}

template<typename T>
bool WorkStealingQueue<T>::Empty() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _deque.empty();
}

template<typename T>
u32 WorkStealingQueue<T>::Size() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return static_cast<u32>(_deque.size());
}

template<typename T>
u32 WorkStealingQueue<T>::Capacity() const
{
    return _capacity;
}

template<typename T>
void WorkStealingQueue<T>::Clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _deque.clear();
}

// Explicit template instantiation for Job* pointers
template class WorkStealingQueue<Job*>;

// Explicit template instantiation for unsigned int (used in tests)
template class WorkStealingQueue<unsigned int>;

} // namespace primal::jobsystem
