/**
 * @file RHIResource.h
 * @brief RHI资源基类定义（RAII模式）
 * @details 使用RAII模式实现GPU资源的自动生命周期管理
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-29
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include "RHITypes.h"

namespace primal::graphics::rhi {

// === 前向声明 ===
class RHIDeviceBase;
template<typename T> class RHIAllocator;

/**
 * @brief 资源使用标志位
 * @details 描述资源的预期用途
 */
enum class ResourceUsage : u32 {
    None = 0x00000000,
    ShaderResource = 0x00000001,       ///< 作为着色器资源
    RenderTarget = 0x00000002,          ///< 作为渲染目标
    DepthStencil = 0x00000004,          ///< 作为深度模板缓冲
    UnorderedAccess = 0x00000008,       ///< 作为无序访问视图
    CopySource = 0x00000010,            ///< 作为复制源
    CopyDest = 0x00000020,              ///< 作为复制目标
    ResolveSource = 0x00000040,         ///< 作为解析源
    ResolveDest = 0x00000080,           ///< 作为解析目标
    Present = 0x00000100,               ///< 作为呈现目标
    IndexBuffer = 0x00000200,           ///< 作为索引缓冲
    VertexBuffer = 0x00000400,          ///< 作为顶点缓冲
    ConstantBuffer = 0x00000800,        ///< 作为常量缓冲
    IndirectArg = 0x00001000            ///< 作为间接参数缓冲
};

// 支持位运算操作
inline ResourceUsage operator|(ResourceUsage a, ResourceUsage b) {
    return static_cast<ResourceUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}

inline ResourceUsage operator&(ResourceUsage a, ResourceUsage b) {
    return static_cast<ResourceUsage>(static_cast<u32>(a) & static_cast<u32>(b));
}

inline bool HasUsage(ResourceUsage usage, ResourceUsage flag) {
    return (usage & flag) != ResourceUsage::None;
}

/**
 * @brief 资源描述符基类
 * @details 所有资源类型的基础描述符
 */
struct ResourceDesc {
    ResourceType type;          ///< 资源类型
    ResourceUsage usage;        ///< 资源用途
    GPUMemoryUsage memoryUsage; ///< 内存使用模式
    u64 size;             ///< 资源大小（字节）
    const char* name;           ///< 资源名称（用于调试）
    
    ResourceDesc() : type(ResourceType::Unknown), usage(ResourceUsage::None),
                    memoryUsage(GPUMemoryUsage::Unknown), size(0), name(nullptr) {}
    
    ResourceDesc(ResourceType t, ResourceUsage u, GPUMemoryUsage mem, u64 sz, const char* n = nullptr)
        : type(t), usage(u), memoryUsage(mem), size(sz), name(n) {}
};

/**
 * @brief 资源子资源描述符
 * @details 用于描述资源的子资源（如纹理的Mip层级、数组切片等）
 */
struct SubresourceDesc {
    u32 mipLevel;          ///< Mip层级
    u32 arraySlice;        ///< 数组切片
    u32 plane;             ///< 平面（用于多平面格式）
    
    SubresourceDesc() : mipLevel(0), arraySlice(0), plane(0) {}
    SubresourceDesc(u32 mip, u32 array, u32 p = 0)
        : mipLevel(mip), arraySlice(array), plane(p) {}
};

/**
 * @brief 资源映射描述符
 * @details 描述CPU如何访问GPU资源
 */
struct ResourceMapDesc {
    void* data;                ///< 映射的数据指针
    u64 offset;            ///< 映射偏移量（字节）
    u64 size;              ///< 映射大小（字节）
    bool isReadback;            ///< 是否为回读映射
    bool isPersistent;          ///< 是否为持久映射
    
    ResourceMapDesc() : data(nullptr), offset(0), size(0), isReadback(false), isPersistent(false) {}
};

/**
 * @brief 资源统计信息
 * @details 用于监控资源使用情况
 */
struct ResourceStats {
    u32 bufferCount;       ///< 缓冲区数量
    u32 textureCount;      ///< 纹理数量
    u32 pipelineCount;     ///< 管线数量
    u64 totalMemoryUsage;  ///< 总内存使用量
    u64 bufferMemoryUsage; ///< 缓冲区内存使用量
    u64 textureMemoryUsage; ///< 纹理内存使用量
    
    ResourceStats() : bufferCount(0), textureCount(0), pipelineCount(0),
                     totalMemoryUsage(0), bufferMemoryUsage(0), textureMemoryUsage(0) {}
};

/**
 * @brief RHI资源基类（RAII模式）
 * @details 管理GPU资源的生命周期，提供自动资源清理
 */
class RHIResource {
    template<typename T> friend class RHIAllocator;
public:
    // === 构造函数和析构函数 ===
    
    /**
     * @brief 构造函数
     * @param device 设备引用
     * @param desc 资源描述符
     */
    RHIResource(RHIDeviceBase& device, const ResourceDesc& desc)
        : device_(device), desc_(desc), handle_(handles::INVALID_RESOURCE),
          state_(ResourceState::Created), refCount_(0), mappedData_(nullptr) {}
    
    /**
     * @brief 虚析构函数
     */
    virtual ~RHIResource() {
        if (state_ != ResourceState::Destroyed) {
            Destroy();
        }
    }
    
    // === 禁用拷贝，支持移动 ===
    
    RHIResource(const RHIResource&) = delete;
    RHIResource& operator=(const RHIResource&) = delete;
    
    RHIResource(RHIResource&& other) noexcept
        : device_(other.device_), desc_(other.desc_), handle_(other.handle_),
          state_(other.state_), refCount_(other.refCount_.exchange(0)), mappedData_(other.mappedData_) {
        other.handle_ = handles::INVALID_RESOURCE;
        other.state_ = ResourceState::Destroyed;
        other.refCount_ = 0;
        other.mappedData_ = nullptr;
    }
    
    RHIResource& operator=(RHIResource&& other) noexcept;
    
    // === 核心接口方法 ===
    
    /**
     * @brief 初始化资源
     * @return 初始化是否成功
     */
    virtual bool Initialize() = 0;
    
    /**
     * @brief 销毁资源
     */
    virtual void Destroy() {
        if (state_ != ResourceState::Destroyed && handle_ != handles::INVALID_RESOURCE) {
            if (mappedData_) {
                Unmap();
            }
            destroyImpl();
            handle_ = handles::INVALID_RESOURCE;
            state_ = ResourceState::Destroyed;
        }
    }
    
    /**
     * @brief 映射资源到CPU内存
     * @param offset 映射偏移量（字节）
     * @param size 映射大小（字节）
     * @return 映射的数据指针，失败返回nullptr
     */
    virtual void* Map(u64 offset = 0, u64 size = 0) {
        if (!CanMap()) return nullptr;
        if (mappedData_) return mappedData_;
        
        mappedData_ = mapImpl(offset, size);
        if (mappedData_) {
            state_ = ResourceState::InUse;
        }
        return mappedData_;
    }
    
    /**
     * @brief 取消映射
     */
    virtual void Unmap() {
        if (mappedData_) {
            unmapImpl();
            mappedData_ = nullptr;
            state_ = ResourceState::Ready;
        }
    }
    
    /**
     * @brief 更新资源数据
     * @param data 数据指针
     * @param size 数据大小（字节）
     * @param offset 写入偏移量（字节）
     * @return 更新是否成功
     */
    virtual bool UpdateData(const void* data, u64 size, u64 offset = 0) {
        if (!data || size == 0 || !CanUpdate()) return false;
        return updateDataImpl(data, size, offset);
    }
    
    /**
     * @brief 获取资源描述符
     * @return 资源描述符引用
     */
    const ResourceDesc& GetDesc() const { return desc_; }

    /**
     * @brief 获取资源大小
     * @return 资源大小（字节）
     */
    u64 GetSize() const { return desc_.size; }
    
    /**
     * @brief 资源句柄
     * @return 资源句柄
     */
    ResourceHandle GetHandle() const { return handle_; }
    
    /**
     * @brief 资源状态
     * @return 当前资源状态
     */
    ResourceState GetState() const { return state_; }
    
    /**
     * @brief 资源类型
     * @return 资源类型
     */
    ResourceType GetType() const { return desc_.type; }
    
    /**
     * @brief 资源用途
     * @return 资源用途标志
     */
    ResourceUsage GetUsage() const { return desc_.usage; }
    
    /**
     * @brief 获取资源名称
     * @return 资源名称
     */
    const char* GetName() const { return desc_.name ? desc_.name : "Unnamed"; }
    
    /**
     * @brief 检查资源是否有效
     * @return 资源是否有效
     */
    bool IsValid() const {
        return state_ != ResourceState::Destroyed && 
               state_ != ResourceState::Unknown &&
               handle_ != handles::INVALID_RESOURCE;
    }
    
    /**
     * @brief 检查资源是否可以映射
     * @return 是否可以映射
     */
    bool CanMap() const {
        return IsValid() && 
               (desc_.memoryUsage == GPUMemoryUsage::Dynamic || 
                desc_.memoryUsage == GPUMemoryUsage::Staging ||
                desc_.memoryUsage == GPUMemoryUsage::Readback) &&
               state_ != ResourceState::InUse;
    }
    
    /**
     * @brief 检查资源是否可以更新
     * @return 是否可以更新
     */
    bool CanUpdate() const {
        return IsValid() && 
               (desc_.memoryUsage == GPUMemoryUsage::Dynamic || 
                desc_.memoryUsage == GPUMemoryUsage::Staging ||
                desc_.memoryUsage == GPUMemoryUsage::Static ||
                desc_.memoryUsage == GPUMemoryUsage::Immutable) &&
               state_ != ResourceState::InUse;
    }
    
    /**
     * @brief 检查资源是否正在使用
     * @return 是否正在使用
     */
    bool IsInUse() const {
        return state_ == ResourceState::InUse || refCount_ > 1;
    }
    
    /**
     * @brief 添加引用
     */
    void AddRef() {
        refCount_.fetch_add(1, std::memory_order_relaxed);
    }
    
    /**
     * @brief 释放引用
     * @return 当前引用计数
     */
    u32 Release() {
        u32 count = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete this;
        }
        return count;
    }
    
    /**
     * @brief 获取引用计数
     * @return 当前引用计数
     */
    u32 GetRefCount() const {
        return refCount_.load(std::memory_order_relaxed);
    }
    
protected:
    // === 派生类必须实现的虚函数 ===
    
    /**
     * @brief 销毁资源的派生类实现
     */
    virtual void destroyImpl() {}
    
    /**
     * @brief 映射资源的派生类实现
     * @param offset 映射偏移量
     * @param size 映射大小
     * @return 映射的数据指针
     */
    virtual void* mapImpl(u64 offset, u64 size) = 0;
    
    /**
     * @brief 取消映射的派生类实现
     */
    virtual void unmapImpl() = 0;
    
    /**
     * @brief 更新数据的派生类实现
     * @param data 数据指针
     * @param size 数据大小
     * @param offset 写入偏移量
     * @return 更新是否成功
     */
    virtual bool updateDataImpl(const void* data, u64 size, u64 offset) = 0;
    
    // === 受保护的成员变量 ===
    
    RHIDeviceBase& device_;              ///< 设备引用
    ResourceDesc desc_;               ///< 资源描述符
    ResourceHandle handle_;           ///< 资源句柄
    ResourceState state_;             ///< 资源状态
    std::atomic<u32> refCount_;  ///< 引用计数
    void* mappedData_;               ///< 映射的数据指针
    
    /**
     * @brief 设置资源句柄（由派生类调用）
     * @param handle 资源句柄
     */
    void SetHandle(ResourceHandle handle) {
        handle_ = handle;
    }
    
    /**
     * @brief 设置资源状态
     * @param state 新的资源状态
     */
    void SetState(ResourceState state) {
        state_ = state;
    }
};

/**
 * @brief 资源管理器
 * @details 管理所有资源的生命周期和统计信息
 */
class ResourceManager {
public:
    /**
     * @brief 获取单例实例
     * @return 资源管理器单例引用
     */
    static ResourceManager& Instance() {
        static ResourceManager instance;
        return instance;
    }
    
    /**
     * @brief 注册资源
     * @param resource 资源指针
     */
    void RegisterResource(RHIResource* resource) {
        if (resource) {
            std::lock_guard<std::mutex> lock(mutex_);
            resources_[resource->GetHandle()] = resource;
            updateStats();
        }
    }
    
    /**
     * @brief 注销资源
     * @param handle 资源句柄
     */
    void UnregisterResource(ResourceHandle handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        resources_.erase(handle);
        updateStats();
    }
    
    /**
     * @brief 获取资源
     * @param handle 资源句柄
     * @return 资源指针，失败返回nullptr
     */
    RHIResource* GetResource(ResourceHandle handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = resources_.find(handle);
        return (it != resources_.end()) ? it->second : nullptr;
    }
    
    /**
     * @brief 获取资源统计信息
     * @return 资源统计信息
     */
    ResourceStats GetStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }
    
    /**
     * @brief 销毁所有资源
     */
    void DestroyAllResources() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [handle, resource] : resources_) {
            if (resource) {
                resource->Destroy();
                delete resource;
            }
        }
        resources_.clear();
        stats_ = ResourceStats();
    }
    
private:
    ResourceManager() = default;
    ~ResourceManager() = default;
    
    /**
     * @brief 更新统计信息
     */
    void updateStats() {
        stats_ = ResourceStats();
        
        for (const auto& [handle, resource] : resources_) {
            if (!resource) continue;
            
            switch (resource->GetType()) {
                case ResourceType::Buffer:
                    stats_.bufferCount++;
                    stats_.bufferMemoryUsage += resource->GetSize();
                    break;
                case ResourceType::Texture:
                case ResourceType::RenderTarget:
                case ResourceType::DepthStencil:
                    stats_.textureCount++;
                    stats_.textureMemoryUsage += resource->GetSize();
                    break;
                case ResourceType::Pipeline:
                    stats_.pipelineCount++;
                    break;
                default:
                    break;
            }
            
            stats_.totalMemoryUsage += resource->GetSize();
        }
    }
    
    mutable std::mutex mutex_;
    std::unordered_map<ResourceHandle, RHIResource*> resources_;
    ResourceStats stats_;
};

} // namespace primal::graphics::rhi