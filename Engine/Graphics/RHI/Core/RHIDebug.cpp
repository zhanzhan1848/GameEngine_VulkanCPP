/**
 * @file RHIDebug.cpp
 * @brief RHI系统分级调试输出模块实现
 * @details 实现基于编译期宏的零运行时开销调试系统
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-30
 * @version 0.2.0
 */

#include "RHIDebug.h"
#include <cstdio>
#include <ctime>

namespace primal::graphics::rhi::debug {

// === 计数器定义 ===

namespace counters {
    std::atomic<uint64_t> ResourceAllocations(0);
    std::atomic<uint64_t> ResourceDeallocations(0);
    std::atomic<uint64_t> BufferCreations(0);
    std::atomic<uint64_t> TextureCreations(0);
    std::atomic<uint64_t> CommandBufferSubmissions(0);
    std::atomic<uint64_t> ShaderCompilations(0);
    std::atomic<uint64_t> PipelineCreations(0);
    std::atomic<uint64_t> MemoryAllocations(0);
    std::atomic<uint64_t> MemoryDeallocations(0);
}

// === 内部辅助函数 ===

/**
 * @brief 获取调试级别名称
 */
static const char* GetDebugLevelName(DebugLevel level) {
    switch (level) {
        case DebugLevel::TRACE: return "TRACE";
        case DebugLevel::DEBUG: return "DEBUG";
        case DebugLevel::INFO:  return "INFO ";
        case DebugLevel::WARN:  return "WARN ";
        case DebugLevel::ERROR: return "ERROR";
        case DebugLevel::FATAL: return "FATAL";
        default: return "UNKN ";
    }
}

/**
 * @brief 获取时间戳字符串
 */
static void GetTimestamp(char* buffer, size_t bufferSize) {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::strftime(buffer, bufferSize, "%H:%M:%S", &tm);
}

/**
 * @brief 提取文件名（去掉路径）
 */
static const char* ExtractFileName(const char* filePath) {
    if (filePath == nullptr) {
        return "unknown";
    }
    
    const char* fileName = filePath;
    while (*filePath != '\0') {
        if (*filePath == '/' || *filePath == '\\') {
            fileName = filePath + 1;
        }
        filePath++;
    }
    return fileName;
}

// === 调试输出函数实现 ===

void OutputDebugMessage(DebugLevel level, const char* file, uint32_t line, 
                       const char* function, const char* message) {
    if (message == nullptr) {
        return;
    }
    
    // 根据输出目标进行处理
    switch (RHI_DEBUG_TARGET) {
        case 1: // 系统日志
            // TODO: 实现系统日志输出
            break;
            
        case 2: // 文件输出
        case 3: // 控制台输出（也输出到文件）
        {
            char timestamp[32];
            GetTimestamp(timestamp, sizeof(timestamp));
            
            const char* fileName = ExtractFileName(file);
            const char* levelName = GetDebugLevelName(level);
            
            // 格式化输出
            // [时间] [级别] 文件名:行号 函数名: 消息
            char formattedMessage[1024];
            int written = snprintf(formattedMessage, sizeof(formattedMessage),
                                  "[%s] [%s] %s:%u %s: %s",
                                  timestamp, levelName, fileName, line, function, message);
            
            if (written > 0 && written < sizeof(formattedMessage)) {
                // 输出到控制台（如果是调试模式或目标为控制台）
                if (RHI_DEBUG_TARGET == 3) {
                    printf("%s\n", formattedMessage);
                    fflush(stdout);
                }
                
                // 输出到文件
                FILE* outputFile = fopen("rhi_debug.log", "a");
                if (outputFile != nullptr) {
                    fprintf(outputFile, "%s\n", formattedMessage);
                    fclose(outputFile);
                }
            }
            break;
        }
        
        case 0: // 无输出
        default:
            break;
    }
}

// === 计数器管理函数实现 ===

void ResetAllCounters() {
    counters::ResourceAllocations.store(0);
    counters::ResourceDeallocations.store(0);
    counters::BufferCreations.store(0);
    counters::TextureCreations.store(0);
    counters::CommandBufferSubmissions.store(0);
    counters::ShaderCompilations.store(0);
    counters::PipelineCreations.store(0);
    counters::MemoryAllocations.store(0);
    counters::MemoryDeallocations.store(0);
}

void GetCounterStats(char* buffer, size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0) {
        return;
    }
    
    int written = snprintf(buffer, bufferSize,
        "=== RHI计数器统计 ===\n"
        "资源分配: %llu\n"
        "资源释放: %llu\n"
        "缓冲区创建: %llu\n"
        "纹理创建: %llu\n"
        "命令缓冲区提交: %llu\n"
        "着色器编译: %llu\n"
        "管线创建: %llu\n"
        "内存分配: %llu\n"
        "内存释放: %llu\n"
        "==================",
        static_cast<unsigned long long>(counters::ResourceAllocations.load()),
        static_cast<unsigned long long>(counters::ResourceDeallocations.load()),
        static_cast<unsigned long long>(counters::BufferCreations.load()),
        static_cast<unsigned long long>(counters::TextureCreations.load()),
        static_cast<unsigned long long>(counters::CommandBufferSubmissions.load()),
        static_cast<unsigned long long>(counters::ShaderCompilations.load()),
        static_cast<unsigned long long>(counters::PipelineCreations.load()),
        static_cast<unsigned long long>(counters::MemoryAllocations.load()),
        static_cast<unsigned long long>(counters::MemoryDeallocations.load())
    );
    
    if (written < 0 || written >= static_cast<int>(bufferSize)) {
        // 缓冲区不足，截断输出
        buffer[bufferSize - 1] = '\0';
    }
}

// === 内存追踪函数实现 ===

struct MemoryAllocationRecord {
    void* ptr;
    size_t size;
    const char* type;
    std::chrono::high_resolution_clock::time_point timestamp;
};

static std::vector<MemoryAllocationRecord> g_memoryRecords;
static std::mutex g_memoryRecordsMutex;

void TrackMemoryAllocation(const char* type, size_t size, void* ptr) {
    if (type == nullptr || ptr == nullptr) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(g_memoryRecordsMutex);
    
    MemoryAllocationRecord record;
    record.ptr = ptr;
    record.size = size;
    record.type = type;
    record.timestamp = std::chrono::high_resolution_clock::now();
    
    g_memoryRecords.push_back(record);
    
    // 更新计数器
    RHI_COUNTER_INC(MemoryAllocations);
}

void TrackMemoryDeallocation(const char* type, void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(g_memoryRecordsMutex);
    
    // 查找并移除记录
    for (auto it = g_memoryRecords.begin(); it != g_memoryRecords.end(); ++it) {
        if (it->ptr == ptr) {
            g_memoryRecords.erase(it);
            break;
        }
    }
    
    // 更新计数器
    RHI_COUNTER_INC(MemoryDeallocations);
}

void GetMemoryStats(char* buffer, size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(g_memoryRecordsMutex);
    
    size_t totalAllocated = 0;
    size_t allocationCount = g_memoryRecords.size();
    
    for (const auto& record : g_memoryRecords) {
        totalAllocated += record.size;
    }
    
    int written = snprintf(buffer, bufferSize,
        "=== RHI内存统计 ===\n"
        "当前分配块数: %zu\n"
        "当前分配总大小: %zu bytes (%.2f MB)\n"
        "平均分配大小: %.2f KB\n"
        "==================",
        allocationCount,
        totalAllocated,
        static_cast<double>(totalAllocated) / (1024.0 * 1024.0),
        allocationCount > 0 ? static_cast<double>(totalAllocated) / (allocationCount * 1024.0) : 0.0
    );
    
    if (written < 0 || written >= static_cast<int>(bufferSize)) {
        // 缓冲区不足，截断输出
        buffer[bufferSize - 1] = '\0';
    }
}

// === 快照和诊断函数实现 ===

void CreateSystemSnapshot() {
    char countersBuffer[1024];
    char memoryBuffer[1024];
    
    GetCounterStats(countersBuffer, sizeof(countersBuffer));
    GetMemoryStats(memoryBuffer, sizeof(memoryBuffer));
    
    RHI_DEBUG("=== RHI系统快照 ===");
    RHI_DEBUG("%s", countersBuffer);
    RHI_DEBUG("%s", memoryBuffer);
    RHI_DEBUG("==================");
}

void DumpDiagnostics() {
    // 输出详细的诊断信息
    CreateSystemSnapshot();
    
    // 可以添加更多诊断信息
    RHI_DEBUG("=== 诊断信息转储完成 ===");
}

} // namespace primal::graphics::rhi::debug