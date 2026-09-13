#pragma once

// FieldSystem.h — PFX 场系统（架构文档 §3.4，Houdini POP 场能力的载体）。
// volume_grid：均匀网格标量/向量场（SoA 连续数据 + 三线性采样）。
// .pgrid：自有紧凑序列化格式（离线转换器的目标格式——VDB/ContentTools SDF
// 均烘焙为 pgrid 后被运行时消费；转换器骨架见 interop/CacheConverters.h）。
// 全部纯 CPU、确定性；界外采样返回 outside 值（SDF 场 = 正的外推距离）。

#include "../PfxTypes.h"

#include <memory>
#include <string>
#include <vector>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx {

class volume_grid {
public:
    enum class kind : u8 { scalar = 0, vector3 = 1 };

    volume_grid() = default;
    ~volume_grid();
    DISABLE_COPY_AND_MOVE(volume_grid);

    bool init_scalar(u32 nx, u32 ny, u32 nz, math::v3 origin, f32 cell, f32 fill);
    bool init_vector(u32 nx, u32 ny, u32 nz, math::v3 origin, f32 cell, math::v3 fill);
    void shutdown();
    bool is_initialized() const { return !_data.empty(); }

    kind get_kind() const { return _kind; }
    math::v3 origin() const { return _origin; }
    f32 cell() const { return _cell; }
    u32 dim(u8 axis) const { return _dims[axis]; }
    f32 outside() const { return _outside; }
    void set_outside(f32 v) { _outside = v; }   // SDF 场的外推值（正 = 体外）

    // ---- 采样（世界空间；三线性插值）----
    f32 sample_scalar(math::v3 world) const;
    math::v3 sample_vector(math::v3 world) const;
    // 中心差分梯度（标量场）：返回 (∂f/∂x, ∂f/∂y, ∂f/∂z)
    math::v3 sample_gradient(math::v3 world) const;

    // ---- 写入（烘焙/测试用）----
    void set_scalar(u32 ix, u32 iy, u32 iz, f32 v);
    void set_vector(u32 ix, u32 iy, u32 iz, math::v3 v);

    // ---- .pgrid 序列化 ----
    bool write_pgrid(const char* path, std::string& err) const;
    bool read_pgrid(const char* path, std::string& err);

private:
    size_t index(u32 ix, u32 iy, u32 iz) const {
        return ((size_t)iz * _dims[1] + iy) * _dims[0] + ix;
    }
    // 世界 → 连续格点坐标；界外标记由调用方处理
    void world_to_grid(math::v3 world, f32& gx, f32& gy, f32& gz) const;

    kind _kind{ kind::scalar };
    u32 _dims[3]{ 0, 0, 0 };
    math::v3 _origin{};
    f32 _cell{ 1.f };
    f32 _outside{ 0.f };
    std::vector<f32> _data;   // 标量: n 项；向量: 3n 项（x,y,z 连续）
};

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
