/**
 * @file RHIDebug.h
 * @brief RHI系统分级调试输出模块
 * @details 基于编译期宏的零运行时开销调试系统，完全移除对string/iostream依赖
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-30
 * @version 0.2.0
 */

#pragma once

#include "CommonHeaders.h"
#include <mutex>
#include <vector>

namespace primal::graphics::rhi::debug {

// === 调试级别枚举 ===

/**
 * @brief RHI调试输出级别
 * @details TRACE为最低级别，ERROR为最高级别
 */
enum class DebugLevel : uint8_t {
    TRACE = 0,    ///< 跟踪级别：详细的执行流程信息
    DEBUG = 1,    ///< 调试级别：开发和调试信息
    INFO = 2,     ///< 信息级别：重要状态变更
    WARN = 3,     ///< 警告级别：潜在问题
    ERROR = 4,    ///< 错误级别：严重错误
    FATAL = 5     ///< 致命级别：系统崩溃
};

// === 编译期调试配置 ===

/**
 * @brief 当前编译的调试级别
 * @details 定义为0则完全禁用调试输出，定义为5则输出所有级别
 * @note 可通过编译选项 -DRHI_DEBUG_LEVEL=X 进行覆盖
 */
#ifndef RHI_DEBUG_LEVEL
#define RHI_DEBUG_LEVEL 0  // 默认为TRACE级别（最低级别）
#endif

/**
 * @brief 调试输出目标
 * @details 0=无输出，1=系统日志，2=文件，3=控制台（仅测试环境）
 */
#ifndef RHI_DEBUG_TARGET
#define RHI_DEBUG_TARGET 0  // 默认无输出（生产环境）
#endif

// === 调试宏定义 ===

/**
 * @brief 获取当前调试级别阈值
 */
#define RHI_GET_DEBUG_LEVEL() static_cast<uint8_t>(RHI_DEBUG_LEVEL)

/**
 * @brief 检查是否应该输出指定级别的调试信息
 */
#define RHI_SHOULD_LOG(level) (RHI_DEBUG_TARGET != 0 && RHI_GET_DEBUG_LEVEL() <= static_cast<uint8_t>(level))

/**
 * @brief 格式化输出缓冲区大小
 */
#define RHI_DEBUG_BUFFER_SIZE 512

/**
 * @brief 内部调试输出函数
 * @param level 调试级别
 * @param file 文件名（不含路径）
 * @param line 行号
 * @param function 函数名
 * @param message 消息内容
 */
extern void OutputDebugMessage(DebugLevel level, const char* file, uint32_t line, 
                              const char* function, const char* message);

/**
 * @brief 核心调试输出宏
 * @param level 调试级别
 * @param message 消息内容
 * @note 完全通过编译期宏控制，Release模式下无任何运行时开销
 */
#define RHI_DEBUG_LOG(level, message) \
    DEBUG_OP( \
        if (RHI_SHOULD_LOG(level)) { \
            OutputDebugMessage(level, __FILE__, __LINE__, __FUNCTION__, message); \
        } \
    )

/**
 * @brief TRACE级别调试输出宏
 * @param format 格式字符串（仅支持%s, %d, %u, %x, %f等基础格式）
 * @param ... 参数列表
 */
#define RHI_TRACE(fmt, ...) \
    DEBUG_OP( \
        if (RHI_SHOULD_LOG(DebugLevel::TRACE)) { \
            char buffer[RHI_DEBUG_BUFFER_SIZE]; \
            snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
            OutputDebugMessage(DebugLevel::TRACE, __FILE__, __LINE__, __FUNCTION__, buffer); \
        } \
    )

/**
 * @brief DEBUG级别调试输出宏
 */
#define RHI_DEBUG(fmt, ...) \
    DEBUG_OP( \
        if (RHI_SHOULD_LOG(DebugLevel::DEBUG)) { \
            char buffer[RHI_DEBUG_BUFFER_SIZE]; \
            snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
            OutputDebugMessage(DebugLevel::DEBUG, __FILE__, __LINE__, __FUNCTION__, buffer); \
        } \
    )

/**
 * @brief INFO级别调试输出宏
 */
#define RHI_INFO(fmt, ...) \
    DEBUG_OP( \
        if (RHI_SHOULD_LOG(DebugLevel::INFO)) { \
            char buffer[RHI_DEBUG_BUFFER_SIZE]; \
            snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
            OutputDebugMessage(DebugLevel::INFO, __FILE__, __LINE__, __FUNCTION__, buffer); \
        } \
    )

/**
 * @brief WARN级别调试输出宏
 */
#define RHI_WARN(fmt, ...) \
    DEBUG_OP( \
        if (RHI_SHOULD_LOG(DebugLevel::WARN)) { \
            char buffer[RHI_DEBUG_BUFFER_SIZE]; \
            snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
            OutputDebugMessage(DebugLevel::WARN, __FILE__, __LINE__, __FUNCTION__, buffer); \
        } \
    )

/**
 * @brief ERROR级别调试输出宏
 */
#define RHI_ERROR(fmt, ...) \
    DEBUG_OP( \
        if (RHI_SHOULD_LOG(DebugLevel::ERROR)) { \
            char buffer[RHI_DEBUG_BUFFER_SIZE]; \
            snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
            OutputDebugMessage(DebugLevel::ERROR, __FILE__, __LINE__, __FUNCTION__, buffer); \
        } \
    )

/**
 * @brief FATAL级别调试输出宏
 * @note 致命错误会终止程序执行
 */
#define RHI_FATAL(fmt, ...) \
    DEBUG_OP( \
        if (RHI_SHOULD_LOG(DebugLevel::FATAL)) { \
            char buffer[RHI_DEBUG_BUFFER_SIZE]; \
            snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
            OutputDebugMessage(DebugLevel::FATAL, __FILE__, __LINE__, __FUNCTION__, buffer); \
            assert(false && buffer); \
        } \
    )

// === 专用调试宏 ===

/**
 * @brief RHI句柄验证宏
 */
#define RHI_VALIDATE_HANDLE(handle, type) \
    DEBUG_OP( \
        if ((handle) == 0) { \
            RHI_ERROR("Invalid " #type " handle: 0"); \
            return false; \
        } \
    )

/**
 * @brief RHI空指针检查宏
 */
#define RHI_VALIDATE_PTR(ptr, name) \
    DEBUG_OP( \
        if ((ptr) == nullptr) { \
            RHI_ERROR("Null pointer: " #name); \
            return false; \
        } \
    )

/**
 * @brief RHI边界检查宏
 */
#define RHI_VALIDATE_RANGE(value, min, max, name) \
    DEBUG_OP( \
        if ((value) < (min) || (value) > (max)) { \
            RHI_ERROR("Range check failed for " #name ": %u (expected %u-%u)", \
                     static_cast<uint32_t>(value), static_cast<uint32_t>(min), static_cast<uint32_t>(max)); \
            return false; \
        } \
    )

/**
 * @brief RHI性能计时开始宏
 */
#define RHI_PERF_BEGIN(name) \
    DEBUG_OP( \
        std::chrono::high_resolution_clock::time_point _perf_begin_##name = std::chrono::high_resolution_clock::now(); \
    )

/**
 * @brief RHI性能计时结束宏
 */
#define RHI_PERF_END(name) \
    DEBUG_OP( \
        auto _perf_end_##name = std::chrono::high_resolution_clock::now(); \
        auto _perf_duration_##name = std::chrono::duration_cast<std::chrono::microseconds>(_perf_end_##name - _perf_begin_##name); \
        RHI_DEBUG("Performance [%s]: %lld microseconds", #name, _perf_duration_##name.count()); \
    )

// === 统计和计数器 ===

/**
 * @brief RHI调试计数器
 */
namespace counters {
    extern std::atomic<uint64_t> ResourceAllocations;
    extern std::atomic<uint64_t> ResourceDeallocations;
    extern std::atomic<uint64_t> BufferCreations;
    extern std::atomic<uint64_t> TextureCreations;
    extern std::atomic<uint64_t> CommandBufferSubmissions;
    extern std::atomic<uint64_t> ShaderCompilations;
    extern std::atomic<uint64_t> PipelineCreations;
    extern std::atomic<uint64_t> MemoryAllocations;
    extern std::atomic<uint64_t> MemoryDeallocations;
}

/**
 * @brief 计数器增加宏
 */
#define RHI_COUNTER_INC(counter) \
    DEBUG_OP( \
        counters::counter.fetch_add(1, std::memory_order_relaxed); \
    )

/**
 * @brief 计数器增加指定值宏
 */
#define RHI_COUNTER_ADD(counter, value) \
    DEBUG_OP( \
        counters::counter.fetch_add(value, std::memory_order_relaxed); \
    )

/**
 * @brief 重置所有计数器
 */
extern void ResetAllCounters();

/**
 * @brief 获取计数器统计信息
 * @param buffer 输出缓冲区
 * @param bufferSize 缓冲区大小
 */
extern void GetCounterStats(char* buffer, size_t bufferSize);

// === 内存追踪 ===

/**
 * @brief 内存分配追踪
 */
extern void TrackMemoryAllocation(const char* type, size_t size, void* ptr);

/**
 * @brief 内存释放追踪
 */
extern void TrackMemoryDeallocation(const char* type, void* ptr);

/**
 * @brief 获取内存使用统计
 * @param buffer 输出缓冲区
 * @param bufferSize 缓冲区大小
 */
extern void GetMemoryStats(char* buffer, size_t bufferSize);

// === 快照和诊断 ===

/**
 * @brief 创建RHI系统快照
 */
extern void CreateSystemSnapshot();

/**
 * @brief 输出诊断信息
 */
extern void DumpDiagnostics();

} // namespace primal::graphics::rhi::debug