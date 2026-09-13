#pragma once

// LegacyPresetConvert.h — pfx.convert：旧 particles 预设 → PFX JSON 资产（架构文档 §4 转换行）。
// interop 层允许 include 旧 Engine/Particles/ 头（边界层）。
//
// 映射（保语义近似）：
//   spawn_rate → SpawnRate.rate；lifetime → SetLifetime；velocity 矩形随机 →
//   AddVelocity(fixed, 方向取速度中值方向，速率取模长范围——有损近似，已在
//   legacy_render 保留原值)；color/scale 区间 → SetColor/SetScale 区间；
//   gravity → GravityForce；drag → DragForce。
// 曲线（scale/alpha/color/force/velocity/rotation）与渲染参数（图集/混合）当前
// JSON 无对应表达：无损保留在 "legacy_render"/"legacy_curves" 字段，N2 渲染器
// 与曲线数据接口落地后升级为正式表达。

#include "Common/CommonHeaders.h"
#include "Particles/ParticlePreset.h"

#include <string>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::convert {

// 单个旧预设 → JSON 文本。失败返回 false 并写 err。
bool convert_preset_to_json(const particles::particle_preset& preset, std::string& out_json, std::string& err);

// 全部内置预设 → out_dir 目录下的 <name>.pfx_emitter 文件。返回成功数。
u32 convert_all_builtins(const char* out_dir, std::string& err);

} // namespace primal::pfx::convert

#endif // !DISABLE_PARTICLE_FX
