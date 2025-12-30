/**
 * @file PerformanceTest.cpp
 * @brief RHI性能测试程序
 * @details 测试RHI核心模块的性能特性
 * 
 * @author RHI开发团队
 * @date 2025-12-29
 */

#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <random>
#include <algorithm>

/**
 * @brief 性能测试计时器
 */
class PerformanceTimer {
private:
    std::chrono::high_resolution_clock::time_point start_time_;
    
public:
    void Start() {
        start_time_ = std::chrono::high_resolution_clock::now();
    }
    
    double ElapsedMilliseconds() const {
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time_);
        return duration.count() / 1000.0;
    }
    
    double ElapsedSeconds() const {
        return ElapsedMilliseconds() / 1000.0;
    }
};

/**
 * @brief 模拟RHI句柄管理器
 */
class MockHandleManager {
private:
    std::vector<uint64_t> handles_;
    std::atomic<uint64_t> next_id_{1};
    
public:
    uint64_t CreateHandle() {
        uint64_t id = next_id_.fetch_add(1);
        uint64_t handle = (1ULL << 32) | id; // 模拟类型ID + 唯一ID
        handles_.push_back(handle);
        return handle;
    }
    
    bool IsValidHandle(uint64_t handle) const {
        return std::find(handles_.begin(), handles_.end(), handle) != handles_.end();
    }
    
    void DestroyHandle(uint64_t handle) {
        auto it = std::find(handles_.begin(), handles_.end(), handle);
        if (it != handles_.end()) {
            handles_.erase(it);
        }
    }
    
    size_t GetHandleCount() const {
        return handles_.size();
    }
};

/**
 * @brief 模拟MPSC队列
 */
template<typename T>
class MockMPSCQueue {
private:
    struct Node {
        T data;
        Node* next;
        Node(const T& d) : data(d), next(nullptr) {}
    };
    
    std::atomic<Node*> head_;
    std::atomic<Node*> tail_;
    
public:
    MockMPSCQueue() : head_(nullptr), tail_(nullptr) {}
    
    void Enqueue(const T& item) {
        Node* new_node = new Node(item);
        Node* prev_tail = tail_.exchange(new_node, std::memory_order_acq_rel);
        if (prev_tail) {
            prev_tail->next = new_node;
        } else {
            head_.store(new_node, std::memory_order_release);
        }
    }
    
    bool Dequeue(T& result) {
        Node* current_head = head_.load(std::memory_order_acquire);
        if (!current_head) {
            return false;
        }
        
        Node* next_head = current_head->next;
        head_.store(next_head, std::memory_order_release);
        
        if (!next_head) {
            tail_.store(nullptr, std::memory_order_release);
        }
        
        result = current_head->data;
        delete current_head;
        return true;
    }
};

/**
 * @brief 测试句柄创建性能
 */
void TestHandleCreationPerformance() {
    std::cout << "=== 句柄创建性能测试 ===" << std::endl;
    
    MockHandleManager manager;
    const int NUM_HANDLES = 100000;
    
    PerformanceTimer timer;
    timer.Start();
    
    std::vector<uint64_t> handles;
    handles.reserve(NUM_HANDLES);
    
    for (int i = 0; i < NUM_HANDLES; ++i) {
        handles.push_back(manager.CreateHandle());
    }
    
    double elapsed = timer.ElapsedMilliseconds();
    std::cout << "创建 " << NUM_HANDLES << " 个句柄耗时: " << elapsed << " ms" << std::endl;
    std::cout << "平均每秒创建句柄数: " << (NUM_HANDLES / (elapsed / 1000.0)) << std::endl;
    
    // 测试句柄验证性能
    timer.Start();
    int valid_count = 0;
    for (uint64_t handle : handles) {
        if (manager.IsValidHandle(handle)) {
            valid_count++;
        }
    }
    elapsed = timer.ElapsedMilliseconds();
    std::cout << "验证 " << NUM_HANDLES << " 个句柄耗时: " << elapsed << " ms" << std::endl;
    std::cout << "有效句柄数: " << valid_count << std::endl;
}

/**
 * @brief 测试MPSC队列性能
 */
void TestMPSCQueuePerformance() {
    std::cout << "\n=== MPSC队列性能测试 ===" << std::endl;
    
    MockMPSCQueue<int> queue;
    const int NUM_OPERATIONS = 100000;
    
    // 单线程生产者测试
    PerformanceTimer timer;
    timer.Start();
    
    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        queue.Enqueue(i);
    }
    
    double enqueue_time = timer.ElapsedMilliseconds();
    std::cout << "单线程入队 " << NUM_OPERATIONS << " 个元素耗时: " << enqueue_time << " ms" << std::endl;
    
    // 消费者测试
    timer.Start();
    int dequeued_count = 0;
    int value;
    
    while (queue.Dequeue(value)) {
        dequeued_count++;
    }
    
    double dequeue_time = timer.ElapsedMilliseconds();
    std::cout << "出队 " << dequeued_count << " 个元素耗时: " << dequeue_time << " ms" << std::endl;
    std::cout << "总吞吐量: " << (NUM_OPERATIONS / ((enqueue_time + dequeue_time) / 1000.0)) << " ops/s" << std::endl;
}

/**
 * @brief 测试多线程MPSC队列性能
 */
void TestMultiThreadMPSCQueue() {
    std::cout << "\n=== 多线程MPSC队列性能测试 ===" << std::endl;
    
    MockMPSCQueue<int> queue;
    const int NUM_THREADS = 4;
    const int OPERATIONS_PER_THREAD = 25000;
    const int TOTAL_OPERATIONS = NUM_THREADS * OPERATIONS_PER_THREAD;
    
    std::atomic<bool> start_flag{false};
    std::vector<std::thread> producers;
    std::atomic<int> total_enqueued{0};
    
    PerformanceTimer timer;
    
    // 创建生产者线程
    for (int t = 0; t < NUM_THREADS; ++t) {
        producers.emplace_back([&queue, &start_flag, &total_enqueued, OPERATIONS_PER_THREAD, t]() {
            // 等待开始信号
            while (!start_flag.load()) {
                std::this_thread::yield();
            }
            
            for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
                queue.Enqueue(t * OPERATIONS_PER_THREAD + i);
                total_enqueued.fetch_add(1);
            }
        });
    }
    
    // 开始计时
    timer.Start();
    start_flag.store(true);
    
    // 等待所有生产者完成
    for (auto& producer : producers) {
        producer.join();
    }
    
    double enqueue_time = timer.ElapsedMilliseconds();
    std::cout << NUM_THREADS << " 个线程并发入队 " << TOTAL_OPERATIONS << " 个元素耗时: " << enqueue_time << " ms" << std::endl;
    std::cout << "多线程入队吞吐量: " << (TOTAL_OPERATIONS / (enqueue_time / 1000.0)) << " ops/s" << std::endl;
    
    // 消费所有元素
    timer.Start();
    int dequeued_count = 0;
    int value;
    
    while (queue.Dequeue(value)) {
        dequeued_count++;
    }
    
    double dequeue_time = timer.ElapsedMilliseconds();
    std::cout << "出队 " << dequeued_count << " 个元素耗时: " << dequeue_time << " ms" << std::endl;
    std::cout << "实际入队数: " << total_enqueued.load() << ", 实际出队数: " << dequeued_count << std::endl;
}

/**
 * @brief 测试内存分配性能
 */
void TestMemoryAllocationPerformance() {
    std::cout << "\n=== 内存分配性能测试 ===" << std::endl;
    
    const int NUM_ALLOCATIONS = 10000;
    const size_t ALLOCATION_SIZE = 1024;
    
    std::vector<void*> pointers;
    pointers.reserve(NUM_ALLOCATIONS);
    
    PerformanceTimer timer;
    
    // 测试malloc/free性能
    timer.Start();
    for (int i = 0; i < NUM_ALLOCATIONS; ++i) {
        void* ptr = std::malloc(ALLOCATION_SIZE);
        pointers.push_back(ptr);
    }
    
    double malloc_time = timer.ElapsedMilliseconds();
    
    timer.Start();
    for (void* ptr : pointers) {
        std::free(ptr);
    }
    
    double free_time = timer.ElapsedMilliseconds();
    
    pointers.clear();
    pointers.reserve(NUM_ALLOCATIONS);
    
    // 测试new/delete性能
    timer.Start();
    for (int i = 0; i < NUM_ALLOCATIONS; ++i) {
        void* ptr = ::operator new(ALLOCATION_SIZE);
        pointers.push_back(ptr);
    }
    
    double new_time = timer.ElapsedMilliseconds();
    
    timer.Start();
    for (void* ptr : pointers) {
        ::operator delete(ptr);
    }
    
    double delete_time = timer.ElapsedMilliseconds();
    
    std::cout << "malloc " << NUM_ALLOCATIONS << " 次耗时: " << malloc_time << " ms" << std::endl;
    std::cout << "free " << NUM_ALLOCATIONS << " 次耗时: " << free_time << " ms" << std::endl;
    std::cout << "new " << NUM_ALLOCATIONS << " 次耗时: " << new_time << " ms" << std::endl;
    std::cout << "delete " << NUM_ALLOCATIONS << " 次耗时: " << delete_time << " ms" << std::endl;
}

/**
 * @brief 主函数
 */
int main() {
    std::cout << "========================================\n";
    std::cout << "    RHI核心模块性能测试程序\n";
    std::cout << "========================================\n\n";
    
    TestHandleCreationPerformance();
    TestMPSCQueuePerformance();
    TestMultiThreadMPSCQueue();
    TestMemoryAllocationPerformance();
    
    std::cout << "\n========================================\n";
    std::cout << "✅ 性能测试完成！\n";
    std::cout << "========================================\n";
    
    return 0;
}