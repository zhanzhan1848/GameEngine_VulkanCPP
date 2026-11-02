#pragma once

// Bridges.h — 渲染桥：渲染器插件是属性表的视图（架构文档 §3.6/N2）。
// SpriteBridge 把标准通道按行打包为旧 Particles::particle_data 交错布局，
// 直接喂给现有 ParticlePass 渲染路径（兼容桥，渲染侧零改动）。
// MeshInstanceBridge 产出 mesh 渲染器的 per-instance 变换缓冲（48B/实例）。

#include "../PfxTypes.h"
#include "Graphics/Volume/SparseParticleVolume.h"
#include "../core/AttributeStore.h"

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::render {

// 把 store 中全部活行按行序打包为旧交错布局。返回打包行数（≤ cap）。
// out 为 nullptr 或 cap 不足时返回需要的行数（探测用法）。
u32 pack_sprite_instances(const attribute_store& store, void* out, u32 cap_bytes);

// Explicit volume opt-in. World radius = abs(scale_rotation.x) * radius_at_scale_one;
// density = max(color.a,0) * density_scale. Dead rows are never submitted.
std::vector<graphics::volume::VolumeParticle> pack_volume_particles(
    const attribute_store& store, f32 radius_at_scale_one, f32 density_scale);

// mesh 渲染器实例数据（48B；P1 起消费旋转与实例色）。
struct mesh_instance_data {
    f32 pos[3];
    f32 col_r;      // 实例色 R（原 pad0；来自 color 通道 rgb）
    f32 rot[4];     // 四元数 [x,y,z,w]（scale_rotation.z 弧度绕 +Y；未写旋转 = 单位姿态）
    f32 scale[2];
    f32 col_gb[2];  // 实例色 G/B（原 pad1）
};
static_assert(sizeof(mesh_instance_data) == 48, "mesh instance layout locked (48B)");

// 把 store 活行打包为 mesh 实例缓冲。返回实例数（≤ cap）。
u32 pack_mesh_instances(const attribute_store& store, mesh_instance_data* out, u32 cap);

// ---- 渲染 instance 几何加载（ForwardSceneRenderer 注册到 ParticleFXMeshPass 前消费）----
// 两种来源：
//   1. cooked .mesh（ContentTools legacy scene blob）→ 文件读取后解包；
//   2. "builtin:<name>"（box/sphere/plane/capsule）→ 程序化几何注册进 content 系统后解包。
// 失败写 err 返回 false。进程级缓存：同路径只读盘一次；失败结果也被缓存（不反复重试）。
struct instance_geometry {
    std::vector<math::v3> positions;
    std::vector<math::v3> normals;   // element_buffer 前 12B；资产无法线时为零向量
    std::vector<u32> indices;        // 统一扩展为 u32（pass 侧建 R32 索引缓冲）
    u32 index_count{ 0 };
};
bool load_instance_geometry(const char* cooked_mesh_path, instance_geometry& out, std::string& err);

} // namespace primal::pfx::render

#endif // !DISABLE_PARTICLE_FX
