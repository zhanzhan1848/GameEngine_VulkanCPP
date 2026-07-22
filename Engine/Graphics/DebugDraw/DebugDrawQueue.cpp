// Phase 5: DebugDrawQueue 实现 —— 单例 + 互斥保护。
//
// ForwardSceneRenderer 每帧 BeginFrame 时 drain_into 到本地 vector，
// 然后 AddLines 到 LineBatchRenderer。EngineDLL 的 DrawDebugXxx 在主线程
// 或网络线程随时写入；drain + add 互斥保证无竞争。

#include "DebugDrawQueue.h"
#include <mutex>

namespace primal::graphics::debug_draw {

namespace {

std::mutex g_mutex;

std::vector<DebugLine>& queue_impl() {
    static std::vector<DebugLine> q;
    return q;
}

} // anonymous namespace

std::vector<DebugLine>& queue() {
    return queue_impl();
}

void add_line(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1, u32 rgb) {
    std::lock_guard<std::mutex> lock(g_mutex);
    queue_impl().push_back({
        math::v3{x0, y0, z0},
        math::v3{x1, y1, z1},
        rgb
    });
}

void clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    queue_impl().clear();
}

u32 drain_into(std::vector<DebugLine>& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& q = queue_impl();
    const u32 n = static_cast<u32>(q.size());
    if (n == 0) return 0;
    out.insert(out.end(), q.begin(), q.end());
    q.clear();
    return n;
}

u32 line_count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<u32>(queue_impl().size());
}

} // namespace primal::graphics::debug_draw
