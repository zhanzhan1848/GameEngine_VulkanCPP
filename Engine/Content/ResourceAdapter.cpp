#include "ResourceAdapter.h"
#include "Utilities/IOStream.h"
#include <filesystem>

namespace primal::content
{
    // 单例实现
    ResourceAdapter& ResourceAdapter::instance() {
        static ResourceAdapter instance;
        return instance;
    }

    bool ResourceAdapter::initialize() {
        if (_initialized.load()) return true;
        
        // 初始化ResourceManager
        if (!ResourceManager::instance().initialize()) {
            set_error("Failed to initialize ResourceManager");
            return false;
        }
        
        _initialized.store(true);
        return true;
    }

    void ResourceAdapter::shutdown() {
        if (!_initialized.load()) return;
        
        // 关闭ResourceManager
        ResourceManager::instance().shutdown();
        
        // 重置统计
        _cache_hits.store(0);
        _cache_misses.store(0);
        _conversion_operations.store(0);
        
        _initialized.store(false);
    }

    // === 兼容现有ContentToEngine接口 ===
    
    id::id_type ResourceAdapter::create_resource_legacy(const void* data, asset_type::type type) {
        if (!_initialized.load()) {
            set_error("ResourceAdapter not initialized");
            return id::invalid_id;
        }
        
        if (!validate_data(data, 1)) {
            set_error("Invalid data pointer");
            return id::invalid_id;
        }
        
        // 直接调用原有的create_resource函数
        id::id_type legacy_id = create_resource(data, type);
        
        if (id::is_valid(legacy_id)) {
            ++_conversion_operations;
        }
        
        return legacy_id;
    }

    void ResourceAdapter::destroy_resource_legacy(id::id_type id, asset_type::type type) {
        if (!_initialized.load()) return;
        
        if (id::is_valid(id)) {
            destroy_resource(id, type);
            ++_conversion_operations;
        }
    }

    // === 新的高级接口 ===
    
    ResourceHandle ResourceAdapter::load_from_file(const char* path, resource_type::type type) {
        if (!_initialized.load()) {
            set_error("ResourceAdapter not initialized");
            return ResourceHandle::invalid();
        }
        
        if (!validate_path(path)) {
            set_error("Invalid file path");
            return ResourceHandle::invalid();
        }
        
        return ResourceManager::instance().load_resource_sync(path, type);
    }

    ResourceHandle ResourceAdapter::load_from_file_async(const char* path, resource_type::type type) {
        if (!_initialized.load()) {
            set_error("ResourceAdapter not initialized");
            return ResourceHandle::invalid();
        }
        
        if (!validate_path(path)) {
            set_error("Invalid file path");
            return ResourceHandle::invalid();
        }
        
        return ResourceManager::instance().load_resource_async(path, type);
    }

    ResourceHandle ResourceAdapter::load_from_memory(const void* data, u32 size, resource_type::type type, const char* name) {
        if (!_initialized.load()) {
            set_error("ResourceAdapter not initialized");
            return ResourceHandle::invalid();
        }
        
        if (!validate_data(data, size)) {
            set_error("Invalid data or size");
            return ResourceHandle::invalid();
        }
        
        return ResourceManager::instance().load_resource_sync(data, size, type, name);
    }

    // === 资源访问 ===
    
    bool ResourceAdapter::is_resource_ready(ResourceHandle handle) const {
        if (!_initialized.load()) return false;
        return ResourceManager::instance().is_ready(handle);
    }

    void* ResourceAdapter::get_gpu_resource(ResourceHandle handle) const {
        if (!_initialized.load()) return nullptr;
        return ResourceManager::instance().get_gpu_resource(handle);
    }

    const u8* ResourceAdapter::get_cpu_data(ResourceHandle handle, u32& size) const {
        if (!_initialized.load()) {
            size = 0;
            return nullptr;
        }
        return ResourceManager::instance().get_cpu_data(handle, size);
    }

    // === 资源生命周期管理 ===
    
    void ResourceAdapter::retain_resource(ResourceHandle handle) {
        if (!_initialized.load()) return;
        ResourceManager::instance().retain_resource(handle);
    }

    void ResourceAdapter::release_resource(ResourceHandle handle) {
        if (!_initialized.load()) return;
        ResourceManager::instance().release_resource(handle);
    }

    // === 资源状态和优先级 ===
    
    resource_state::type ResourceAdapter::get_resource_state(ResourceHandle handle) const {
        if (!_initialized.load()) return resource_state::error;
        return ResourceManager::instance().get_state(handle);
    }

    void ResourceAdapter::set_resource_priority(ResourceHandle handle, resource_priority::type priority) {
        if (!_initialized.load()) return;
        ResourceManager::instance().set_priority(handle, priority);
    }

    // === 兼容性转换 ===
    
    ResourceHandle ResourceAdapter::get_handle_from_legacy_id(id::id_type legacy_id) const {
        if (!_initialized.load()) return ResourceHandle::invalid();
        
        ResourceHandle handle = ResourceManager::instance().get_handle_from_legacy_id(legacy_id);
        
        if (handle.is_valid()) {
            ++_cache_hits;
        } else {
            ++_cache_misses;
        }
        
        ++_conversion_operations;
        return handle;
    }

    id::id_type ResourceAdapter::get_legacy_id_from_handle(ResourceHandle handle) const {
        if (!_initialized.load()) return id::invalid_id;
        
        id::id_type legacy_id = ResourceManager::instance().get_legacy_id(handle);
        ++_conversion_operations;
        return legacy_id;
    }

    // === 资源类型转换 ===
    
    resource_type::type ResourceAdapter::convert_asset_type(asset_type::type asset_type) {
        switch (asset_type) {
            case asset_type::mesh:
                return resource_type::geometry;
            case asset_type::texture:
                return resource_type::texture;
            case asset_type::material:
                return resource_type::material;
            case asset_type::audio:
                return resource_type::audio;
            case asset_type::animation:
                return resource_type::animation;
            case asset_type::skeleton:
                return resource_type::skeleton;
            default:
                return resource_type::unknown;
        }
    }

    asset_type::type ResourceAdapter::convert_resource_type(resource_type::type resource_type) {
        switch (resource_type) {
            case resource_type::geometry:
                return asset_type::mesh;
            case resource_type::texture:
                return asset_type::texture;
            case resource_type::material:
                return asset_type::material;
            case resource_type::audio:
                return asset_type::audio;
            case resource_type::animation:
                return asset_type::animation;
            case resource_type::skeleton:
                return asset_type::skeleton;
            default:
                return asset_type::unkonwn;
        }
    }

    // === 批量操作接口 ===
    
    void ResourceAdapter::load_resources_batch(const BatchLoadRequest* requests, u32 count, BatchLoadResult* results) {
        if (!_initialized.load() || !requests || !results || count == 0) {
            set_error("Invalid batch load parameters");
            return;
        }
        
        for (u32 i = 0; i < count; ++i) {
            const auto& request = requests[i];
            auto& result = results[i];
            
            result.handle = load_from_file(request.path, request.type);
            result.success = result.handle.is_valid();
            
            if (result.success && request.priority != resource_priority::normal) {
                set_resource_priority(result.handle, request.priority);
            }
            
            if (!result.success) {
                result.error_message = get_last_error();
            }
        }
    }

    void ResourceAdapter::load_resources_batch_async(const BatchLoadRequest* requests, u32 count, BatchLoadResult* results) {
        if (!_initialized.load() || !requests || !results || count == 0) {
            set_error("Invalid batch load parameters");
            return;
        }
        
        for (u32 i = 0; i < count; ++i) {
            const auto& request = requests[i];
            auto& result = results[i];
            
            result.handle = load_from_file_async(request.path, request.type);
            result.success = result.handle.is_valid();
            
            if (result.success && request.priority != resource_priority::normal) {
                set_resource_priority(result.handle, request.priority);
            }
            
            if (!result.success) {
                result.error_message = get_last_error();
            }
        }
    }

    // === 缓存和预加载 ===
    
    void ResourceAdapter::preload_common_resources() {
        if (!_initialized.load()) return;
        
        // 这里可以根据项目需求预加载常用资源
        // 例如：默认材质、常用纹理、UI资源等
        
        // 示例：预加载默认资源
        const char* common_resources[] = {
            "Assets/Textures/default_diffuse.dds",
            "Assets/Textures/default_normal.dds",
            "Assets/Materials/default_material.mat",
            // 添加更多常用资源...
        };
        
        for (const char* path : common_resources) {
            if (std::filesystem::exists(path)) {
                // 根据文件扩展名确定类型
                resource_type::type type = resource_type::unknown;
                std::string ext = std::filesystem::path(path).extension().string();
                
                if (ext == ".dds" || ext == ".png" || ext == ".jpg") {
                    type = resource_type::texture;
                } else if (ext == ".mat") {
                    type = resource_type::material;
                }
                
                if (type != resource_type::unknown) {
                    ResourceHandle handle = load_from_file_async(path, type);
                    if (handle.is_valid()) {
                        set_resource_priority(handle, resource_priority::high);
                    }
                }
            }
        }
    }

    void ResourceAdapter::prepare_for_scene_change() {
        if (!_initialized.load()) return;
        
        // 场景切换前的准备工作
        // 1. 保存当前缓存状态
        ResourceManager::instance().save_cache();
        
        // 2. 标记当前场景资源为低优先级
        // (具体实现需要根据场景管理系统来定制)
    }

    void ResourceAdapter::cleanup_after_scene_change() {
        if (!_initialized.load()) return;
        
        // 场景切换后的清理工作
        garbage_collect();
    }

    void ResourceAdapter::garbage_collect() {
        if (!_initialized.load()) return;
        ResourceManager::instance().garbage_collect();
    }

    void ResourceAdapter::force_cleanup_unused_resources() {
        if (!_initialized.load()) return;
        
        // 强制清理所有未使用的资源
        // 这是一个更激进的清理策略，通常在内存紧张时使用
        garbage_collect();
        
        // 可以添加更多清理逻辑，比如清理CPU缓存等
    }

    // === 统计和调试 ===
    
    ResourceAdapter::AdapterStatistics ResourceAdapter::get_statistics() const {
        AdapterStatistics stats{};
        
        if (_initialized.load()) {
            stats.resource_manager_stats = ResourceManager::instance().get_statistics();
        }
        
        stats.adapter_cache_hits = _cache_hits.load();
        stats.adapter_cache_misses = _cache_misses.load();
        stats.conversion_operations = _conversion_operations.load();
        
        // 统计legacy资源数量需要访问ContentToEngine的内部状态
        // 这里暂时设为0，实际实现时需要根据具体情况调整
        stats.legacy_resources_count = 0;
        
        return stats;
    }

    void ResourceAdapter::reset_statistics() {
        _cache_hits.store(0);
        _cache_misses.store(0);
        _conversion_operations.store(0);
    }

    void ResourceAdapter::dump_resource_info(ResourceHandle handle) const {
        if (!_initialized.load() || !handle.is_valid()) return;
        
        // 输出资源详细信息 (用于调试)
        // 实际实现中可以使用日志系统
        printf("Resource Handle: index=%u, type=%u, generation=%u\n", 
               handle.index, handle.type, handle.generation);
        
        resource_state::type state = get_resource_state(handle);
        printf("State: %u\n", static_cast<u32>(state));
        
        u32 size;
        const u8* data = get_cpu_data(handle, size);
        printf("CPU Data: %p, Size: %u\n", data, size);
        
        void* gpu_resource = get_gpu_resource(handle);
        printf("GPU Resource: %p\n", gpu_resource);
    }

    void ResourceAdapter::dump_all_resources() const {
        if (!_initialized.load()) return;
        
        // 输出所有资源的统计信息
        AdapterStatistics stats = get_statistics();
        
        printf("=== Resource Adapter Statistics ===\n");
        printf("Total Resources: %u\n", stats.resource_manager_stats.total_resources);
        printf("Loaded Resources: %u\n", stats.resource_manager_stats.loaded_resources);
        printf("Loading Resources: %u\n", stats.resource_manager_stats.loading_resources);
        printf("Cache Hits: %u\n", stats.resource_manager_stats.cache_hits);
        printf("Cache Misses: %u\n", stats.resource_manager_stats.cache_misses);
        printf("Total Memory Usage: %llu bytes\n", stats.resource_manager_stats.total_memory_usage);
        printf("GPU Memory Usage: %llu bytes\n", stats.resource_manager_stats.gpu_memory_usage);
        printf("Adapter Cache Hits: %u\n", stats.adapter_cache_hits);
        printf("Adapter Cache Misses: %u\n", stats.adapter_cache_misses);
        printf("Conversion Operations: %u\n", stats.conversion_operations);
        printf("===================================\n");
    }

    // === 私有辅助方法 ===
    
    void ResourceAdapter::set_error(const std::string& error) {
        std::lock_guard<std::mutex> lock(_error_mutex);
        _last_error = error;
    }

    bool ResourceAdapter::validate_path(const char* path) const {
        return path && *path && strlen(path) > 0;
    }

    bool ResourceAdapter::validate_data(const void* data, u32 size) const {
        return data != nullptr && size > 0;
    }

    void ResourceAdapter::update_legacy_cache(id::id_type legacy_id, ResourceHandle handle) {
        // 更新legacy ID到handle的映射缓存
        // 这个功能已经在ResourceManager中实现
    }

    void ResourceAdapter::remove_from_legacy_cache(id::id_type legacy_id) {
        // 从legacy缓存中移除映射
        // 这个功能已经在ResourceManager中实现
    }
}