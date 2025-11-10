#pragma once
#include "ResourceAdapter.h"

// 简化的资源管理API - 对外提供最简单易用的接口
namespace primal::resources
{
    // === 初始化和关闭 ===
    
    // 初始化资源系统
    inline bool initialize() {
        return content::ResourceAdapter::instance().initialize();
    }
    
    // 关闭资源系统
    inline void shutdown() {
        content::ResourceAdapter::instance().shutdown();
    }
    
    // === 资源句柄类型别名 ===
    
    using Handle = content::ResourceHandle;
    using State = content::resource_state::type;
    using Priority = content::resource_priority::type;
    
    // === 简化的资源加载接口 ===
    
    // 同步加载资源
    inline Handle load(const char* path) {
        // 根据文件扩展名自动判断资源类型
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        content::resource_type::type type = content::resource_type::unknown;
        
        if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".mesh") {
            type = content::resource_type::geometry;
        } else if (ext == ".dds" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
            type = content::resource_type::texture;
        } else if (ext == ".mat" || ext == ".material") {
            type = content::resource_type::material;
        } else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg") {
            type = content::resource_type::audio;
        } else if (ext == ".anim" || ext == ".animation") {
            type = content::resource_type::animation;
        } else if (ext == ".skel" || ext == ".skeleton") {
            type = content::resource_type::skeleton;
        }
        
        return content::ResourceAdapter::instance().load_from_file(path, type);
    }
    
    // 异步加载资源
    inline Handle load_async(const char* path) {
        // 同样的类型推断逻辑
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        content::resource_type::type type = content::resource_type::unknown;
        
        if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".mesh") {
            type = content::resource_type::geometry;
        } else if (ext == ".dds" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
            type = content::resource_type::texture;
        } else if (ext == ".mat" || ext == ".material") {
            type = content::resource_type::material;
        } else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg") {
            type = content::resource_type::audio;
        } else if (ext == ".anim" || ext == ".animation") {
            type = content::resource_type::animation;
        } else if (ext == ".skel" || ext == ".skeleton") {
            type = content::resource_type::skeleton;
        }
        
        return content::ResourceAdapter::instance().load_from_file_async(path, type);
    }
    
    // 指定类型加载
    inline Handle load_mesh(const char* path) {
        return content::ResourceAdapter::instance().load_from_file(path, content::resource_type::geometry);
    }
    
    inline Handle load_texture(const char* path) {
        return content::ResourceAdapter::instance().load_from_file(path, content::resource_type::texture);
    }
    
    inline Handle load_material(const char* path) {
        return content::ResourceAdapter::instance().load_from_file(path, content::resource_type::material);
    }
    
    inline Handle load_audio(const char* path) {
        return content::ResourceAdapter::instance().load_from_file(path, content::resource_type::audio);
    }
    
    // 异步版本
    inline Handle load_mesh_async(const char* path) {
        return content::ResourceAdapter::instance().load_from_file_async(path, content::resource_type::geometry);
    }
    
    inline Handle load_texture_async(const char* path) {
        return content::ResourceAdapter::instance().load_from_file_async(path, content::resource_type::texture);
    }
    
    inline Handle load_material_async(const char* path) {
        return content::ResourceAdapter::instance().load_from_file_async(path, content::resource_type::material);
    }
    
    inline Handle load_audio_async(const char* path) {
        return content::ResourceAdapter::instance().load_from_file_async(path, content::resource_type::audio);
    }
    
    // 从内存加载
    inline Handle load_from_memory(const void* data, u32 size, const char* name = nullptr) {
        return content::ResourceAdapter::instance().load_from_memory(data, size, content::resource_type::unknown, name);
    }
    
    // === 资源状态查询 ===
    
    // 检查资源是否准备就绪
    inline bool is_ready(Handle handle) {
        return content::ResourceAdapter::instance().is_resource_ready(handle);
    }
    
    // 获取资源状态
    inline State get_state(Handle handle) {
        return content::ResourceAdapter::instance().get_resource_state(handle);
    }
    
    // 检查句柄是否有效
    inline bool is_valid(Handle handle) {
        return handle.is_valid();
    }
    
    // === 资源访问 ===
    
    // 获取GPU资源指针
    inline void* get_gpu_resource(Handle handle) {
        return content::ResourceAdapter::instance().get_gpu_resource(handle);
    }
    
    // 获取CPU数据
    inline const u8* get_cpu_data(Handle handle, u32& size) {
        return content::ResourceAdapter::instance().get_cpu_data(handle, size);
    }
    
    // === 资源生命周期管理 ===
    
    // 增加引用计数
    inline void retain(Handle handle) {
        content::ResourceAdapter::instance().retain_resource(handle);
    }
    
    // 减少引用计数
    inline void release(Handle handle) {
        content::ResourceAdapter::instance().release_resource(handle);
    }
    
    // 设置资源优先级
    inline void set_priority(Handle handle, Priority priority) {
        content::ResourceAdapter::instance().set_resource_priority(handle, priority);
    }
    
    // === 批量操作 ===
    
    // 批量加载资源
    struct LoadRequest {
        const char* path;
        Priority priority = content::resource_priority::normal;
    };
    
    struct LoadResult {
        Handle handle;
        bool success;
        const char* error = nullptr;
    };
    
    inline void load_batch(const LoadRequest* requests, u32 count, LoadResult* results) {
        std::vector<content::ResourceAdapter::BatchLoadRequest> adapter_requests(count);
        std::vector<content::ResourceAdapter::BatchLoadResult> adapter_results(count);
        
        // 转换请求格式
        for (u32 i = 0; i < count; ++i) {
            adapter_requests[i].path = requests[i].path;
            adapter_requests[i].priority = requests[i].priority;
            
            // 自动推断类型
            std::string ext = std::filesystem::path(requests[i].path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            
            if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".mesh") {
                adapter_requests[i].type = content::resource_type::geometry;
            } else if (ext == ".dds" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
                adapter_requests[i].type = content::resource_type::texture;
            } else if (ext == ".mat" || ext == ".material") {
                adapter_requests[i].type = content::resource_type::material;
            } else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg") {
                adapter_requests[i].type = content::resource_type::audio;
            } else {
                adapter_requests[i].type = content::resource_type::unknown;
            }
        }
        
        // 执行批量加载
        content::ResourceAdapter::instance().load_resources_batch(
            adapter_requests.data(), count, adapter_results.data());
        
        // 转换结果格式
        for (u32 i = 0; i < count; ++i) {
            results[i].handle = adapter_results[i].handle;
            results[i].success = adapter_results[i].success;
            results[i].error = adapter_results[i].error_message;
        }
    }
    
    // 异步批量加载
    inline void load_batch_async(const LoadRequest* requests, u32 count, LoadResult* results) {
        std::vector<content::ResourceAdapter::BatchLoadRequest> adapter_requests(count);
        std::vector<content::ResourceAdapter::BatchLoadResult> adapter_results(count);
        
        // 转换请求格式 (同上)
        for (u32 i = 0; i < count; ++i) {
            adapter_requests[i].path = requests[i].path;
            adapter_requests[i].priority = requests[i].priority;
            
            std::string ext = std::filesystem::path(requests[i].path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            
            if (ext == ".obj" || ext == ".fbx" || ext == ".gltf" || ext == ".mesh") {
                adapter_requests[i].type = content::resource_type::geometry;
            } else if (ext == ".dds" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
                adapter_requests[i].type = content::resource_type::texture;
            } else if (ext == ".mat" || ext == ".material") {
                adapter_requests[i].type = content::resource_type::material;
            } else if (ext == ".wav" || ext == ".mp3" || ext == ".ogg") {
                adapter_requests[i].type = content::resource_type::audio;
            } else {
                adapter_requests[i].type = content::resource_type::unknown;
            }
        }
        
        // 执行异步批量加载
        content::ResourceAdapter::instance().load_resources_batch_async(
            adapter_requests.data(), count, adapter_results.data());
        
        // 转换结果格式
        for (u32 i = 0; i < count; ++i) {
            results[i].handle = adapter_results[i].handle;
            results[i].success = adapter_results[i].success;
            results[i].error = adapter_results[i].error_message;
        }
    }
    
    // === 缓存和内存管理 ===
    
    // 预加载常用资源
    inline void preload_common_resources() {
        content::ResourceAdapter::instance().preload_common_resources();
    }
    
    // 垃圾回收
    inline void garbage_collect() {
        content::ResourceAdapter::instance().garbage_collect();
    }
    
    // 强制清理未使用资源
    inline void cleanup_unused() {
        content::ResourceAdapter::instance().force_cleanup_unused_resources();
    }
    
    // 场景切换管理
    inline void prepare_scene_change() {
        content::ResourceAdapter::instance().prepare_for_scene_change();
    }
    
    inline void cleanup_after_scene_change() {
        content::ResourceAdapter::instance().cleanup_after_scene_change();
    }
    
    // === 统计和调试 ===
    
    struct Statistics {
        u32 total_resources;
        u32 loaded_resources;
        u32 loading_resources;
        u32 cache_hits;
        u32 cache_misses;
        u64 memory_usage_bytes;
        u64 gpu_memory_usage_bytes;
    };
    
    inline Statistics get_statistics() {
        auto adapter_stats = content::ResourceAdapter::instance().get_statistics();
        
        Statistics stats{};
        stats.total_resources = adapter_stats.resource_manager_stats.total_resources;
        stats.loaded_resources = adapter_stats.resource_manager_stats.loaded_resources;
        stats.loading_resources = adapter_stats.resource_manager_stats.loading_resources;
        stats.cache_hits = adapter_stats.resource_manager_stats.cache_hits;
        stats.cache_misses = adapter_stats.resource_manager_stats.cache_misses;
        stats.memory_usage_bytes = adapter_stats.resource_manager_stats.total_memory_usage;
        stats.gpu_memory_usage_bytes = adapter_stats.resource_manager_stats.gpu_memory_usage;
        
        return stats;
    }
    
    // 重置统计信息
    inline void reset_statistics() {
        content::ResourceAdapter::instance().reset_statistics();
    }
    
    // 输出调试信息
    inline void dump_statistics() {
        content::ResourceAdapter::instance().dump_all_resources();
    }
    
    // === 错误处理 ===
    
    inline const char* get_last_error() {
        return content::ResourceAdapter::instance().get_last_error();
    }
    
    inline void clear_last_error() {
        content::ResourceAdapter::instance().clear_last_error();
    }
    
    // === RAII资源管理器 ===
    
    using Resource = content::ScopedResource;
    
    // 便利函数：创建RAII资源
    inline Resource make_resource(const char* path) {
        return Resource(load(path));
    }
    
    inline Resource make_resource_async(const char* path) {
        return Resource(load_async(path));
    }
    
    // === 常用资源类型的便利宏 ===
    
    #define LOAD_MESH(path) primal::resources::load_mesh(path)
    #define LOAD_TEXTURE(path) primal::resources::load_texture(path)
    #define LOAD_MATERIAL(path) primal::resources::load_material(path)
    #define LOAD_AUDIO(path) primal::resources::load_audio(path)
    
    #define LOAD_MESH_ASYNC(path) primal::resources::load_mesh_async(path)
    #define LOAD_TEXTURE_ASYNC(path) primal::resources::load_texture_async(path)
    #define LOAD_MATERIAL_ASYNC(path) primal::resources::load_material_async(path)
    #define LOAD_AUDIO_ASYNC(path) primal::resources::load_audio_async(path)
    
    // === 优先级常量 ===
    
    namespace priority {
        constexpr Priority low = content::resource_priority::low;
        constexpr Priority normal = content::resource_priority::normal;
        constexpr Priority high = content::resource_priority::high;
        constexpr Priority critical = content::resource_priority::critical;
    }
    
    // === 状态常量 ===
    
    namespace state {
        constexpr State unloaded = content::resource_state::unloaded;
        constexpr State loading = content::resource_state::loading;
        constexpr State ready = content::resource_state::ready;
        constexpr State error = content::resource_state::error;
    }
}