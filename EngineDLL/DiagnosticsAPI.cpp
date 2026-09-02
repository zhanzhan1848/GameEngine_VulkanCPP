// =============================================================================
// DiagnosticsAPI.cpp — 日志 L0/L2 实现（Docs/CLI_Agent_Interface_Plan.md §11.2）
//
// 设计要点：
//   · ConsoleSink 在发射线程同步写 stderr——与核心层尚存的裸 cout/cerr 保持
//     交错顺序（轨道 C 迁移完成前的过渡语义）；
//   · RingBufferSink / 崩溃快照在发射线程同步更新（崩溃路径只依赖这两者，
//     async-signal-safe 的 CrashHandler 不取锁直接读已发布快照）；
//   · FileSink / 宿主回调（InstallLogCallback）/ 事件表（on_log）经后台 flush
//     线程异步送达（§11.2 的后台 flush 语义；v1 用互斥队列，MPSC lock-free
//     是无接口差异的后续升级）；
//   · FlushLogQueue() 供宿主在退出前/断言前排空（selftest 亦用）。
// =============================================================================

#include "DiagnosticsAPI.h"

#include "CoreDiag.h"
#include "EngineEventsAPI.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#else
#include <io.h>
#include <windows.h>
#endif

namespace primal::engine_dll::diag {

namespace {

constexpr std::uint32_t kLevelTrace = primal::engine_dll::diag::kLevelTrace;
constexpr std::uint32_t kLevelDebug = primal::engine_dll::diag::kLevelDebug;
constexpr std::uint32_t kLevelInfo  = primal::engine_dll::diag::kLevelInfo;
constexpr std::uint32_t kLevelWarn  = primal::engine_dll::diag::kLevelWarn;
constexpr std::uint32_t kLevelError = primal::engine_dll::diag::kLevelError;
constexpr std::uint32_t kLevelFatal = primal::engine_dll::diag::kLevelFatal;

const char* LevelName(std::uint32_t level)
{
    switch (level)
    {
    case kLevelTrace: return "TRACE";
    case kLevelDebug: return "DEBUG";
    case kLevelInfo:  return "INFO";
    case kLevelWarn:  return "WARN";
    case kLevelError: return "ERROR";
    default:          return "FATAL";
    }
}

// --- 黑匣子：预分配环形缓冲，覆盖最旧 ---
struct RingEntry
{
    std::uint32_t level = 0;
    std::string timestamp;   // "HH:MM:SS.mmm"（发射时刻，本地时钟）
    std::string category;
    std::string message;
};

constexpr std::size_t kRingCapacity = 2048;
constexpr std::size_t kFormatBufferSize = 1024;
constexpr std::size_t kSnapshotBytes = 64 * 1024;   // 崩溃快照双缓冲（含 4 字节长度头）

// 全部可变状态收进单一持有者（Meyers 单例）：Dispatch 可能在 EngineDLL 自身
// 静态初始化期间（DllMain constructor 里的首条迁移日志）被调用，跨 TU 的
// 命名空间级静态初始化顺序不可依赖——函数内 static 保证首次使用即完整构造。
struct DiagState
{
    std::mutex mutex;
    std::uint32_t level = kLevelInfo;               // 默认 INFO 及以上可见
    std::vector<std::string> categories;            // 空 = 全部类别
    primal_log_callback callback = nullptr;         // CallbackSink（宿主可插拔）
    void* userdata = nullptr;
    std::vector<RingEntry> ring{kRingCapacity};     // 预分配，运行期不重配
    std::size_t ringHead = 0;                       // 下一个写入位
    std::size_t ringCount = 0;                      // 有效条数（≤ capacity）

    // FileSink（L2）：统一日志文件 + 按尺寸轮转
    FILE* file = nullptr;
    std::string filePath;
    std::uint32_t fileMaxBytes = 8u * 1024u * 1024u; // 默认 8 MiB/文件
    std::uint32_t fileMaxFiles = 3;                  // .log → .1 → .2 → .3
    std::uint32_t fileWritten = 0;
    std::mutex fileMutex;   // ProcessEntry 可能同时来自 worker 与 FlushLogQueue

    // 崩溃快照双缓冲：发射线程锁内序列化后原子发布，CrashHandler 无锁读取
    char snapshotBufs[2][kSnapshotBytes];
    std::atomic<const char*> snapshotPub{snapshotBufs[0]};
    int snapshotActive = 0;                          // 正在填充的槽（仅锁内访问）

    // 帧计数（L2 结构化字段）：RenderFrame 导出每帧推进
    std::atomic<std::uint64_t> frameCounter{0};

    // 后台 flush 队列：FileSink / 宿主回调 / 事件表在 worker 线程送达
    struct QueueEntry
    {
        std::uint32_t level;
        std::string category;
        std::string message;
    };
    std::deque<QueueEntry> queue;
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::thread worker;
    std::atomic<bool> workerRunning{false};
    std::atomic<bool> workerStop{false};

    // 崩溃转储路径前缀："目录/primal-crash-"（pid 在信号处理时追加）
    char crashPathPrefix[1024] = {0};

    // 无析构函数：State() 故意泄漏（见 State() 注释），worker 随进程退出回收。
};

DiagState& State()
{
    // 故意泄漏：日志子系统可能在任意静态析构阶段（甚至 DllMain 卸载析构里）
    // 被调用，销毁顺序不可控——mutex/队列必须常驻到进程结束。worker 线程
    // 随进程退出回收（不 join 的 thread 对象也在泄漏块内，不会触发 terminate）。
    static DiagState* state = new DiagState();
    return *state;
}

bool CategoryAllowed(const std::string& category)
{
    if (State().categories.empty()) return true; // 空名单 = 不过滤
    for (const auto& allowed : State().categories)
    {
        if (category == allowed) return true;
    }
    return false; // 名单非空时未命中即拒绝
}

std::string FormatTimestamp()
{
    // 与 RHIDebug.cpp 的 GetTimestamp 同一惯例：标准 std::localtime（调用点
    // 全部在 State().mutex 临界区内，不存在并发 gmtime/localtime 静态缓冲竞争）。
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count() % 1000;
    std::tm local{};
    local = *std::localtime(&seconds);
    char text[16];
    std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03d",
                  local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(millis));
    return std::string(text);
}

// 在 State().mutex 临界区内调用：把环形缓冲序列化进崩溃快照槽并原子发布。
// 快照布局：[u32 len][text...]，len 含长度头自身——CrashHandler 只需一次
// write(已发布指针, len)，全程不取锁、不分配。
void PublishCrashSnapshot()
{
    char* buf = State().snapshotBufs[State().snapshotActive];
    std::size_t used = 0;
    auto append = [&](const char* s, std::size_t n)
    {
        if (used + n > kSnapshotBytes - 1) n = (kSnapshotBytes - 1 > used) ? (kSnapshotBytes - 1 - used) : 0;
        if (n) { std::memcpy(buf + 4 + used, s, n); used += n; }
    };
    for (std::size_t i = 0; i < State().ringCount; ++i)
    {
        const std::size_t index = (State().ringHead + kRingCapacity - State().ringCount + i) % kRingCapacity;
        const RingEntry& entry = State().ring[index];
        append("[", 1);
        append(entry.timestamp.c_str(), entry.timestamp.size());
        append("][", 2);
        append(LevelName(entry.level), std::strlen(LevelName(entry.level)));
        append("][", 2);
        append(entry.category.c_str(), entry.category.size());
        append("] ", 2);
        append(entry.message.c_str(), entry.message.size());
        append("\n", 1);
        if (used >= kSnapshotBytes - 256) break; // 快照满，保留最新已写入部分
    }
    const std::uint32_t total = static_cast<std::uint32_t>(used + 4);
    std::memcpy(buf, &total, 4);
    State().snapshotPub.store(buf, std::memory_order_release);
    State().snapshotActive = 1 - State().snapshotActive; // 下一轮填充另一槽（发布中的槽保持稳定）
}

void ProcessEntry(const DiagState::QueueEntry& entry)
{
    // FileSink + 轮转（L2）。State().fileMutex 串行化 worker 与 FlushLogQueue 两个入口。
    {
        std::lock_guard<std::mutex> flock(State().fileMutex);
        if (State().file != nullptr)
        {
            std::string line = "[";
            line += LevelName(entry.level);
            line += "][";
            line += entry.category;
            line += "] ";
            line += entry.message;
            line += '\n';
            std::fwrite(line.data(), 1, line.size(), State().file);
            State().fileWritten += static_cast<std::uint32_t>(line.size());
            if (State().fileMaxBytes > 0 && State().fileWritten >= State().fileMaxBytes)
            {
                std::fclose(State().file);
                State().file = nullptr;
                // .log → .1 → .2 → .maxFiles（最旧被覆盖）
                for (std::uint32_t i = State().fileMaxFiles; i >= 1; --i)
                {
                    const std::string to = State().filePath + "." + std::to_string(i);
                    if (i == State().fileMaxFiles) std::remove(to.c_str());
                    const std::string from = (i == 1) ? State().filePath
                                                      : State().filePath + "." + std::to_string(i - 1);
                    std::rename(from.c_str(), to.c_str());
                }
            }
            if (State().file == nullptr)
            {
                State().file = std::fopen(State().filePath.c_str(), "a");
                State().fileWritten = 0;
            }
        }
    }

    // CallbackSink → 宿主（后台线程；§11.2 异步送达语义）。
    if (State().callback != nullptr)
    {
        State().callback(State().userdata, entry.level, entry.category.c_str(), entry.message.c_str());
    }

    // 事件表（§12）on_log：与回调同批送达（带帧号）。
    primal::engine_dll::events::FireLog(entry.level, entry.category.c_str(), entry.message.c_str(),
                                        State().frameCounter.load(std::memory_order_relaxed));

    // 设备丢失识别 → on_device_lost（Dawn 探测点在轨道 B 重定向；Vulkan 站点
    // 见 VulkanQuery.cpp；Metal 在 macOS 无 device-lost 语义，CB 失败已是 ERROR 日志）。
    if (entry.category == "RHI")
    {
        static constexpr const char* kMarkers[][2] = {
            { "[DawnDevice] Device lost", "Dawn" },
            { "[VulkanQueryPool] DEVICE_LOST", "Vulkan" },
            { "[VulkanDevice] Device lost", "Vulkan" },
        };
        for (const auto* marker : kMarkers)
        {
            const std::size_t markerLen = std::strlen(marker[0]);
            if (entry.message.compare(0, markerLen, marker[0]) == 0)
            {
                primal::engine_dll::events::FireDeviceLost(marker[1], entry.message.c_str());
                break;
            }
        }
    }
}

void WorkerLoop()
{
    for (;;)
    {
        std::deque<DiagState::QueueEntry> batch;
        {
            std::unique_lock<std::mutex> lock(State().queueMutex);
            State().queueCv.wait(lock, [] { return State().workerStop.load() || !State().queue.empty(); });
            batch.swap(State().queue);
            if (State().queue.empty() && State().workerStop.load()) return;
        }
        for (const auto& entry : batch) ProcessEntry(entry);
    }
}

void EnsureWorker()
{
    bool expected = false;
    if (State().workerRunning.compare_exchange_strong(expected, true))
    {
        State().worker = std::thread(WorkerLoop);
    }
}

} // anonymous namespace

// 非变参分发核心：Emit 与核心层插槽（primal::diag::SetEmitter 注入的正是它）共用。
// 签名与 primal::diag::EmitFn 严格一致；宿主参数恒为 null（分发器无状态）。
void Dispatch(void* host, std::uint32_t level, const char* category, const char* message)
{
    (void)host;
    {
        std::lock_guard<std::mutex> lock(State().mutex);
        if (level < State().level) return;
        const std::string categoryText(category);

        // ConsoleSink → stderr，发射线程同步写（与裸流交错顺序的过渡语义）。
        std::fprintf(stderr, "[PRIMAL:%s][%s] %s\n", LevelName(level), category, message);

        // RingBufferSink → 常驻黑匣子 + 崩溃快照发布（CrashHandler 无锁读取）。
        RingEntry& entry = State().ring[State().ringHead];
        entry.level = level;
        entry.timestamp = FormatTimestamp();
        entry.category = categoryText;
        entry.message = message;
        State().ringHead = (State().ringHead + 1) % kRingCapacity;
        if (State().ringCount < kRingCapacity) ++State().ringCount;
        PublishCrashSnapshot();

        if (!CategoryAllowed(categoryText)) return;

        // 入队给 worker：FileSink / 宿主回调 / 事件表异步送达。停机后回退同步。
        if (State().workerRunning.load())
        {
            std::lock_guard<std::mutex> qlock(State().queueMutex);
            State().queue.push_back(DiagState::QueueEntry{level, categoryText, message});
        }
        else
        {
            ProcessEntry(DiagState::QueueEntry{level, categoryText, message});
        }
    }
    State().queueCv.notify_all();
    EnsureWorker(); // 首条日志懒启动 worker（锁外）
}

void Emit(std::uint32_t level, const char* category, const char* fmt, ...)
{
    if (category == nullptr) category = "Engine";
    if (fmt == nullptr) return;

    char message[kFormatBufferSize];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    Dispatch(nullptr, level, category, message);
}

void RegisterCoreEmitter()
{
    // 核心 → EngineDLL 的依赖倒置接点（CLI 规划 §11.2）：核心只调插槽，
    // 这里把本模块的非变参分发器挂上去。userdata 恒 null（分发器无状态）。
    primal::diag::SetEmitter(&Dispatch, nullptr);
}

void AdvanceFrameCounter()
{
    State().frameCounter.fetch_add(1, std::memory_order_relaxed);
}

const void* GetCrashSnapshot(std::uint32_t* outLen)
{
    const char* pub = State().snapshotPub.load(std::memory_order_acquire);
    if (outLen != nullptr)
    {
        std::uint32_t len = 0;
        std::memcpy(&len, pub, 4); // 长度头在发布时已写定，读取原子可见
        *outLen = len;
    }
    return pub;
}

const char* GetCrashPathPrefix()
{
    return State().crashPathPrefix;
}

void PrepareCrashDumpPath()
{
    std::string dir;
    if (!State().filePath.empty())
    {
        const auto pos = State().filePath.find_last_of('/');
        dir = (pos == std::string::npos) ? std::string(".") : State().filePath.substr(0, pos);
    }
    else
    {
#if defined(_WIN32)
        const char* temp = std::getenv("TEMP");
        dir = (temp != nullptr) ? temp : ".";
#else
        dir = "/tmp";
#endif
    }
    std::snprintf(State().crashPathPrefix, sizeof(State().crashPathPrefix), "%s/primal-crash-", dir.c_str());
}

// 仅 async-signal-safe 原语：open/write/close + 手工 int/hex→ascii。
// 不取锁、不分配、不调 printf 家族。
void WriteCrashDumpMin(const char* pathPrefix, int signal, const void* faultAddr)
{
    // pid → ascii（getpid 为 POSIX 列明的 async-signal-safe 纯函数）。
    char path[1152];
    std::size_t n = 0;
    for (; pathPrefix[n] != '\0' && n < sizeof(path) - 24; ++n) path[n] = pathPrefix[n];
#if !defined(_WIN32)
    long pidValue = static_cast<long>(getpid());
#else
    long pidValue = static_cast<long>(GetCurrentProcessId());
#endif
    char digits[12];
    int d = 0;
    if (pidValue <= 0) { digits[d++] = '0'; pidValue = 0; }
    while (pidValue > 0 && d < 12) { digits[d++] = static_cast<char>('0' + pidValue % 10); pidValue /= 10; }
    for (int i = d - 1; i >= 0; --i) path[n++] = digits[i];

    // 头部："signal=<dec> addr=<hex>\n"（手工格式化，严格信号安全）。
    char header[64];
    std::size_t h = 0;
    const char* lead = "signal=";
    for (const char* p = lead; *p != '\0' && h < sizeof(header) - 1; ++p) header[h++] = *p;
    if (signal <= 0) { header[h++] = '0'; }
    {
        char dec[12];
        int k = 0;
        int v = signal;
        if (v <= 0) v = 0;
        while (v > 0 && k < 12) { dec[k++] = static_cast<char>('0' + v % 10); v /= 10; }
        for (int i = k - 1; i >= 0 && h < sizeof(header) - 1; --i) header[h++] = dec[i];
    }
    const char* mid = " addr=0x";
    for (const char* p = mid; *p != '\0' && h < sizeof(header) - 1; ++p) header[h++] = *p;
    {
        const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(faultAddr);
        char hex[16];
        int k = 0;
        if (addr == 0) hex[k++] = '0';
        std::uintptr_t v = addr;
        while (v > 0 && k < 16) { hex[k++] = "0123456789abcdef"[v & 0xF]; v >>= 4; }
        for (int i = k - 1; i >= 0 && h < sizeof(header) - 1; --i) header[h++] = hex[i];
    }
    header[h++] = '\n';

#if defined(_WIN32)
    const int fd = _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
#else
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
    if (fd >= 0)
    {
#if defined(_WIN32)
        _write(fd, header, static_cast<unsigned>(h));
#else
        ssize_t ignored = write(fd, header, h);
        (void)ignored;
#endif
        std::uint32_t len = 0;
        const char* snap = static_cast<const char*>(GetCrashSnapshot(&len));
        if (snap != nullptr && len > 4 && len <= kSnapshotBytes)
        {
#if defined(_WIN32)
            _write(fd, snap + 4, len - 4);
#else
            ssize_t ignored2 = write(fd, snap + 4, len - 4);
            (void)ignored2;
#endif
        }
#if defined(_WIN32)
        _close(fd);
#else
        close(fd);
#endif
    }

    // 宿主 on_crash（§12.3：宿主承诺 async-signal-safe）。黑匣子直指已发布快照。
    std::uint32_t len = 0;
    const char* snap = static_cast<const char*>(GetCrashSnapshot(&len));
    primal_crash_info info{};
    info.reason = "fatal signal";
    info.blackbox = (snap != nullptr && len > 4) ? snap + 4 : nullptr;
    info.blackbox_len = (snap != nullptr && len > 4) ? len - 4 : 0;
    primal::engine_dll::events::FireCrash(&info);
}

void FlushLogQueue()
{
    // 同步排空：宿主退出前 / 自测断言前调用。持队列锁逐条处理（worker 此刻
    // 也在竞争同一把锁，天然不重复）。
    std::lock_guard<std::mutex> qlock(State().queueMutex);
    while (!State().queue.empty())
    {
        DiagState::QueueEntry entry = std::move(State().queue.front());
        State().queue.pop_front();
        // ProcessEntry 内部会再取 State().mutex（file 状态），此处已不持 State().mutex。
        ProcessEntry(entry);
    }
}

} // namespace primal::engine_dll::diag

extern "C" {

EDITOR_INTERFACE std::uint32_t InstallLogCallback(primal_log_callback callback, void* userdata)
{
    std::lock_guard<std::mutex> lock(primal::engine_dll::diag::State().mutex);
    primal::engine_dll::diag::State().callback = callback;
    primal::engine_dll::diag::State().userdata = userdata;
    return 1;
}

EDITOR_INTERFACE void SetLogLevel(std::uint32_t level)
{
    std::lock_guard<std::mutex> lock(primal::engine_dll::diag::State().mutex);
    primal::engine_dll::diag::State().level = level;
}

EDITOR_INTERFACE void SetCategoryFilter(const char* commaSeparatedCategories)
{
    using namespace primal::engine_dll::diag;
    std::lock_guard<std::mutex> lock(State().mutex);
    State().categories.clear();
    if (commaSeparatedCategories == nullptr) return; // null = 不过滤
    const char* p = commaSeparatedCategories;
    while (*p != '\0')
    {
        while (*p == ' ' || *p == ',') ++p;
        if (*p == '\0') break;
        const char* start = p;
        while (*p != '\0' && *p != ',') ++p;
        State().categories.emplace_back(start, static_cast<std::size_t>(p - start));
    }
}

EDITOR_INTERFACE void SetLogFile(const char* path, std::uint32_t maxBytes, std::uint32_t maxFiles)
{
    using namespace primal::engine_dll::diag;
    {
        std::lock_guard<std::mutex> lock(State().mutex);
        if (State().file != nullptr)
        {
            std::fclose(State().file);
            State().file = nullptr;
        }
        if (path == nullptr || path[0] == '\0') return; // 关闭 FileSink
        State().filePath = path;
        State().fileMaxBytes = maxBytes != 0 ? maxBytes : 8u * 1024u * 1024u;
        State().fileMaxFiles = maxFiles != 0 ? maxFiles : 3;
        State().fileWritten = 0;
        State().file = std::fopen(State().filePath.c_str(), "a");
        if (State().file == nullptr)
            std::fprintf(stderr, "[PRIMAL:ERROR][Engine] SetLogFile 打开失败: %s\n", path);
    }
    PrepareCrashDumpPath(); // 崩溃转储跟随日志目录（无锁读 State().filePath，串行调用）
}

EDITOR_INTERFACE void FlushLogQueue()
{
    primal::engine_dll::diag::FlushLogQueue();
}

EDITOR_INTERFACE std::uint32_t CopyRecentLogs(char* buffer, std::uint32_t bufferSize)
{
    using namespace primal::engine_dll::diag;
    std::lock_guard<std::mutex> lock(State().mutex);

    const auto serialize = [](std::string& out)
    {
        out.clear();
        for (std::size_t i = 0; i < State().ringCount; ++i)
        {
            const std::size_t index = (State().ringHead + kRingCapacity - State().ringCount + i) % kRingCapacity;
            const RingEntry& entry = State().ring[index];
            out += "[";
            out += entry.timestamp;
            out += "][";
            out += LevelName(entry.level);
            out += "][";
            out += entry.category;
            out += "] ";
            out += entry.message;
            out += '\n';
        }
    };

    // 探询模式：null buffer → 返回所需字节数（含结尾 '\0'）。
    std::string text;
    if (buffer == nullptr || bufferSize == 0)
    {
        serialize(text);
        return static_cast<std::uint32_t>(text.size() + 1);
    }

    serialize(text);
    if (text.size() + 1 > bufferSize) return 0; // 尺寸不足，调用方应先用探询模式
    std::memcpy(buffer, text.c_str(), text.size() + 1);
    return static_cast<std::uint32_t>(text.size());
}

} // extern "C"
