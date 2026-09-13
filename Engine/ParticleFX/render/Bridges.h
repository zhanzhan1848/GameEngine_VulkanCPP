#pragma once

// Bridges.h — 渲染桥：渲染器插件是属性表的视图（架构文档 §3.6/N2）。
// SpriteBridge 把标准通道按行打包为旧 Particles::particle_data 交错布局，
// 直接喂给现有 ParticlePass 渲染路径（兼容桥，渲染侧零改动）。
// MeshInstanceBridge 产出 mesh 渲染器的 per-instance 变换缓冲（48B/实例）。

#include "../PfxTypes.h"
#include "../core/AttributeStore.h"

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::render {

// 把 store 中全部活行按行序打包为旧交错布局。返回打包行数（≤ cap）。
// out 为 nullptr 或 cap 不足时返回需要的行数（探测用法）。
u32 pack_sprite_instances(const attribute_store& store, void* out, u32 cap_bytes);

// mesh 渲染器实例数据（48B；四元数当前为单位姿态——自旋通道 N4+ 引入）。
struct mesh_instance_data {
    f32 pos[3];
    f32 pad0;
    f32 rot[4];    // 单位四元数 [x,y,z,w]
    f32 scale[2];
    f32 pad1[2];
};
static_assert(sizeof(mesh_instance_data) == 48, "mesh instance layout locked (48B)");

// 把 store 活行打包为 mesh 实例缓冲。返回实例数（≤ cap）。
u32 pack_mesh_instances(const attribute_store& store, mesh_instance_data* out, u32 cap);

} // namespace primal::pfx::render

#endif // !DISABLE_PARTICLE_FX
