#include "Components/ScriptFilesystem.h"
#include "Components/Script.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#if !defined(_WIN32)  // MSVC 没有 pthread.h；Windows 路径不设置线程名
#include <pthread.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace primal::script {

namespace {
constexpr size_t kDefaultMaxBytes    = 64 * 1024 * 1024;     // 64 MB
constexpr size_t kAbsoluteMaxBytes   = 256 * 1024 * 1024;    // 256 MB
constexpr size_t kMaxListEntries     = 10000;

// Generate random suffix for atomic temp file.
// macOS: arc4random. WASM: emscripten_get_now seeded linear congruential.
std::string random_suffix() {
    char buf[17];
#if defined(__APPLE__)
    // arc4random 是 BSD/macOS 专有（MSVC 无此符号）
    unsigned r1 = arc4random();
    unsigned r2 = arc4random();
#else
    // Emscripten/Windows: LCG（Emscripten 无强随机源；Windows 曾在此分支调用
    // arc4random 导致 C3861）
    static unsigned seed = static_cast<unsigned>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    seed = seed * 1103515245u + 12345u;
    unsigned r1 = seed;
    seed = seed * 1103515245u + 12345u;
    unsigned r2 = seed;
#endif
    std::snprintf(buf, sizeof(buf), "%08x%08x", r1, r2);
    return std::string(buf);
}

// Read up to max_bytes from path. Returns nullopt on any failure.
std::optional<std::string> read_file_with_limit(const std::filesystem::path& path, size_t max_bytes) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (ec) return std::nullopt;
    if (sz > max_bytes) return std::nullopt;

    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;

    std::string out;
    out.resize(static_cast<size_t>(sz));
    f.read(&out[0], sz);
    if (!f) return std::nullopt;
    return out;
}
}  // anonymous namespace

ScriptFilesystem::ScriptFilesystem() {
    roots_[static_cast<size_t>(RootKind::Data)].writable    = false;
    roots_[static_cast<size_t>(RootKind::Data)].append_only = false;
    roots_[static_cast<size_t>(RootKind::Data)].removable   = false;

    roots_[static_cast<size_t>(RootKind::Save)].writable    = true;
    roots_[static_cast<size_t>(RootKind::Save)].append_only = false;
    roots_[static_cast<size_t>(RootKind::Save)].removable   = true;

    roots_[static_cast<size_t>(RootKind::Log)].writable    = false;
    roots_[static_cast<size_t>(RootKind::Log)].append_only = true;
    roots_[static_cast<size_t>(RootKind::Log)].removable   = false;

    roots_[static_cast<size_t>(RootKind::Cache)].writable    = true;
    roots_[static_cast<size_t>(RootKind::Cache)].append_only = false;
    roots_[static_cast<size_t>(RootKind::Cache)].removable   = true;
}

ScriptFilesystem::~ScriptFilesystem() {
    shutdown();
}

ScriptFilesystem& ScriptFilesystem::instance() {
    static ScriptFilesystem inst;
    return inst;
}

ScriptFilesystem& filesystem() { return ScriptFilesystem::instance(); }

void ScriptFilesystem::set_root(RootKind kind, const std::filesystem::path& path) {
    // Host-only: call during init before any script runs. Roots_ is
    // read-only after init; no lock needed because no concurrent access.
    roots_[static_cast<size_t>(kind)].path = path;
    // Auto-create directory so write() doesn't fail on missing parent.
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    // Silently ignore errors — host may set read-only data:// root that doesn't
    // exist yet; that's fine, exists()/read() return false/nil appropriately.
}

std::filesystem::path ScriptFilesystem::root(RootKind kind) const {
    return roots_[static_cast<size_t>(kind)].path;
}

std::optional<std::filesystem::path> ScriptFilesystem::resolve(const std::string& vpath) const {
    namespace fs = std::filesystem;

    // 1. Find "://" separator. Reject if missing or at position 0.
    auto sep = vpath.find("://");
    if (sep == std::string::npos || sep == 0) return std::nullopt;

    std::string root_name = vpath.substr(0, sep);
    std::string rel_path  = vpath.substr(sep + 3);

    // 2. Map root name to RootKind.
    RootKind kind;
    if      (root_name == "data")  kind = RootKind::Data;
    else if (root_name == "save")  kind = RootKind::Save;
    else if (root_name == "log")   kind = RootKind::Log;
    else if (root_name == "cache") kind = RootKind::Cache;
    else return std::nullopt;

    // 3. rel_path must be non-empty.
    if (rel_path.empty()) return std::nullopt;

    // 4. Reject absolute paths (leading / or \).
    if (rel_path[0] == '/' || rel_path[0] == '\\') return std::nullopt;

    // 5. Reject Windows drive letters (e.g., "C:\..." or "C:/...").
    if (rel_path.size() >= 2 && rel_path[1] == ':') return std::nullopt;

    // 6 + 7. Iterate components; reject "..", skip empty.
    fs::path normalized;
    for (const auto& part : fs::path(rel_path)) {
        if (part.empty()) continue;          // skip "//"
        if (part == "..") return std::nullopt;
        normalized /= part;
    }
    if (normalized.empty()) return std::nullopt;

    // 8. Look up root entry; reject if unconfigured (path empty).
    const auto& root_entry = roots_[static_cast<size_t>(kind)];
    if (root_entry.path.empty()) return std::nullopt;

    // 9. Compute final path (weakly_canonical resolves symlinks lexically).
    std::error_code ec;
    auto final_path = fs::weakly_canonical(root_entry.path / normalized, ec);
    if (ec) return std::nullopt;
    auto root_canonical = fs::weakly_canonical(root_entry.path, ec);
    if (ec) return std::nullopt;

    // 10. Containment check — blocks symlink-based escapes (e.g. root contains a link to /etc).
    auto mismatch_it = std::mismatch(root_canonical.begin(), root_canonical.end(),
                                     final_path.begin(), final_path.end());
    if (mismatch_it.first != root_canonical.end()) return std::nullopt;

    return final_path;
}

std::optional<std::string> ScriptFilesystem::read(const std::string& vpath, size_t max_bytes) {
    auto resolved = resolve(vpath);
    if (!resolved) return std::nullopt;
    return read_file_with_limit(*resolved, max_bytes);
}

bool ScriptFilesystem::write(const std::string& vpath, std::string_view content) {
    if (content.size() > kAbsoluteMaxBytes) return false;

    auto resolved = resolve(vpath);
    if (!resolved) return false;

    // Find which root this resolved path belongs to (permission check).
    namespace fs = std::filesystem;
    const RootEntry* root_entry = nullptr;
    fs::path resolved_root;
    for (size_t i = 0; i < roots_.size(); ++i) {
        if (roots_[i].path.empty()) continue;
        std::error_code ec;
        auto rc = fs::weakly_canonical(roots_[i].path, ec);
        if (ec) continue;
        auto mismatch_it = std::mismatch(rc.begin(), rc.end(),
                                         resolved->begin(), resolved->end());
        if (mismatch_it.first == rc.end()) {
            root_entry = &roots_[i];
            resolved_root = rc;
            break;
        }
    }
    if (!root_entry) return false;

    // log:// allows append only; data:// never writable.
    if (!root_entry->writable && !root_entry->append_only) {
        std::fprintf(stderr, "ScriptFilesystem::write: root not writable (%s)\n", vpath.c_str());
        return false;
    }

    // Auto-create parent directories.
    std::error_code ec;
    fs::create_directories(resolved->parent_path(), ec);

    // Atomic write: temp file + rename. Used for save:// (writable, not append_only).
    // cache:// uses direct write.
    if (root_entry->writable && !root_entry->append_only) {
        fs::path tmp = resolved->string() + ".tmp." + random_suffix();
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f.write(content.data(), content.size());
            if (!f) {
                std::error_code rm_ec;
                fs::remove(tmp, rm_ec);
                return false;
            }
        }
        fs::rename(tmp, *resolved, ec);
        if (ec) {
            std::error_code rm_ec;
            fs::remove(tmp, rm_ec);
            return false;
        }
        return true;
    }
    return false;
}

bool ScriptFilesystem::append(const std::string& vpath, std::string_view content) {
    if (content.size() > kAbsoluteMaxBytes) return false;

    auto resolved = resolve(vpath);
    if (!resolved) return false;

    namespace fs = std::filesystem;
    const RootEntry* root_entry = nullptr;
    for (size_t i = 0; i < roots_.size(); ++i) {
        if (roots_[i].path.empty()) continue;
        std::error_code ec;
        auto rc = fs::weakly_canonical(roots_[i].path, ec);
        if (ec) continue;
        auto mismatch_it = std::mismatch(rc.begin(), rc.end(),
                                         resolved->begin(), resolved->end());
        if (mismatch_it.first == rc.end()) {
            root_entry = &roots_[i];
            break;
        }
    }
    if (!root_entry) return false;

    // Append allowed on: save:// (writable), cache:// (writable), log:// (append_only).
    // Rejected on data:// (not writable, not append_only).
    if (!root_entry->writable && !root_entry->append_only) {
        std::fprintf(stderr, "ScriptFilesystem::append: root not appendable (%s)\n", vpath.c_str());
        return false;
    }

    std::error_code ec;
    fs::create_directories(resolved->parent_path(), ec);

    std::ofstream f(*resolved, std::ios::binary | std::ios::app);
    if (!f) return false;
    f.write(content.data(), content.size());
    return static_cast<bool>(f);
}

bool ScriptFilesystem::exists(const std::string& vpath) {
    auto resolved = resolve(vpath);
    if (!resolved) return false;
    std::error_code ec;
    return std::filesystem::exists(*resolved, ec);
}

std::optional<uint64_t> ScriptFilesystem::size(const std::string& vpath) {
    auto resolved = resolve(vpath);
    if (!resolved) return std::nullopt;
    std::error_code ec;
    auto sz = std::filesystem::file_size(*resolved, ec);
    if (ec) return std::nullopt;
    return static_cast<uint64_t>(sz);
}

std::vector<std::string> ScriptFilesystem::list(const std::string& vpath) {
    std::vector<std::string> out;
    auto resolved = resolve(vpath);
    if (!resolved) return out;

    std::error_code ec;
    if (!std::filesystem::is_directory(*resolved, ec)) return out;

    for (auto it = std::filesystem::directory_iterator(*resolved, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        out.push_back(it->path().filename().string());
        if (out.size() >= kMaxListEntries) break;
    }
    return out;
}

bool ScriptFilesystem::remove(const std::string& vpath) {
    auto resolved = resolve(vpath);
    if (!resolved) return false;

    namespace fs = std::filesystem;
    const RootEntry* root_entry = nullptr;
    for (size_t i = 0; i < roots_.size(); ++i) {
        if (roots_[i].path.empty()) continue;
        std::error_code ec;
        auto rc = fs::weakly_canonical(roots_[i].path, ec);
        if (ec) continue;
        auto mismatch_it = std::mismatch(rc.begin(), rc.end(),
                                         resolved->begin(), resolved->end());
        if (mismatch_it.first == rc.end()) {
            root_entry = &roots_[i];
            break;
        }
    }
    if (!root_entry || !root_entry->removable) {
        std::fprintf(stderr, "ScriptFilesystem::remove: root not removable (%s)\n", vpath.c_str());
        return false;
    }

    std::error_code ec;
    return fs::remove(*resolved, ec);
}

void ScriptFilesystem::read_async(const std::string& vpath, size_t max_bytes,
                                  std::function<void(bool, std::string)> callback) {
    auto resolved = resolve(vpath);
    if (!resolved) {
        // Path illegal — still post callback to main thread with failure.
        post_to_main_thread([cb = std::move(callback)]() { cb(false, ""); });
        return;
    }
    auto path = *resolved;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        work_queue_.push_back([path, max_bytes, cb = std::move(callback)]() {
            auto content = read_file_with_limit(path, max_bytes);
            bool ok = content.has_value();
            std::string data = ok ? *content : "";
            post_to_main_thread(
                [cb, ok, data = std::move(data)]() { cb(ok, std::move(data)); });
        });
    }
    queue_cv_.notify_one();
}

void ScriptFilesystem::initialize() {
    if (worker_.joinable()) return;  // already running
    stop_ = false;
    worker_ = std::thread([this]() {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#if defined(__APPLE__)
        pthread_setname_np("ScriptFSWorker");
#else
        // Linux 签名带 pthread_t（macOS 版本作用于当前线程无此参数）
        pthread_setname_np(pthread_self(), "ScriptFSWorker");
#endif
#endif
        worker_loop();
    });
}

void ScriptFilesystem::shutdown() {
    // Must be idempotent — called by LuaBackend::shutdown() AND ~ScriptFilesystem().
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    // Pending work_queue_ entries dropped; their Lua refs leak until L close.
}

void ScriptFilesystem::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() { return stop_.load() || !work_queue_.empty(); });
            if (stop_.load() && work_queue_.empty()) return;
            task = std::move(work_queue_.front());
            work_queue_.pop_front();
        }
        task();
    }
}

}  // namespace primal::script
