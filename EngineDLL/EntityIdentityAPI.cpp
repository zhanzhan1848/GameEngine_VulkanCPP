// EntityIdentity C ABI 实现（2026-11-05 lightmap 稳定键轮）。契约见
// EntityIdentityAPI.h。零管线依赖：直接进注册表（引擎内 mutex 串行化），
// 无渲染帧参与，编辑器推送线程可安全调用。

#include "EntityIdentityAPI.h"

#include "Graphics/EntityIdentityRegistry.h"

#include <cstddef>

// ABI 纪律：POD 镜像 static_assert 锁尺寸（沿 AssetAPI.h 惯例）。
static_assert(sizeof(EntityIdentityC) == 68,
              "EntityIdentityC layout frozen (ABI 镜像锁尺寸)");
static_assert(sizeof(primal::graphics::EntityIdentity) == 68,
              "engine EntityIdentity layout must match C mirror");
static_assert(sizeof(EntityIdentityC) == sizeof(primal::graphics::EntityIdentity),
              "EntityIdentityC / EntityIdentity layout parity");
static_assert(offsetof(EntityIdentityC, stable_key) ==
                  offsetof(primal::graphics::EntityIdentity, stable_key),
              "EntityIdentityC / EntityIdentity field offset parity");

EDITOR_INTERFACE unsigned int PipelineSetEntityIdentities(const struct EntityIdentityC* identities,
                                                          unsigned int count) {
    // 布局同构已由上方 static_assert（尺寸 + 字段偏移）锁死 → 同构视图
    // 零拷贝直传；宿主未来改动 C 侧类型时此处编译期即断。u32 → size_t。
    if (!identities && count > 0) return 0u;
    if (count == 0) return 1u;
    return primal::graphics::SetEntityIdentities(
        reinterpret_cast<const primal::graphics::EntityIdentity*>(identities),
        static_cast<size_t>(count)) ? 1u : 0u;
}

EDITOR_INTERFACE void PipelineClearEntityIdentities() {
    primal::graphics::ClearEntityIdentities();
}
