#pragma once

// GeometryCache.h — 离线烘焙点缓存回放数据层（架构文档 §5，Houdini/Alembic 联动）。
//
// .pcache 自有格式（小端）：
//   "PCH1" | u32 frame_count | u32 point_count
//   | u32 frame_indices[frame_count]        —— 递增帧号（对应烘焙 fps）
//   | f32  points[frame_count][point_count*3]
// 转换器（Alembic/bgeo → .pcache）见 CacheConverters.h——运行时零第三方依赖。

#include "../PfxTypes.h"

#include <memory>
#include <string>
#include <vector>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx::interop {

class geometry_cache {
public:
    geometry_cache() = default;
    ~geometry_cache() = default;
    DISABLE_COPY_AND_MOVE(geometry_cache);

    bool load(const char* path, std::string& err);
    bool save(const char* path, std::string& err) const;   // 离线烘焙写入端（测试/工具用）

    // 回放采样：time 秒 × fps → 帧（最近邻）。loop=false 时越界取末端帧。
    // 返回该帧点数（0 = 未加载）；out_points 容量不足返回 -所需容量（负值）。
    s32 sample(f32 time, f32 fps, bool loop, math::v3* out_points, u32 cap) const;

    u32 frame_count() const { return (u32)_frame_ids.size(); }
    u32 point_count() const { return _point_count; }

    // 烘焙写入端（转换器/测试用）
    u32 add_frame(const f32* xyz, u32 count);   // 追加一帧，返回帧下标；点数不一致返回 u32_invalid_id

private:
    u32 _point_count{ 0 };
    std::vector<u32> _frame_ids;
    std::vector<f32> _data;   // 全帧连续 [frame][point*3]
};

} // namespace primal::pfx::interop

#endif // !DISABLE_PARTICLE_FX
