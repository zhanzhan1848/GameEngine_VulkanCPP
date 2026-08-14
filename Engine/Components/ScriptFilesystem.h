#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace primal::script {

enum class RootKind { Data, Save, Log, Cache };

class ScriptFilesystem {
public:
    static ScriptFilesystem& instance();

    // Host application configures physical root paths (once, at init).
    void set_root(RootKind kind, const std::filesystem::path& path);
    std::filesystem::path root(RootKind kind) const;

    // Path resolution + validation. Returns nullopt on any validation failure.
    std::optional<std::filesystem::path> resolve(const std::string& vpath) const;

    // Synchronous operations.
    std::optional<std::string> read(const std::string& vpath, size_t max_bytes);
    bool write(const std::string& vpath, std::string_view content);
    bool append(const std::string& vpath, std::string_view content);
    bool exists(const std::string& vpath);
    std::optional<uint64_t> size(const std::string& vpath);
    std::vector<std::string> list(const std::string& vpath);
    bool remove(const std::string& vpath);

    // Async read — callback always fires on main thread.
    void read_async(const std::string& vpath, size_t max_bytes,
                    std::function<void(bool, std::string)> callback);

    // Lifecycle.
    void initialize();
    void shutdown();

private:
    struct RootEntry {
        std::filesystem::path path;
        bool writable    = false;
        bool append_only = false;
        bool removable   = false;
    };
    std::array<RootEntry, 4> roots_{};

    std::thread             worker_;
    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::function<void()>> work_queue_;
    std::atomic<bool>       stop_{false};

    void worker_loop();

    ScriptFilesystem();
    ~ScriptFilesystem();
    ScriptFilesystem(const ScriptFilesystem&) = delete;
    ScriptFilesystem& operator=(const ScriptFilesystem&) = delete;
};

ScriptFilesystem& filesystem();

}  // namespace primal::script
