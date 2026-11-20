#pragma once

// EntityIdentity C ABI（2026-11-05 lightmap 稳定键轮）。
//
// 契约：导出只增不改；EntityIdentityC 与引擎侧
// primal::graphics::EntityIdentity（Engine/Graphics/EntityIdentityRegistry.h）
// 逐字节同构，布局由 EntityIdentityAPI.cpp 的 static_assert 双向锁死
//（沿 AssetAPI.h / LightmapAPI.h 惯例）。C# 侧镜像必须逐字段对齐。
//
// 语义：SetEntityIdentities = 整表原子替换（C# 侧推送全量快照；先整批
// 校验，任一条目键为空或 >=64 字符则整批拒绝返回 0 且旧表保持不变；
// count==0 = 清空，空快照合法）。Clear 显式清空。
// stable_key 约定 = "e" + entityGuid "N" 格式（32 小写 hex），总长 33。
// 线程安全（引擎内 mutex）——编辑器可从推送线程直接调用。

#include "Common.h"

// 与 primal::graphics::EntityIdentity 逐字节同构（68B）。
struct EntityIdentityC {
    unsigned int native_entity_id;
    char stable_key[64];
};

EDITOR_INTERFACE unsigned int PipelineSetEntityIdentities(const struct EntityIdentityC* identities,
                                                          unsigned int count);  // 1=ok 0=失败（整批拒绝）

EDITOR_INTERFACE void PipelineClearEntityIdentities();
