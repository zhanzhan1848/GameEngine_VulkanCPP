#pragma once

#include "CommonHeaders.h"
#include <string>   // R3 IWYU: std::string 使用点直接补齐
#include <vector>

// ============================================================================
// SurfaceStaticPages —— SC per-mesh 静态表面页 sidecar（"SCS1"）读取器。
//
// 计划：Docs/2026-09-30-sc-per-mesh-surface-bake-and-lookup-plan.md P2。
// 离线烘焙端：ContentTools/common/CardSurfaceBake.h（同结构 static_assert
// 逐字节锁定，改动必须两侧同步）。
//
// 身份契约（计划 §3.1）：sidecar 记录以 (geometry_digest, layout_digest, material_digest)
// 三摘要键控；任一不匹配 → FindMesh 返回 INVALID，运行时对该 mesh 保持
// 旧路径（capture），绝不读旧页冒充新数据（§3.3 版本不匹配语义）。
//
// 生产约束（§3.2）：静态卡的表面数据只能来自本 sidecar——合成后静态卡
// 跳过 capture 与 CardFill（SurfaceCachePass），外来几何结构上无法写入。
//
// 资源预算（P0 登记）：sidecar 不入 git（300MB @ Sponza），按需再生成：
//   primal-engine sc-bake-surface --in <model> --out <model>.scsurf
//   --texture-dir <dir>
// 运行时只上传被分配卡片的页（受现有 atlas 容量约束，GPU 侧无增量）；
// CPU 侧按需 fread 单卡页平面，无全量驻留。
// ============================================================================

namespace primal::graphics::lumen {

// 与离线端 ScsHeader/ScsMeshRecord/ScsCardPageEntry 逐字节镜像。
struct ScsHeaderRT
{
    u32 magic;
    u32 version;
    u32 header_bytes;
    u32 flags;
    f32 albedo_scale;
    f32 texels_per_unit;
    u32 mesh_count;
    u32 page_size;
    u32 min_resolution;
    u32 max_resolution;
    f32 capture_near;
    f32 capture_far_scale;
    f32 capture_far_bias;
    u32 reserved[11];
};
static_assert(sizeof(ScsHeaderRT) == 96, "SCS1 header mirror drift (CardSurfaceBake.h)");

struct ScsMeshRecordRT
{
    u64 geometry_digest;
    u64 layout_digest;
    u64 material_digest;
    u32 name_hash;
    u32 card_count;
    u64 pages_offset;
    u64 pages_bytes;
};
static_assert(sizeof(ScsMeshRecordRT) == 48, "SCS1 mesh record mirror drift");

struct ScsCardPageEntryRT
{
    u32 card_index;
    u32 resolution;
    u64 albedo_offset;
    u64 normal_offset;
    u64 depth_offset;
    u64 params_offset;
    u64 triangle_offset;
};
static_assert(sizeof(ScsCardPageEntryRT) == 48, "SCS1 page entry mirror drift");

class SurfaceStaticPages
{
public:
    static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;
    static constexpr u32 kScsMagic = 0x31534353u;   // "SCS1"
    static constexpr u32 kScsVersion = 4u;

    SurfaceStaticPages() = default;
    ~SurfaceStaticPages();
    SurfaceStaticPages(const SurfaceStaticPages&) = delete;
    SurfaceStaticPages& operator=(const SurfaceStaticPages&) = delete;

    // 解析 header + mesh 记录（页数据不驻留，按需 ReadCardPlanes）。
    bool Attach(const char* path);
    void Detach();
    bool IsAttached() const { return file_ != nullptr; }
    const char* Path() const { return path_.c_str(); }

    // 三摘要键控查找；INVALID = 无页（调用方保持 capture 路径）。
    u32 FindMesh(u64 geometry_digest, u64 layout_digest, u64 material_digest) const;

    u32 MeshCount() const { return static_cast<u32>(meshes_.size()); }
    u64 TotalPageBytes() const;
    f32 TexelsPerUnit() const { return header_.texels_per_unit; }
    f32 CaptureNear() const { return header_.capture_near; }
    f32 AlbedoScale() const { return header_.albedo_scale; }

    // 单卡表面平面读取（可同时读取原始 TriangleID）（albedo RGBA8 / normal RG16F / depth R32F）。
    // page_card = 卡在 mesh bake.cards 内的序号；out_resolution = 页分辨率。
    bool ReadCardPlanes(u32 slot, u32 page_card,
                        std::vector<u8>& out_albedo,
                        std::vector<u8>& out_normal,
                        std::vector<f32>& out_depth,
                        u32& out_resolution, std::vector<u32>* out_triangles = nullptr) const;

    // 页平面文件偏移（pool 构建走批量读时的诊断/审计用）。
    bool CardPageEntry(u32 slot, u32 page_card, ScsCardPageEntryRT& out) const;

private:
    FILE* file_ = nullptr;
    std::string path_;
    ScsHeaderRT header_{};
    struct MeshEntry
    {
        u64 geometry_digest;
        u64 layout_digest;
        u64 material_digest;
        u32 card_count;
        std::vector<ScsCardPageEntryRT> pages;
        u64 entries_offset;   // CardPageEntry 数组文件偏移
    };
    std::vector<MeshEntry> meshes_;
};

}  // namespace primal::graphics::lumen
