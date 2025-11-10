#pragma once
#include "ResourceManager.h"
#include "ContentToEngine.h"

namespace primal::content
{
    // 资源适配器 - 新旧系统之间的桥梁
    class ResourceAdapter
    {
    public:
        // 单例访问
        static ResourceAdapter& instance();
        
        // 初始化和关闭
        bool initialize();
        void shutdown();
        
        // === 兼容现有ContentToEngine接口 ===
        
        // 创建资源 (兼容现有接口，内部使用新的ResourceManager)
        id::id_type create_resource_legacy(const void* data, asset_type::type type);
        void destroy_resource_legacy(id::id_type id, asset_type::type type);
        
        // === 新的高级接口 ===
        
        // 从文件加载资源
        ResourceHandle load_from_file(const char* path, resource_type::type type);
        ResourceHandle load_from_file_async(const char* path, resource_type::type type);
        
        // 从内存加载资源
        ResourceHandle load_from_memory(const void* data, u32 size, resource_type::type type, const char* name = nullptr);
        
        // 资源访问
        bool is_resource_ready(ResourceHandle handle) const;
        void* get_gpu_resource(ResourceHandle handle) const;
        const u8* get_cpu_data(ResourceHandle handle, u32& size) const;
        
        // 资源生命周期管理
        void retain_resource(ResourceHandle handle);
        void release_resource(ResourceHandle handle);
        
        // 资源状态和优先级
        resource_state::type get_resource_state(ResourceHandle handle) const;
        void set_resource_priority(ResourceHandle handle, resource_priority::type priority);
        
        // === 兼容性转换 ===
        
        // 新句柄 <-> 旧ID 转换
        ResourceHandle get_handle_from_legacy_id(id::id_type legacy_id) const;
        id::id_type get_legacy_id_from_handle(ResourceHandle handle) const;
        
        // 资源类型转换
        static resource_type::type convert_asset_type(asset_type::type asset_type);
        static asset_type::type convert_resource_type(resource_type::type resource_type);
        
        // === 批量操作接口 ===
        
        // 批量加载资源
        struct BatchLoadRequest
        {
            const char* path;
            resource_type::type type;
            resource_priority::type priority = resource_priority::normal;
        };
        
        struct BatchLoadResult
        {
            ResourceHandle handle;
            bool success;
            const char* error_message = nullptr;
        };
        
        void load_resources_batch(const BatchLoadRequest* requests, u32 count, BatchLoadResult* results);
        void load_resources_batch_async(const BatchLoadRequest* requests, u32 count, BatchLoadResult* results);
        
        // === 缓存和预加载 ===
        
        // 预加载常用资源
        void preload_common_resources();
        
        // 场景切换时的资源管理
        void prepare_for_scene_change();
        void cleanup_after_scene_change();
        
        // 内存管理
        void garbage_collect();
        void force_cleanup_unused_resources();
        
        // === 统计和调试 ===
        
        struct AdapterStatistics
        {
            ResourceManager::Statistics resource_manager_stats;
            u32 legacy_resources_count;
            u32 adapter_cache_hits;
            u32 adapter_cache_misses;
            u32 conversion_operations;
        };
        
        AdapterStatistics get_statistics() const;
        void reset_statistics();
        
        // 调试信息
        void dump_resource_info(ResourceHandle handle) const;
        void dump_all_resources() const;
        
        // === 错误处理 ===
        
        const char* get_last_error() const { return _last_error.c_str(); }
        void clear_last_error() { _last_error.clear(); }
        
    private:
        ResourceAdapter() = default;
        ~ResourceAdapter() = default;
        
        // 禁止拷贝和赋值
        ResourceAdapter(const ResourceAdapter&) = delete;
        ResourceAdapter& operator=(const ResourceAdapter&) = delete;
        
        // 内部辅助方法
        void set_error(const std::string& error);
        bool validate_path(const char* path) const;
        bool validate_data(const void* data, u32 size) const;
        
        // 缓存管理
        void update_legacy_cache(id::id_type legacy_id, ResourceHandle handle);
        void remove_from_legacy_cache(id::id_type legacy_id);
        
        // 统计计数器
        mutable std::atomic<u32> _cache_hits{0};
        mutable std::atomic<u32> _cache_misses{0};
        mutable std::atomic<u32> _conversion_operations{0};
        
        // 错误信息
        std::string _last_error;
        
        // 初始化状态
        std::atomic<bool> _initialized{false};
        
        // 线程安全
        mutable std::mutex _error_mutex;
    };
    
    // === 便利宏和函数 ===
    
    // 简化的资源加载宏
    #define LOAD_RESOURCE(path, type) ResourceAdapter::instance().load_from_file(path, resource_type::type)
    #define LOAD_RESOURCE_ASYNC(path, type) ResourceAdapter::instance().load_from_file_async(path, resource_type::type)
    
    // RAII资源管理器
    class ScopedResource
    {
    public:
        explicit ScopedResource(ResourceHandle handle) : _handle(handle) 
        {
            if (_handle.is_valid()) {
                ResourceAdapter::instance().retain_resource(_handle);
            }
        }
        
        ~ScopedResource() 
        {
            if (_handle.is_valid()) {
                ResourceAdapter::instance().release_resource(_handle);
            }
        }
        
        // 移动构造和赋值
        ScopedResource(ScopedResource&& other) noexcept : _handle(other._handle) 
        {
            other._handle = ResourceHandle::invalid();
        }
        
        ScopedResource& operator=(ScopedResource&& other) noexcept 
        {
            if (this != &other) {
                if (_handle.is_valid()) {
                    ResourceAdapter::instance().release_resource(_handle);
                }
                _handle = other._handle;
                other._handle = ResourceHandle::invalid();
            }
            return *this;
        }
        
        // 禁止拷贝
        ScopedResource(const ScopedResource&) = delete;
        ScopedResource& operator=(const ScopedResource&) = delete;
        
        // 访问器
        ResourceHandle get() const { return _handle; }
        bool is_valid() const { return _handle.is_valid(); }
        bool is_ready() const { return ResourceAdapter::instance().is_resource_ready(_handle); }
        
        void* get_gpu_resource() const 
        {
            return ResourceAdapter::instance().get_gpu_resource(_handle);
        }
        
        const u8* get_cpu_data(u32& size) const 
        {
            return ResourceAdapter::instance().get_cpu_data(_handle, size);
        }
        
    private:
        ResourceHandle _handle;
    };
}