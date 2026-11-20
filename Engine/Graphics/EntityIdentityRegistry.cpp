// EntityIdentityRegistry 实现（2026-11-05 lightmap 稳定键轮）。契约与
// 全量替换语义见头注释。

#include "EntityIdentityRegistry.h"
#include "Engine/Common/CoreLogStream.h"  // PRIMAL_LOG_STREAM

#include <cstring>
#include <mutex>
#include <unordered_map>

namespace primal::graphics {

namespace {

// Meyers 单例（C++20 静态初始化线程安全；进程级唯一表）。
std::unordered_map<u32, std::string>& RegistryTable() {
    static std::unordered_map<u32, std::string> table;
    return table;
}

std::mutex& RegistryMutex() {
    static std::mutex m;
    return m;
}

} // namespace

bool SetEntityIdentities(const EntityIdentity* ids, size_t count) {
    if (!ids && count > 0) return false;

    // 先整批校验再应用（原子：拒绝时旧表保持不变）。
    for (size_t i = 0; i < count; ++i) {
        const EntityIdentity& e = ids[i];
        const size_t len = strnlen(e.stable_key, sizeof(e.stable_key));
        if (len == 0 || len >= sizeof(e.stable_key)) {
            PRIMAL_LOG_STREAM(Warn, "Render")
                << "[EntityIdentityRegistry] reject batch: entry[" << i
                << "] native_entity_id=" << e.native_entity_id
                << " stable_key empty or untrimmed >= 64 chars" << std::endl;
            return false;
        }
    }

    // 整表原子替换（2026-11-05 对接修正轮）：C# 侧推送均为全量快照——
    // 新表锁外构建、锁内 swap，旧条目（含已删实体残留键）全部失效，杜绝
    // ECS id 回收复用后新实体误挂旧键。count==0 → 空表（清空，合法快照）。
    // 批内重复 native_entity_id 取首条（emplace 不覆盖，确定性）。
    std::unordered_map<u32, std::string> fresh;
    fresh.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        fresh.emplace(ids[i].native_entity_id,
                      std::string(ids[i].stable_key,
                                  strnlen(ids[i].stable_key,
                                          sizeof(ids[i].stable_key))));
    }
    std::lock_guard<std::mutex> lock(RegistryMutex());
    RegistryTable().swap(fresh);
    return true;
}

void ClearEntityIdentities() {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    RegistryTable().clear();
}

bool GetStableKey(u32 native_entity_id, std::string& out) {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    const auto it = RegistryTable().find(native_entity_id);
    if (it == RegistryTable().end()) return false;
    out = it->second;
    return true;
}

} // namespace primal::graphics
