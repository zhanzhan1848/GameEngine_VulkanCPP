/**
 * @file RHIDevice.cpp
 * @brief RHI设备基类实现
 * @details RHI设备基类的非模板方法实现
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-29
 * @version 0.1.0
 */

#include "RHIDevice.h"
#include <cassert>
#include <algorithm>
#include <unordered_map>
#include <mutex>

namespace primal::graphics::rhi {

// === 全局设备管理器 ===

/**
 * @brief 全局设备管理器
 * @details 管理所有RHI设备的生命周期
 */
class DeviceManager {
public:
    /**
     * @brief 获取单例实例
     * @return 设备管理器单例引用
     */
    static DeviceManager& Instance() {
        static DeviceManager instance;
        return instance;
    }
    
    /**
     * @brief 注册设备
     * @param device 设备指针
     * @return 设备ID
     */
    u32 RegisterDevice(RHIDeviceBase* device) {
        u32 id = nextDeviceId_++;
        devices_.emplace_back(id, device);
        return id;
    }
    
    /**
     * @brief 注销设备
     * @param deviceId 设备ID
     */
    void UnregisterDevice(u32 deviceId) {
        for (u64 i = 0; i < devices_.size(); ++i) {
            if (devices_[i].first == deviceId) {
                devices_.erase(i);
                break;
            }
        }
    }
    
    /**
     * @brief 获取设备数量
     * @return 设备数量
     */
    size_t GetDeviceCount() const {
        return devices_.size();
    }
    
    /**
     * @brief 获取所有设备ID
     * @param ids 输出数组，用于存储设备ID
     * @param maxIds 数组最大容量
     * @return 实际写入的ID数量
     */
    size_t GetAllDeviceIds(u32* ids, size_t maxIds) const {
        size_t count = 0;
        for (const auto& [id, device] : devices_) {
            if (count < maxIds) {
                ids[count++] = id;
            } else {
                break;
            }
        }
        return count;
    }
    
    /**
     * @brief 等待所有设备空闲
     */
    void WaitAllDevicesIdle() const {
        for (const auto& [id, device] : devices_) {
            if (device && device->IsValid()) {
                device->WaitIdle();
            }
        }
    }
    
    /**
     * @brief 关闭所有设备
     */
    void ShutdownAllDevices() {
        for (const auto& [id, device] : devices_) {
            if (device) {
                device->Shutdown();
            }
        }
        devices_.clear();
    }
    
private:
    DeviceManager() = default;
    ~DeviceManager() = default;
    
    utl::vector<std::pair<u32, RHIDeviceBase*>> devices_;
    u32 nextDeviceId_{1};
};

// === 设备工厂方法 ===

/**
 * @brief 设备工厂类
 * @details 根据平台创建对应的设备实例
 */
class DeviceFactory {
public:
    /**
     * @brief 创建设备
     * @param desc 设备描述符
     * @return 设备句柄，失败返回nullptr
     */
    template<typename DeviceType>
    static DeviceType* CreateDevice(const DeviceDesc& desc) {
        DeviceType* device = new DeviceType(desc);
        if (device && device->Initialize()) {
            return device;
        } else {
            delete device;
            return nullptr;
        }
    }
    
    /**
     * @brief 销毁设备
     * @param device 设备指针
     */
    template<typename DeviceType>
    static void DestroyDevice(DeviceType* device) {
        if (device) {
            device->Shutdown();
            delete device;
        }
    }
};

// === 设备信息查询工具 ===

/**
 * @brief 查询所有可用的适配器信息
 * @param platform 目标平台
 * @param adapters 输出数组，用于存储适配器信息
 * @param maxAdapters 数组最大容量
 * @return 实际查询到的适配器数量
 */
size_t QueryAvailableAdapters(RHIPlatform platform, DeviceInfo* adapters, size_t maxAdapters) {
    // 这里应该根据不同平台实现具体的适配器查询逻辑
    // 目前返回0，表示需要在派生类中实现
    (void)platform;
    (void)adapters;
    (void)maxAdapters;
    return 0;
}

/**
 * @brief 检查平台是否受支持
 * @param platform 要检查的平台
 * @return 平台是否受支持
 */
bool IsPlatformSupported(RHIPlatform platform) {
    switch (platform) {
        case RHIPlatform::D3D12:
#if defined(_WIN64)
            return true;
#else
            return false;
#endif
            
        case RHIPlatform::Vulkan:
            return true; // Vulkan跨平台支持
            
        case RHIPlatform::Metal:
#if defined(__APPLE__)
            return true;
#else
            return false;
#endif
            
        case RHIPlatform::Dawn:
            return true; // Dawn跨平台支持
            
        default:
            return false;
    }
}

/**
 * @brief 获取推荐的设备描述符
 * @param platform 目标平台
 * @return 推荐的设备描述符
 */
DeviceDesc GetRecommendedDeviceDesc(RHIPlatform platform) {
    DeviceDesc desc;
    desc.platform = platform;
    desc.enableDebug = 
#ifdef _DEBUG
        true;
#else
        false;
#endif
    desc.enableValidation = desc.enableDebug;
    desc.adapterIndex = 0;
    desc.maxFramesInFlight = 3; // 默认三重缓冲
    
    return desc;
}

// === 性能计数器 ===

/**
 * @brief 设备性能计数器
 * @details 用于监控设备性能统计信息
 */
struct DevicePerformanceCounters {
    u64 frameCount;               ///< 总帧数
    u64 drawCallCount;            ///< 绘制调用次数
    u64 computeDispatchCount;     ///< 计算分派次数
    u64 bufferCreations;          ///< 缓冲区创建次数
    u64 textureCreations;         ///< 纹理创建次数
    u64 pipelineCreations;        ///< 管线创建次数
    float averageFrameTime;            ///< 平均帧时间（毫秒）
    float averageGPUTime;              ///< 平均GPU时间（毫秒）
    u64 memoryUsage;              ///< 当前内存使用量（字节）
    u64 peakMemoryUsage;          ///< 峰值内存使用量（字节）
    
    DevicePerformanceCounters() 
        : frameCount(0), drawCallCount(0), computeDispatchCount(0),
          bufferCreations(0), textureCreations(0), pipelineCreations(0),
          averageFrameTime(0.0f), averageGPUTime(0.0f),
          memoryUsage(0), peakMemoryUsage(0) {}
};

/**
 * @brief 性能监控器
 */
class PerformanceMonitor {
public:
    /**
     * @brief 获取单例实例
     * @return 性能监控器单例引用
     */
    static PerformanceMonitor& Instance() {
        static PerformanceMonitor instance;
        return instance;
    }
    
    /**
     * @brief 更新性能计数器
     * @param deviceId 设备ID
     * @param counters 性能计数器
     */
    void UpdateCounters(u32 deviceId, const DevicePerformanceCounters& counters) {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_[deviceId] = counters;
    }
    
    /**
     * @brief 获取设备的性能计数器
     * @param deviceId 设备ID
     * @return 性能计数器的常量指针，如果设备不存在返回nullptr
     */
    const DevicePerformanceCounters* GetCounters(u32 deviceId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = counters_.find(deviceId);
        return (it != counters_.end()) ? &it->second : nullptr;
    }
    
    /**
     * @brief 清除所有计数器
     */
    void ClearAllCounters() {
        std::lock_guard<std::mutex> lock(mutex_);
        counters_.clear();
    }
    
private:
    PerformanceMonitor() = default;
    ~PerformanceMonitor() = default;
    
    mutable std::mutex mutex_;
    std::unordered_map<u32, DevicePerformanceCounters> counters_;
};

// === 调试和日志 ===

/**
 * @brief 设备调试级别
 */
enum class DeviceDebugLevel : u8 {
    None = 0,        ///< 无调试信息
    Error = 1,       ///< 仅错误
    Warning = 2,     ///< 警告和错误
    Info = 3,        ///< 信息、警告和错误
    Verbose = 4      ///< 详细信息
};

/**
 * @brief 设置设备调试级别
 * @param level 调试级别
 */
void SetDeviceDebugLevel(DeviceDebugLevel level) {
    // 实现调试级别设置逻辑
    (void)level;
}

/**
 * @brief 输出设备调试消息
 * @param level 消息级别
 * @param deviceId 设备ID
 * @param message 消息内容
 */
void LogDeviceMessage(DeviceDebugLevel level, u32 deviceId, const char* message) {
    // 实现调试消息输出逻辑
    (void)level;
    (void)deviceId;
    (void)message;
}

/**
 * @brief 注册设备
 * @param device 设备指针
 * @return 设备ID
 */
u32 RHIDeviceManager::RegisterDevice(RHIDeviceBase* device) {
    u32 id = nextDeviceId_++;
    devices_.emplace_back(id, device);
    return id;
}

/**
 * @brief 注销设备
 * @param deviceId 设备ID
 */
void RHIDeviceManager::UnregisterDevice(u32 deviceId) {
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].first == deviceId) {
            devices_.erase(devices_.begin() + i);
            break;
        }
    }
}

/**
 * @brief 获取设备数量
 * @return 设备数量
 */
size_t RHIDeviceManager::GetDeviceCount() const {
    return devices_.size();
}

/**
 * @brief 获取设备
 * @param deviceId 设备ID
 * @return 设备指针
 */
RHIDeviceBase* RHIDeviceManager::GetDevice(u32 deviceId) const {
    for (const auto& [id, device] : devices_) {
        if (id == deviceId) {
            return device;
        }
    }
    return nullptr;
}

// 全局设备管理器实例定义
RHIDeviceManager g_deviceManager;

RHIDeviceBase* RHIDeviceManager::GetActiveDevice() const {
    if (devices_.empty()) return nullptr;
    return devices_.back().second;
}

} // namespace primal::graphics::rhi