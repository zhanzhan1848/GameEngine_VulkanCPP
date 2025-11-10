#pragma once

#include "CommonHeaders.h"
#include "Utilities/FreeList.h"
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <future>

namespace primal::content
{
    // 资源类型枚举
    namespace resource_type
    {
        enum type : u32
        {
            unknown = 0,
            geometry,
            material, 
            texture,
            shader,
            audio,
            
            count
        };
    }

    // 资源状态枚举
    namespace resource_state
    {
        enum type : u32
        {
            unloaded = 0,    // 未加载
            loading,         // 加载中
            ready,           // 就绪
            error,           // 错误
            unloading        // 卸载中
        };
    }

    // 资源优先级
    namespace resource_priority
    {
        enum type : u32
        {
            low = 0,
            normal,
            high,
            critical
        };
    }

    // 轻量级资源句柄 (8字节)
    struct ResourceHandle
    {
        u32 index : 24;        // 资源索引 (支持16M资源)
        u32 type : 8;          // 资源类型
        u32 generation;        // 世代标识

        [[nodiscard]] constexpr bool is_valid() const { return index != 0; }
        [[nodiscard]] static constexpr ResourceHandle invalid() { return {0, 0, 0}; }
        
        bool operator==(const ResourceHandle& other) const {
            return index == other.index && generation == other.generation && type == other.type;
        }
        
        bool operator!=(const ResourceHandle& other) const {
            return !(*this == other);
        }
    };

    // 资源元数据
    struct ResourceMetadata
    {
        std::string path;                    // 资源路径
        u64 file_hash;                      // 文件哈希
        u64 file_size;                      // 文件大小
        u64 last_modified;                  // 最后修改时间
        resource_type::type type;           // 资源类型
        resource_priority::type priority;   // 优先级
        std::atomic<u32> ref_count{0};      // 引用计数
        std::atomic<u64> last_access_time{0}; // 最后访问时间
        std::atomic<u32> access_count{0};   // 访问次数
    };

    // 资源运行时信息
    struct ResourceRuntimeInfo
    {
        std::atomic<resource_state::type> state{resource_state::unloaded};
        void* gpu_resource{nullptr};        // GPU资源指针
        std::unique_ptr<u8[]> cpu_data;     // CPU数据
        u32 cpu_data_size{0};              // CPU数据大小
        id::id_type legacy_id{id::invalid_id}; // 兼容旧系统的ID
        std::future<bool> load_future;      // 异步加载Future
        std::string error_message;          // 错误信息
    };

    // 高性能资源管理器
    class ResourceManager
    {
    public:
        DISABLE_COPY_AND_MOVE(ResourceManager);
        
        // 单例访问
        static ResourceManager& instance();
        
        // 初始化和关闭
        bool initialize();
        void shutdown();
        
        // 同步资源加载
        ResourceHandle load_resource_sync(const char* path, resource_type::type type);
        ResourceHandle load_resource_sync(const void* data, u32 size, resource_type::type type, const char* name = nullptr);
        
        // 异步资源加载
        ResourceHandle load_resource_async(const char* path, resource_type::type type);
        std::future<ResourceHandle> load_resource_async_future(const char* path, resource_type::type type);
        
        // 资源访问
        bool is_ready(ResourceHandle handle) const;
        resource_state::type get_state(ResourceHandle handle) const;
        void* get_gpu_resource(ResourceHandle handle) const;
        const u8* get_cpu_data(ResourceHandle handle, u32& size) const;
        
        // 生命周期管理
        void retain_resource(ResourceHandle handle);
        void release_resource(ResourceHandle handle);
        u32 get_ref_count(ResourceHandle handle) const;
        
        // 优先级和访问管理
        void set_priority(ResourceHandle handle, resource_priority::type priority);
        resource_priority::type get_priority(ResourceHandle handle) const;
        void mark_accessed(ResourceHandle handle);
        
        // 缓存管理
        void preload_resources(const ResourceHandle* handles, u32 count);
        void unload_unused_resources(u32 max_unused_time_ms = 300000); // 5分钟
        void force_unload_resource(ResourceHandle handle);
        
        // 统计信息
        struct Statistics
        {
            u32 total_resources;
            u32 loaded_resources;
            u32 loading_resources;
            u64 total_memory_usage;
            u64 gpu_memory_usage;
            u32 cache_hits;
            u32 cache_misses;
        };
        Statistics get_statistics() const;
        
        // 持久化
        bool save_cache(const char* cache_file_path = nullptr);
        bool load_cache(const char* cache_file_path = nullptr);
        
        // 兼容性接口 (与现有ContentToEngine集成)
        id::id_type get_legacy_id(ResourceHandle handle) const;
        ResourceHandle get_handle_from_legacy_id(id::id_type legacy_id) const;
        
        // 错误处理
        const char* get_error_message(ResourceHandle handle) const;
        
        // 内存管理
        void set_memory_budget(u64 cpu_budget_bytes, u64 gpu_budget_bytes);
        void garbage_collect();
        
    private:
        ResourceManager() = default;
        ~ResourceManager() = default;
        
        // 内部实现
        ResourceHandle create_handle(resource_type::type type);
        bool validate_handle(ResourceHandle handle) const;
        ResourceMetadata* get_metadata(ResourceHandle handle) const;
        ResourceRuntimeInfo* get_runtime_info(ResourceHandle handle) const;
        
        // 异步加载工作线程
        void async_loader_thread();
        void process_load_queue();
        
        // 缓存管理
        void update_lru_cache();
        bool should_unload_resource(const ResourceMetadata& metadata, const ResourceRuntimeInfo& runtime_info) const;
        
        // 持久化辅助函数
        u64 calculate_file_hash(const char* path) const;
        std::string get_default_cache_path() const;
        
        // 数据成员
        mutable std::mutex _metadata_mutex;
        mutable std::mutex _runtime_mutex;
        mutable std::mutex _load_queue_mutex;
        
        utl::free_list<ResourceMetadata> _metadata_pool;
        utl::free_list<ResourceRuntimeInfo> _runtime_pool;
        
        std::unordered_map<std::string, ResourceHandle> _path_to_handle;
        std::unordered_map<u64, ResourceHandle> _hash_to_handle;
        std::unordered_map<id::id_type, ResourceHandle> _legacy_id_to_handle;
        
        std::vector<ResourceHandle> _load_queue;
        std::condition_variable _load_queue_cv;
        std::atomic<bool> _shutdown_requested{false};
        std::thread _async_loader_thread;
        
        // 统计信息
        mutable std::atomic<u32> _cache_hits{0};
        mutable std::atomic<u32> _cache_misses{0};
        
        // 内存预算
        std::atomic<u64> _cpu_memory_budget{1024 * 1024 * 1024}; // 1GB默认
        std::atomic<u64> _gpu_memory_budget{512 * 1024 * 1024};  // 512MB默认
        std::atomic<u64> _current_cpu_usage{0};
        std::atomic<u64> _current_gpu_usage{0};
        
        // 世代计数器
        std::atomic<u32> _generation_counter{1};
        
        // 缓存文件路径
        std::string _cache_file_path;
    };

    // 便利函数
    inline ResourceManager& get_resource_manager() { return ResourceManager::instance(); }
    
    // RAII资源句柄包装器
    class ScopedResourceHandle
    {
    public:
        explicit ScopedResourceHandle(ResourceHandle handle) : _handle(handle) {
            if (_handle.is_valid()) {
                get_resource_manager().retain_resource(_handle);
            }
        }
        
        ~ScopedResourceHandle() {
            if (_handle.is_valid()) {
                get_resource_manager().release_resource(_handle);
            }
        }
        
        ScopedResourceHandle(const ScopedResourceHandle& other) : _handle(other._handle) {
            if (_handle.is_valid()) {
                get_resource_manager().retain_resource(_handle);
            }
        }
        
        ScopedResourceHandle& operator=(const ScopedResourceHandle& other) {
            if (this != &other) {
                if (_handle.is_valid()) {
                    get_resource_manager().release_resource(_handle);
                }
                _handle = other._handle;
                if (_handle.is_valid()) {
                    get_resource_manager().retain_resource(_handle);
                }
            }
            return *this;
        }
        
        ScopedResourceHandle(ScopedResourceHandle&& other) noexcept : _handle(other._handle) {
            other._handle = ResourceHandle::invalid();
        }
        
        ScopedResourceHandle& operator=(ScopedResourceHandle&& other) noexcept {
            if (this != &other) {
                if (_handle.is_valid()) {
                    get_resource_manager().release_resource(_handle);
                }
                _handle = other._handle;
                other._handle = ResourceHandle::invalid();
            }
            return *this;
        }
        
        ResourceHandle get() const { return _handle; }
        ResourceHandle operator*() const { return _handle; }
        bool is_valid() const { return _handle.is_valid(); }
        
    private:
        ResourceHandle _handle;
    };
}

// 哈希函数支持
namespace std
{
    template<>
    struct hash<primal::content::ResourceHandle>
    {
        size_t operator()(const primal::content::ResourceHandle& handle) const noexcept {
            return hash<u64>{}(static_cast<u64>(handle.index) | 
                              (static_cast<u64>(handle.generation) << 24) |
                              (static_cast<u64>(handle.type) << 56));
        }
    };
}