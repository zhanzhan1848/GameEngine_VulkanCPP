#include "ResourceManager.h"
#include "ContentToEngine.h"
#include "Utilities/IOStream.h"
#include <filesystem>
#include <fstream>
#include <chrono>

namespace primal::content
{
    namespace
    {
        // 缓存文件魔数和版本
        constexpr u32 cache_file_magic = 0x52534D47; // "RSMG"
        constexpr u32 cache_file_version = 1;
        
        // 默认配置
        constexpr u32 max_async_load_threads = 4;
        constexpr u32 lru_cleanup_interval_ms = 30000; // 30秒
        constexpr u32 default_unused_timeout_ms = 300000; // 5分钟
        
        // 获取当前时间戳(毫秒)
        u64 get_current_time_ms() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        
        // 计算文件哈希
        u64 calculate_file_hash_impl(const char* path) {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open()) return 0;
            
            // 简单的哈希算法 (可以替换为更强的算法)
            u64 hash = 0xcbf29ce484222325ULL; // FNV-1a offset basis
            constexpr u64 prime = 0x100000001b3ULL; // FNV-1a prime
            
            char buffer[4096];
            while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
                for (std::streamsize i = 0; i < file.gcount(); ++i) {
                    hash ^= static_cast<u8>(buffer[i]);
                    hash *= prime;
                }
            }
            return hash;
        }
        
        // 获取文件大小
        u64 get_file_size(const char* path) {
            std::error_code ec;
            auto size = std::filesystem::file_size(path, ec);
            return ec ? 0 : size;
        }
        
        // 获取文件修改时间
        u64 get_file_last_modified(const char* path) {
            std::error_code ec;
            auto time = std::filesystem::last_write_time(path, ec);
            if (ec) return 0;
            
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();
        }
    }

    // 单例实现
    ResourceManager& ResourceManager::instance() {
        static ResourceManager instance;
        return instance;
    }

    bool ResourceManager::initialize() {
        // 初始化内存池
        _metadata_pool.reserve(16384);  // 预分配16K资源元数据
        _runtime_pool.reserve(16384);   // 预分配16K运行时信息
        
        // 设置默认缓存路径
        _cache_file_path = get_default_cache_path();
        
        // 加载持久化缓存
        load_cache();
        
        // 启动异步加载线程
        _shutdown_requested = false;
        _async_loader_thread = std::thread(&ResourceManager::async_loader_thread, this);
        
        return true;
    }

    void ResourceManager::shutdown() {
        // 停止异步加载线程
        {
            std::lock_guard<std::mutex> lock(_load_queue_mutex);
            _shutdown_requested = true;
        }
        _load_queue_cv.notify_all();
        
        if (_async_loader_thread.joinable()) {
            _async_loader_thread.join();
        }
        
        // 保存缓存
        save_cache();
        
        // 清理所有资源
        {
            std::lock_guard<std::mutex> meta_lock(_metadata_mutex);
            std::lock_guard<std::mutex> runtime_lock(_runtime_mutex);
            
            _path_to_handle.clear();
            _hash_to_handle.clear();
            _legacy_id_to_handle.clear();
            _metadata_pool.clear();
            _runtime_pool.clear();
        }
    }

    ResourceHandle ResourceManager::load_resource_sync(const char* path, resource_type::type type) {
        if (!path || !*path) return ResourceHandle::invalid();
        
        // 检查是否已经加载
        {
            std::lock_guard<std::mutex> lock(_metadata_mutex);
            auto it = _path_to_handle.find(path);
            if (it != _path_to_handle.end()) {
                mark_accessed(it->second);
                ++_cache_hits;
                return it->second;
            }
        }
        
        ++_cache_misses;
        
        // 创建新的资源句柄
        ResourceHandle handle = create_handle(type);
        if (!handle.is_valid()) return ResourceHandle::invalid();
        
        // 获取文件信息
        u64 file_hash = calculate_file_hash(path);
        u64 file_size = get_file_size(path);
        u64 last_modified = get_file_last_modified(path);
        
        if (file_size == 0) {
            // 文件不存在或无法访问
            return ResourceHandle::invalid();
        }
        
        // 设置元数据
        ResourceMetadata* metadata = get_metadata(handle);
        if (!metadata) return ResourceHandle::invalid();
        
        metadata->path = path;
        metadata->file_hash = file_hash;
        metadata->file_size = file_size;
        metadata->last_modified = last_modified;
        metadata->type = type;
        metadata->priority = resource_priority::normal;
        metadata->ref_count = 1;
        metadata->last_access_time = get_current_time_ms();
        metadata->access_count = 1;
        
        // 设置运行时信息
        ResourceRuntimeInfo* runtime_info = get_runtime_info(handle);
        if (!runtime_info) return ResourceHandle::invalid();
        
        runtime_info->state = resource_state::loading;
        
        // 同步加载资源数据
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            runtime_info->state = resource_state::error;
            runtime_info->error_message = "Failed to open file: " + std::string(path);
            return handle;
        }
        
        // 读取文件数据
        runtime_info->cpu_data = std::make_unique<u8[]>(file_size);
        runtime_info->cpu_data_size = static_cast<u32>(file_size);
        
        file.read(reinterpret_cast<char*>(runtime_info->cpu_data.get()), file_size);
        if (!file) {
            runtime_info->state = resource_state::error;
            runtime_info->error_message = "Failed to read file: " + std::string(path);
            return handle;
        }
        
        // 创建GPU资源 (通过现有的ContentToEngine系统)
        id::id_type legacy_id = create_resource(runtime_info->cpu_data.get(), 
                                               static_cast<asset_type::type>(type));
        
        if (id::is_valid(legacy_id)) {
            runtime_info->legacy_id = legacy_id;
            runtime_info->state = resource_state::ready;
            
            // 更新映射
            {
                std::lock_guard<std::mutex> lock(_metadata_mutex);
                _path_to_handle[path] = handle;
                _hash_to_handle[file_hash] = handle;
                _legacy_id_to_handle[legacy_id] = handle;
            }
        } else {
            runtime_info->state = resource_state::error;
            runtime_info->error_message = "Failed to create GPU resource";
        }
        
        return handle;
    }

    ResourceHandle ResourceManager::load_resource_sync(const void* data, u32 size, resource_type::type type, const char* name) {
        if (!data || size == 0) return ResourceHandle::invalid();
        
        // 创建新的资源句柄
        ResourceHandle handle = create_handle(type);
        if (!handle.is_valid()) return ResourceHandle::invalid();
        
        // 计算数据哈希
        u64 data_hash = 0xcbf29ce484222325ULL; // FNV-1a
        constexpr u64 prime = 0x100000001b3ULL;
        const u8* bytes = static_cast<const u8*>(data);
        for (u32 i = 0; i < size; ++i) {
            data_hash ^= bytes[i];
            data_hash *= prime;
        }
        
        // 检查是否已经存在相同数据的资源
        {
            std::lock_guard<std::mutex> lock(_metadata_mutex);
            auto it = _hash_to_handle.find(data_hash);
            if (it != _hash_to_handle.end()) {
                mark_accessed(it->second);
                ++_cache_hits;
                return it->second;
            }
        }
        
        ++_cache_misses;
        
        // 设置元数据
        ResourceMetadata* metadata = get_metadata(handle);
        if (!metadata) return ResourceHandle::invalid();
        
        metadata->path = name ? name : "<memory>";
        metadata->file_hash = data_hash;
        metadata->file_size = size;
        metadata->last_modified = get_current_time_ms();
        metadata->type = type;
        metadata->priority = resource_priority::normal;
        metadata->ref_count = 1;
        metadata->last_access_time = get_current_time_ms();
        metadata->access_count = 1;
        
        // 设置运行时信息
        ResourceRuntimeInfo* runtime_info = get_runtime_info(handle);
        if (!runtime_info) return ResourceHandle::invalid();
        
        runtime_info->state = resource_state::loading;
        
        // 复制数据
        runtime_info->cpu_data = std::make_unique<u8[]>(size);
        runtime_info->cpu_data_size = size;
        std::memcpy(runtime_info->cpu_data.get(), data, size);
        
        // 创建GPU资源
        id::id_type legacy_id = create_resource(data, static_cast<asset_type::type>(type));
        
        if (id::is_valid(legacy_id)) {
            runtime_info->legacy_id = legacy_id;
            runtime_info->state = resource_state::ready;
            
            // 更新映射
            {
                std::lock_guard<std::mutex> lock(_metadata_mutex);
                _hash_to_handle[data_hash] = handle;
                _legacy_id_to_handle[legacy_id] = handle;
                if (name) {
                    _path_to_handle[name] = handle;
                }
            }
        } else {
            runtime_info->state = resource_state::error;
            runtime_info->error_message = "Failed to create GPU resource";
        }
        
        return handle;
    }

    ResourceHandle ResourceManager::load_resource_async(const char* path, resource_type::type type) {
        if (!path || !*path) return ResourceHandle::invalid();
        
        // 检查是否已经加载
        {
            std::lock_guard<std::mutex> lock(_metadata_mutex);
            auto it = _path_to_handle.find(path);
            if (it != _path_to_handle.end()) {
                mark_accessed(it->second);
                ++_cache_hits;
                return it->second;
            }
        }
        
        ++_cache_misses;
        
        // 创建新的资源句柄
        ResourceHandle handle = create_handle(type);
        if (!handle.is_valid()) return ResourceHandle::invalid();
        
        // 设置基本元数据
        ResourceMetadata* metadata = get_metadata(handle);
        if (!metadata) return ResourceHandle::invalid();
        
        metadata->path = path;
        metadata->type = type;
        metadata->priority = resource_priority::normal;
        metadata->ref_count = 1;
        metadata->last_access_time = get_current_time_ms();
        metadata->access_count = 1;
        
        // 设置运行时信息
        ResourceRuntimeInfo* runtime_info = get_runtime_info(handle);
        if (!runtime_info) return ResourceHandle::invalid();
        
        runtime_info->state = resource_state::loading;
        
        // 添加到加载队列
        {
            std::lock_guard<std::mutex> lock(_load_queue_mutex);
            _load_queue.push_back(handle);
        }
        _load_queue_cv.notify_one();
        
        // 更新路径映射
        {
            std::lock_guard<std::mutex> lock(_metadata_mutex);
            _path_to_handle[path] = handle;
        }
        
        return handle;
    }

    bool ResourceManager::is_ready(ResourceHandle handle) const {
        if (!validate_handle(handle)) return false;
        
        const ResourceRuntimeInfo* runtime_info = get_runtime_info(handle);
        return runtime_info && runtime_info->state == resource_state::ready;
    }

    resource_state::type ResourceManager::get_state(ResourceHandle handle) const {
        if (!validate_handle(handle)) return resource_state::error;
        
        const ResourceRuntimeInfo* runtime_info = get_runtime_info(handle);
        return runtime_info ? runtime_info->state.load() : resource_state::error;
    }

    void* ResourceManager::get_gpu_resource(ResourceHandle handle) const {
        if (!validate_handle(handle)) return nullptr;
        
        const ResourceRuntimeInfo* runtime_info = get_runtime_info(handle);
        if (!runtime_info || runtime_info->state != resource_state::ready) {
            return nullptr;
        }
        
        return runtime_info->gpu_resource;
    }

    const u8* ResourceManager::get_cpu_data(ResourceHandle handle, u32& size) const {
        size = 0;
        if (!validate_handle(handle)) return nullptr;
        
        const ResourceRuntimeInfo* runtime_info = get_runtime_info(handle);
        if (!runtime_info || !runtime_info->cpu_data) {
            return nullptr;
        }
        
        size = runtime_info->cpu_data_size;
        return runtime_info->cpu_data.get();
    }

    void ResourceManager::retain_resource(ResourceHandle handle) {
        if (!validate_handle(handle)) return;
        
        ResourceMetadata* metadata = get_metadata(handle);
        if (metadata) {
            metadata->ref_count.fetch_add(1);
            mark_accessed(handle);
        }
    }

    void ResourceManager::release_resource(ResourceHandle handle) {
        if (!validate_handle(handle)) return;
        
        ResourceMetadata* metadata = get_metadata(handle);
        if (metadata) {
            u32 old_count = metadata->ref_count.fetch_sub(1);
            if (old_count == 1) {
                // 引用计数归零，可以考虑卸载
                // 这里不立即卸载，而是在垃圾回收时处理
            }
        }
    }

    void ResourceManager::mark_accessed(ResourceHandle handle) {
        if (!validate_handle(handle)) return;
        
        ResourceMetadata* metadata = get_metadata(handle);
        if (metadata) {
            metadata->last_access_time = get_current_time_ms();
            metadata->access_count.fetch_add(1);
        }
    }

    void ResourceManager::set_priority(ResourceHandle handle, resource_priority::type priority) {
        if (!validate_handle(handle)) return;
        
        ResourceMetadata* metadata = get_metadata(handle);
        if (metadata) {
            metadata->priority = priority;
        }
    }

    ResourceManager::Statistics ResourceManager::get_statistics() const {
        Statistics stats{};
        
        std::lock_guard<std::mutex> meta_lock(_metadata_mutex);
        std::lock_guard<std::mutex> runtime_lock(_runtime_mutex);
        
        stats.total_resources = static_cast<u32>(_metadata_pool.size());
        stats.cache_hits = _cache_hits.load();
        stats.cache_misses = _cache_misses.load();
        stats.total_memory_usage = _current_cpu_usage.load();
        stats.gpu_memory_usage = _current_gpu_usage.load();
        
        // 统计各状态资源数量
        for (u32 i = 0; i < _runtime_pool.size(); ++i) {
            if (_runtime_pool.is_valid_id(i)) {
                const auto& runtime_info = _runtime_pool[i];
                switch (runtime_info.state.load()) {
                    case resource_state::ready:
                        ++stats.loaded_resources;
                        break;
                    case resource_state::loading:
                        ++stats.loading_resources;
                        break;
                }
            }
        }
        
        return stats;
    }

    bool ResourceManager::save_cache(const char* cache_file_path) {
        const std::string path = cache_file_path ? cache_file_path : _cache_file_path;
        
        std::ofstream file(path, std::ios::binary);
        if (!file.is_open()) return false;
        
        // 写入文件头
        file.write(reinterpret_cast<const char*>(&cache_file_magic), sizeof(cache_file_magic));
        file.write(reinterpret_cast<const char*>(&cache_file_version), sizeof(cache_file_version));
        
        std::lock_guard<std::mutex> lock(_metadata_mutex);
        
        // 写入资源数量
        u32 resource_count = static_cast<u32>(_path_to_handle.size());
        file.write(reinterpret_cast<const char*>(&resource_count), sizeof(resource_count));
        
        // 写入每个资源的缓存信息
        for (const auto& [path, handle] : _path_to_handle) {
            const ResourceMetadata* metadata = get_metadata(handle);
            if (!metadata) continue;
            
            // 写入路径长度和路径
            u32 path_length = static_cast<u32>(path.length());
            file.write(reinterpret_cast<const char*>(&path_length), sizeof(path_length));
            file.write(path.c_str(), path_length);
            
            // 写入缓存数据
            file.write(reinterpret_cast<const char*>(&metadata->file_hash), sizeof(metadata->file_hash));
            file.write(reinterpret_cast<const char*>(&metadata->file_size), sizeof(metadata->file_size));
            file.write(reinterpret_cast<const char*>(&metadata->last_modified), sizeof(metadata->last_modified));
            file.write(reinterpret_cast<const char*>(&metadata->type), sizeof(metadata->type));
            file.write(reinterpret_cast<const char*>(&metadata->priority), sizeof(metadata->priority));
            
            u64 last_access = metadata->last_access_time.load();
            u32 access_count = metadata->access_count.load();
            file.write(reinterpret_cast<const char*>(&last_access), sizeof(last_access));
            file.write(reinterpret_cast<const char*>(&access_count), sizeof(access_count));
        }
        
        return file.good();
    }

    bool ResourceManager::load_cache(const char* cache_file_path) {
        const std::string path = cache_file_path ? cache_file_path : _cache_file_path;
        
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false; // 缓存文件不存在是正常的
        
        // 读取文件头
        u32 magic, version;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        file.read(reinterpret_cast<char*>(&version), sizeof(version));
        
        if (magic != cache_file_magic || version != cache_file_version) {
            return false; // 版本不匹配
        }
        
        // 读取资源数量
        u32 resource_count;
        file.read(reinterpret_cast<char*>(&resource_count), sizeof(resource_count));
        
        // 读取缓存信息 (但不立即加载资源)
        for (u32 i = 0; i < resource_count && file.good(); ++i) {
            // 读取路径
            u32 path_length;
            file.read(reinterpret_cast<char*>(&path_length), sizeof(path_length));
            
            std::string resource_path(path_length, '\0');
            file.read(&resource_path[0], path_length);
            
            // 读取缓存数据
            u64 file_hash, file_size, last_modified, last_access;
            resource_type::type type;
            resource_priority::type priority;
            u32 access_count;
            
            file.read(reinterpret_cast<char*>(&file_hash), sizeof(file_hash));
            file.read(reinterpret_cast<char*>(&file_size), sizeof(file_size));
            file.read(reinterpret_cast<char*>(&last_modified), sizeof(last_modified));
            file.read(reinterpret_cast<char*>(&type), sizeof(type));
            file.read(reinterpret_cast<char*>(&priority), sizeof(priority));
            file.read(reinterpret_cast<char*>(&last_access), sizeof(last_access));
            file.read(reinterpret_cast<char*>(&access_count), sizeof(access_count));
            
            // 验证文件是否仍然存在且未修改
            if (get_file_last_modified(resource_path.c_str()) == last_modified &&
                get_file_size(resource_path.c_str()) == file_size) {
                
                // 如果是高频访问的资源，考虑预加载
                if (access_count > 10 && priority >= resource_priority::normal) {
                    // 可以在这里添加预加载逻辑
                }
            }
        }
        
        return file.good();
    }

    // 私有实现方法
    ResourceHandle ResourceManager::create_handle(resource_type::type type) {
        std::lock_guard<std::mutex> meta_lock(_metadata_mutex);
        std::lock_guard<std::mutex> runtime_lock(_runtime_mutex);
        
        // 分配元数据
        u32 metadata_id = _metadata_pool.add();
        if (metadata_id == utl::free_list<ResourceMetadata>::invalid_id) {
            return ResourceHandle::invalid();
        }
        
        // 分配运行时信息
        u32 runtime_id = _runtime_pool.add();
        if (runtime_id == utl::free_list<ResourceRuntimeInfo>::invalid_id) {
            _metadata_pool.remove(metadata_id);
            return ResourceHandle::invalid();
        }
        
        // 确保索引一致
        assert(metadata_id == runtime_id);
        
        ResourceHandle handle;
        handle.index = metadata_id;
        handle.type = type;
        handle.generation = _generation_counter.fetch_add(1);
        
        return handle;
    }

    bool ResourceManager::validate_handle(ResourceHandle handle) const {
        return handle.is_valid() && 
               handle.index < _metadata_pool.capacity() &&
               _metadata_pool.is_valid_id(handle.index);
    }

    ResourceMetadata* ResourceManager::get_metadata(ResourceHandle handle) const {
        if (!validate_handle(handle)) return nullptr;
        return &_metadata_pool[handle.index];
    }

    ResourceRuntimeInfo* ResourceManager::get_runtime_info(ResourceHandle handle) const {
        if (!validate_handle(handle)) return nullptr;
        return &_runtime_pool[handle.index];
    }

    void ResourceManager::async_loader_thread() {
        while (!_shutdown_requested) {
            std::unique_lock<std::mutex> lock(_load_queue_mutex);
            _load_queue_cv.wait(lock, [this] { 
                return !_load_queue.empty() || _shutdown_requested; 
            });
            
            if (_shutdown_requested) break;
            
            // 处理加载队列
            std::vector<ResourceHandle> handles_to_process;
            handles_to_process.swap(_load_queue);
            lock.unlock();
            
            // 处理每个资源
            for (ResourceHandle handle : handles_to_process) {
                ResourceMetadata* metadata = get_metadata(handle);
                ResourceRuntimeInfo* runtime_info = get_runtime_info(handle);
                
                if (!metadata || !runtime_info) continue;
                
                // 异步加载文件
                const std::string& path = metadata->path;
                
                // 获取文件信息
                u64 file_size = get_file_size(path.c_str());
                if (file_size == 0) {
                    runtime_info->state = resource_state::error;
                    runtime_info->error_message = "File not found: " + path;
                    continue;
                }
                
                // 更新元数据
                metadata->file_hash = calculate_file_hash(path.c_str());
                metadata->file_size = file_size;
                metadata->last_modified = get_file_last_modified(path.c_str());
                
                // 读取文件
                std::ifstream file(path, std::ios::binary);
                if (!file.is_open()) {
                    runtime_info->state = resource_state::error;
                    runtime_info->error_message = "Failed to open file: " + path;
                    continue;
                }
                
                runtime_info->cpu_data = std::make_unique<u8[]>(file_size);
                runtime_info->cpu_data_size = static_cast<u32>(file_size);
                
                file.read(reinterpret_cast<char*>(runtime_info->cpu_data.get()), file_size);
                if (!file) {
                    runtime_info->state = resource_state::error;
                    runtime_info->error_message = "Failed to read file: " + path;
                    continue;
                }
                
                // 创建GPU资源
                id::id_type legacy_id = create_resource(runtime_info->cpu_data.get(), 
                                                       static_cast<asset_type::type>(metadata->type));
                
                if (id::is_valid(legacy_id)) {
                    runtime_info->legacy_id = legacy_id;
                    runtime_info->state = resource_state::ready;
                    
                    // 更新映射
                    {
                        std::lock_guard<std::mutex> map_lock(_metadata_mutex);
                        _hash_to_handle[metadata->file_hash] = handle;
                        _legacy_id_to_handle[legacy_id] = handle;
                    }
                } else {
                    runtime_info->state = resource_state::error;
                    runtime_info->error_message = "Failed to create GPU resource";
                }
            }
        }
    }

    u64 ResourceManager::calculate_file_hash(const char* path) const {
        return calculate_file_hash_impl(path);
    }

    std::string ResourceManager::get_default_cache_path() const {
        return "./resource_cache.bin";
    }

    id::id_type ResourceManager::get_legacy_id(ResourceHandle handle) const {
        if (!validate_handle(handle)) return id::invalid_id;
        
        const ResourceRuntimeInfo* runtime_info = get_runtime_info(handle);
        return runtime_info ? runtime_info->legacy_id : id::invalid_id;
    }

    ResourceHandle ResourceManager::get_handle_from_legacy_id(id::id_type legacy_id) const {
        std::lock_guard<std::mutex> lock(_metadata_mutex);
        auto it = _legacy_id_to_handle.find(legacy_id);
        return it != _legacy_id_to_handle.end() ? it->second : ResourceHandle::invalid();
    }

    const char* ResourceManager::get_error_message(ResourceHandle handle) const {
        if (!validate_handle(handle)) return "Invalid handle";
        
        const ResourceRuntimeInfo* runtime_info = get_runtime_info(handle);
        return runtime_info && !runtime_info->error_message.empty() ? 
               runtime_info->error_message.c_str() : nullptr;
    }

    void ResourceManager::garbage_collect() {
        const u64 current_time = get_current_time_ms();
        std::vector<ResourceHandle> handles_to_unload;
        
        {
            std::lock_guard<std::mutex> meta_lock(_metadata_mutex);
            std::lock_guard<std::mutex> runtime_lock(_runtime_mutex);
            
            for (u32 i = 0; i < _metadata_pool.size(); ++i) {
                if (!_metadata_pool.is_valid_id(i)) continue;
                
                const auto& metadata = _metadata_pool[i];
                const auto& runtime_info = _runtime_pool[i];
                
                // 检查是否应该卸载
                if (metadata.ref_count.load() == 0 && 
                    runtime_info.state == resource_state::ready &&
                    (current_time - metadata.last_access_time.load()) > default_unused_timeout_ms) {
                    
                    ResourceHandle handle;
                    handle.index = i;
                    handle.type = metadata.type;
                    handle.generation = 0; // 不需要精确的generation用于卸载
                    
                    handles_to_unload.push_back(handle);
                }
            }
        }
        
        // 卸载未使用的资源
        for (ResourceHandle handle : handles_to_unload) {
            force_unload_resource(handle);
        }
    }

    void ResourceManager::force_unload_resource(ResourceHandle handle) {
        if (!validate_handle(handle)) return;
        
        ResourceRuntimeInfo* runtime_info = get_runtime_info(handle);
        if (!runtime_info) return;
        
        // 销毁GPU资源
        if (id::is_valid(runtime_info->legacy_id)) {
            ResourceMetadata* metadata = get_metadata(handle);
            if (metadata) {
                destroy_resource(runtime_info->legacy_id, static_cast<asset_type::type>(metadata->type));
            }
        }
        
        // 清理运行时数据
        runtime_info->state = resource_state::unloaded;
        runtime_info->gpu_resource = nullptr;
        runtime_info->cpu_data.reset();
        runtime_info->cpu_data_size = 0;
        runtime_info->legacy_id = id::invalid_id;
        runtime_info->error_message.clear();
    }
}