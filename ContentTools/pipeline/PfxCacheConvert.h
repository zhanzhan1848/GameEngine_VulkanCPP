#pragma once

// PfxCacheConvert.h — PFX 离线缓存转换器（架构文档 §5/N4，ContentTools 侧离线工具）。
//
//   alembic_to_pcache：Alembic Ogawa 点缓存/多边形网格位置样本 → .pcache
//   （运行时 geometry_cache 消费；格式契约见架构文档 §5 与 GeometryCache.h）。
//   vdb_to_pgrid：OpenVDB Float/Vec3 Grid 采样为 .pgrid（PGRD v1，标量/向量）
// 链接 vendored Alembic+Imath（third_party/Alembic/Mac）与 OpenVDB（third_party/OpenVDB/Mac）。
// 本目标为离线工具，不进运行时（libEngine 零第三方依赖）。

#include <string>

namespace pfx_convert {

// 从 Alembic 归档提取点位置样本序列，写出 .pcache。
// object_path 为空时取归档中首个 Points/PolyMesh 对象。fps 为烘焙帧率。
bool alembic_to_pcache(const char* abc_path, const char* object_path, float fps,
                       const char* out_pcache, std::string& err);

// vdb_to_pgrid：OpenVDB Float Grid（SDF/标量）或 Vec3SGrid（向量场）→ .pgrid。
// grid_name 为空取第一个网格。采样网格 active bbox 的体素中心。
bool vdb_to_pgrid(const char* vdb_path, const char* grid_name, const char* out_pgrid, std::string& err);

// 测试辅助：写一个 2 帧 × 3 点的合成 Alembic 点缓存（确定性）。
bool make_test_abc(const char* abc_path, std::string& err);

// 读回 .pcache 头校验（magic/帧数/点数）。返回帧数，失败 -1。
int verify_pcache(const char* pcache_path, std::string& err);

} // namespace pfx_convert
