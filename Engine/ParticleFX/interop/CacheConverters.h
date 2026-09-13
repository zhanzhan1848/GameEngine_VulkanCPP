#pragma once

// CacheConverters.h — 离线缓存转换器骨架（架构文档 §5/N4）。
//
// 目标格式（运行时零第三方依赖）：
//   .pgrid  —— volume_grid 均匀网格（标量 SDF / 向量速度场）
//   .pcache —— geometry_cache 点缓存
//
// 源格式转换需要离线库（OpenVDB / Alembic，仅 ContentTools 侧链接）：
//   vdb_converter    ：OpenVDB Vec3f/Float Grid 采样为 pgrid（只读子集）
//   alembic_converter：Alembic Ogawa 点缓存采样为 pcache
// 本骨架提供格式契约与失败语义；库接入（third_party + LICENSE 登记）按
// 架构文档 §8 风险条目推进。运行时永远不链接 OpenVDB/Alembic。

#include "../PfxTypes.h"

#include <string>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::convert {

// 明确的"库未接入"失败语义（宿主调用可探测能力）。
bool vdb_converter_available();     // 当前恒 false
bool alembic_converter_available(); // 当前恒 false

// 骨架入口：未接入库时返回 false 且 err 说明（格式契约见本文档 §5 表）。
bool vdb_to_pgrid(const char* vdb_path, const char* grid_name, const char* out_pgrid, std::string& err);
bool alembic_to_pcache(const char* abc_path, const char* object_path, f32 fps, const char* out_pcache,
                       std::string& err);

} // namespace primal::pfx::convert

#endif // !DISABLE_PARTICLE_FX
