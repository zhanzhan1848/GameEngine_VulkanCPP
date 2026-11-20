#pragma once

// EntityIdentityRegistry（2026-11-05 lightmap 稳定键轮）。
//
// 契约：native entity id（ECS 运行时 id，会话内易失）→ 跨会话稳定键
//（编辑器 entityGuid 的 "e"+32 小写 hex 形态，总长 33 字符）的注册表。
// lightmap bake 写键（LightmapSceneCollector）与渲染匹配键
//（LightmapModule::UpdateFrame）经 ResolveLightmapInstanceKey 共用本表，
// 使持久化的 lightmap.lmap（name[64] 键）跨会话按稳定键匹配，不再因
// load-order / 会话 id 漂移静默失配。
//
// 线程模型：全接口 mutex 串行化。编辑器经 EngineDLL（外部推送线程）写，
// lightmap 收集（主线程）与渲染匹配（渲染线程）读。单次调用内部原子；
// 无读写锁分层（表规模 = 场景实体数，量级小，mutex 足够）。
//
// 语义（2026-11-05 对接修正轮：全量替换）：
//   - SetEntityIdentities(ids, count)：整表原子替换——先整批校验，通过后
//     锁内以新表换掉旧表（旧条目全部失效，不残留）。C# 侧所有推送点均为
//     全量快照（PrimalEditor Native/INativeEngineBridgeLike.cs 适配层），
//     故选全量替换：已删实体的残留键不会在 ECS id 回收复用后（编辑器不
//     跟踪的 mesh 路径存在复用）把新实体 bake/匹配到错误键下。同一份数据
//     重复推送幂等。批内重复 native_entity_id 取首条（确定性）。
//   - count==0（含 ids==nullptr）= 替换为空表（清空）——空快照合法。
//   - 校验：任一条目 stable_key 为空、或非零长度超过 63 字符
//     （LMAP name[64] 装不下）→ 整批拒绝（旧表保持不变），返回 false。
//   - ids==nullptr 且 count>0 → false。
//   - 未推送 / 推空表期间注册表为空表是合法态：收集/渲染未命中走
//     ResolveLightmapInstanceKey 的旧命名回退，自愈（两侧同款）。
//
// 注意：老 .lmap 产物（mesh_<hierarchyId>_e<entityId> 键）跨会话失配是
// 可接受语义——rebake 即可（LightmapContainer version 1 二进制格式不变，
// 变的只是 name 内容）。

#include "CommonHeaders.h"
#include "Common/Id.h"

#include <string>

namespace primal::graphics {

// C ABI 镜像 EntityIdentityC（EngineDLL/EntityIdentityAPI.h）逐字节同构；
// 布局一致性由 EntityIdentityAPI.cpp 的 static_assert 双向锁死。
struct EntityIdentity {
    u32 native_entity_id{0};
    char stable_key[64]{};
};

// 整表原子替换（见上语义）。返回 false = 整批拒绝（校验失败/null 指针），
// 旧表保持不变。
bool SetEntityIdentities(const EntityIdentity* ids, size_t count);

// 清空注册表（场景卸载/全量重建前由宿主调用）。
void ClearEntityIdentities();

// 命中 → true 且 out = 稳定键；未命中 → false（out 不改动）。
bool GetStableKey(u32 native_entity_id, std::string& out);

// lightmap 实例键统一解析（bake 写键与渲染匹配键两侧共用，杜绝双实现漂移）：
// 注册表命中 → 稳定键；未命中 → 老命名 "mesh_<mesh键位>_e<entityId>"
//（runtime id 串，行为与 2026-11-05 之前零回归）。
// mesh_key_id 传 content geometry id（proxy.contentMeshId，资产稳定；editor
// 管线路径的 proxy.meshId 是 mesh 资源绑定的 ECS 实体 id，勿传），仅回退路径
// 使用（2026-11-05 对接修正：收集器与模块两侧均已改传 contentMeshId）。
inline std::string ResolveLightmapInstanceKey(u32 entity_id, id::id_type mesh_key_id) {
    std::string stable;
    if (GetStableKey(entity_id, stable)) return stable;
    return "mesh_" + std::to_string(mesh_key_id) + "_e" + std::to_string(entity_id);
}

} // namespace primal::graphics
