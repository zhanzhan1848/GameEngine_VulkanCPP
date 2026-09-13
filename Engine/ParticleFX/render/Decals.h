#pragma once

// Decals.h — decal 渲染器 CPU 侧数据打包（架构文档 §7-N6）。
// 每个活粒子 → 一个球形投影体积（世界中心 + 半径 + 颜色）。
// 屏幕 space 投影：ParticleFXDecalPass 全屏实例绘制，片段从场景深度重建世界位置，
// 体积内按径向衰减给表面着色（延迟贴花标准技法，深度对齐 v1、法线对齐 v2）。

#include "../PfxTypes.h"
#include "../core/AttributeStore.h"

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::render {

// decal 实例（48B，与 ParticleFXDecal.metal 的 PFXDecalInstance 对齐）
struct decal_instance_data {
    f32 center_radius[4];   // xyz = 世界中心, w = 半径
    f32 color[4];           // rgb = 贴花色, a = 强度
    f32 params[4];          // x = 软边比例 (0..1)，yzw 保留
};
static_assert(sizeof(decal_instance_data) == 48, "decal instance layout locked (48B)");

// 活粒子 → decal 实例（颜色取 color 通道，半径统一）。返回写入数。
u32 pack_decal_instances(const attribute_store& store, f32 radius, f32 intensity,
                         decal_instance_data* out, u32 cap);

} // namespace primal::pfx::render

#endif // !DISABLE_PARTICLE_FX
