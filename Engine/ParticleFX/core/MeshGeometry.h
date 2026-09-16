#pragma once

// MeshGeometry.h — PFX 发射源网格采样几何（ShapeLocation shape=mesh_surface 消费）。
// 与场表（FieldSystem 的 .pgrid）同模式：编译期从 OBJ 文件加载三角形汤，
// 由 emitter_program 持共享实例、sim_context 视图下发给模块；运行期零 IO。
//
// 采样加速数据（load_obj 末尾烘焙）：
//   area  — 面积均匀（概率 ∝ 三角形面积）
//   color — 顶点色 R 加权（概率 ∝ 面积 × 三角形顶点色 R 均值；网格无顶点色时退化为 area）
//   uv    — uv0.u 加权（同上，无 uv 时退化为 area）
// bias 极化（运行期参数）按 per-triangle 均值随机接受实现，无需按 bias 重烘 CDF。

#include "../PfxTypes.h"

#include <string>
#include <vector>

#ifndef DISABLE_PARTICLE_FX

namespace primal::pfx {

struct mesh_geometry {
    std::vector<math::v3> positions;
    std::vector<u32>      indices;    // 三角形汤（3 / 三角形）
    std::vector<f32>      corner_r;   // 与 indices 平行：顶点色 R（缺失 = -1）
    std::vector<f32>      corner_u;   // 与 indices 平行：uv0.u（缺失 = -1）

    u32 triangle_count() const { return (u32)(indices.size() / 3); }

    // 三角形抽取分布：cdf 为累计概率（末项 = total），means 为每三角形概率权重
    //（面积 × 密度；面积变体密度恒 1），dens 为纯密度（bias 极化的接受概率基准）。
    // valid=false 表示源数据缺席（调用方退化为 area）。
    struct weight_cdf {
        std::vector<f32> cdf;
        std::vector<f32> means;
        std::vector<f32> dens;
        f32              total{ 0.f };
        f32              max_dens{ 1.f };
        bool             valid{ false };
    };
    weight_cdf area;
    weight_cdf color;
    weight_cdf uv;

    // cdf 上按 [0,total) 均匀样本抽三角形下标（二分；空分布返回 0）
    u32 sample_triangle(const weight_cdf& w, f32 u) const;

    // OBJ 加载（路径相对进程 CWD —— 与 fields 的 .pgrid 同约定）。
    // 多 shape 全部并入一张汤；面片自动三角化。失败返回 false 并写 err。
    bool load_obj(const char* path, std::string& err);
};

} // namespace primal::pfx

#endif // !DISABLE_PARTICLE_FX
