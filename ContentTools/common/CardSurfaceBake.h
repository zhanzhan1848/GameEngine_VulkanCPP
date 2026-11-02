#pragma once

// ============================================================================
// SC per-mesh 离线表面页烘焙（P1）——
//   Docs/2026-09-30-sc-per-mesh-surface-bake-and-lookup-plan.md §3.3/§4/P1。
//
// 对 mesh 的每张 baked 卡片：沿卡面法线做逐 texel 中心正交射线，与
// **owner mesh 自己的三角形**求交，记录首个命中表面：
//   - coverage mask（albedo.a）
//   - 卡局部 axis depth（R32F，-1 = 无效，与运行时查询契约一致）
//   - mesh 局部插值顶点法线（无属性时用几何法线），按卡眼翻转后存 oct RG16F
//   - albedo（sRGB 纹理双线性 × tint × albedo_scale，线性 RGBA8）
//   - 材质参数（RGBA8：roughness/metallic/emissive_mag/flags——当前
//     MaterialInstance 默认值；SC 消费方尚未读取，契约完整性占位）
//
// owner 契约（§3.2）：烘焙输入只有 owner mesh 的几何与材质；场景内其他
// mesh 无法进入静态页。多 Texel 中心射线漏掉的细几何 = 覆盖欠采样，宁可
// mask=0 也不伪有效（§3.2"无真实覆盖的 texel 保持无效"）。
//
// 产物 = sidecar 文件（"SCS1"，不进 mesh blob——393 mesh 全量页约
// 数百 MB，blob 化会拖垮资产管线）。sidecar 以 geometry/layout/material 三摘要
// 键控（§3.1 身份契约）：几何变化或卡片重排都会使页失效，运行时拒绝
// 旧页冒充新数据。
//
// 运行时读者：Engine/Graphics/Lumen/SurfaceCache/SurfaceStaticPages.{h,cpp}
// ——两侧 struct 布局用 static_assert 逐字节锁定，改动必须同步。
// ============================================================================

#include "ToolsCommon.h"
#include "CardBake.h"
#include "Engine/Graphics/RHI/Core/CardSurfaceIdentity.h"
#include "Engine/Graphics/RHI/Core/CardBoundarySidecar.h"

namespace primal::tools::card_surface {

// ---- 摘要（§3.1 MeshAssetID+GeometryVersion / CardID+LayoutVersion 的落地）----
// geometry: 顶点数/索引数/位置字节/索引字节/element 字节 逐字节 FNV-1a64。
// layout:   CDB2 卡片数组逐字节 FNV-1a64。
u64 fnv1a64(const void* data, u64 bytes, u64 seed = 0xcbf29ce484222325ull);

// ---- 纹理源（调用方负责加载；bake 只采样）----
struct RgbaImage
{
    u32 width{ 0 }, height{ 0 };
    std::vector<u8> rgba;   // sRGB 字节（stbi 原样）
    bool valid{ false };
};

// ---- mesh 几何视图（零拷贝；来自 pipeline::PackedMeshView）----
struct MeshGeometryView
{
    const f32* positions{ nullptr };   // 12 B × vertex_count
    u32 vertex_count{ 0 };
    const u8* indices{ nullptr };      // index_size ∈ {2,4}（可为展宽副本）
    u32 index_size{ 0 };
    u32 index_count{ 0 };              // 3 的倍数
    const u8* elements{ nullptr };     // 逐顶点；static_normal_texture 布局（uv 在 +16）
    u32 element_stride{ 0 };           // 24 = static_normal_texture；0 = 无 UV/法线
    // [P0 身份契约] 摘要专用原生索引字节：geometry_digest 必须与运行时
    // ComputeCardBakeDigests（RHIMeshAsset.h，blob 原生 index_buffer 字节）
    // 逐字节一致；空 = 退回 indices/index_size。
    const u8* digest_indices{ nullptr };
    u32 digest_index_size{ 0 };
};

// ---- 单卡页（平面序列，与 SCS1 文件逐平面一致）----
struct CardPage
{
    u32 card_index{ 0 };
    u32 resolution{ 0 };
    std::vector<u8> albedo;    // RGBA8 线性（a = coverage 255/0）
    std::vector<u8> normal;    // RG16F oct（mesh 局部，已按卡眼令 dot(N, forward) <= 0）
    std::vector<u32> triangle_ids; // Original index-buffer triangle ordinal; UINT32_MAX = invalid.
    std::vector<f32> depth;    // R32F 卡局部 axis depth（-1 = 无效）
    std::vector<u8> params;    // RGBA8: roughness/metallic/emissive/flags
    u32 valid_texels{ 0 };     // 审计计数
    u32 hits_total{ 0 };       // 审计计数（= valid_texels，冗余给未来的多层扩展）
};

struct MeshBakeResult
{
    u64 geometry_digest{ 0 };
    u64 layout_digest{ 0 };
    u64 material_digest{ 0 };
    u32 name_hash{ 0 };
    f32 depth_limit_scale{ graphics::rhi::card_surface_identity::CaptureFarScale };
    std::vector<CardPage> pages;
};

struct BakeParams
{
    // 卡片分辨率公式与运行时一致：max(e[u],e[v])·2·texels_per_unit 钳
    // [64,128] 页对齐 32。页大小 32 = SurfaceCachePass 的 page_size 默认值；
    // SC_MIN_CARD_RES 默认 64。texels_per_unit 与运行时分配密度
    //（PRIMAL_SC_TEXELS_PER_UNIT，默认 64）必须同值——[P3 单变量候选]。
    u32 page_size{ 32 };
    u32 min_resolution{ 64 };
    u32 max_resolution{ 128 };
    f32 texels_per_unit{ 64.f };
    // albedo = sRGB 纹理 × tint × albedo_scale（全局采样比例，不等于修改源纹理字节）。
    f32 albedo_scale{ 1.f };
    f32 tint[3]{ 1.f, 1.f, 1.f };
    // 材质参数占位（MaterialInstance 默认：roughness 0.5 / metallic 0）。
    u8 roughness{ 128 };
    u8 metallic{ 0 };
    // 射线最大深度 = max(extent)·2·depth_limit_scale（卡上表面可能深于卡厚）。
    f32 depth_limit_scale{ graphics::rhi::card_surface_identity::CaptureFarScale };
    // 每卡页 log（0=静默 1=逐卡一行）。
    u32 verbosity{ 0 };
};

// 对单个 mesh 烘焙全部卡片页。albedo 无效 → tint×scale 纯色。
// cards.cards 为空 → 返回 false（无卡可烘，调用方跳过）。
bool bake_mesh(const MeshGeometryView& geom, const card_bake::data& cards,
               const RgbaImage& albedo, const BakeParams& params, MeshBakeResult& out);

// Supplementary samples keep their true mesh-local position; they are NOT atlas
// texels and must never be inserted into SCS1 at a different texel position.
// Normal is unflipped (the consumer must orient it for its selected card).
// This CPU result does not certify visibility or contain evaluated lighting.
using BoundarySample = graphics::rhi::card_boundary::Sample;
using BoundaryBakeResult = graphics::rhi::card_boundary::Mesh;

// Explicit owner-local TriangleIDs only. Sort/deduplicate requests, sample each
// edge at <= spacing mesh units, include each corner once per primitive (do NOT
// weld different primitives/material seams). Budget overflow or malformed input
// fails with an empty result, never silently truncates a triangle's boundary.
// Optional persistence uses card_boundary::Write/Read (SCB1); no runtime/cache
// activation is implied by this baking API.
bool bake_boundary_samples(const MeshGeometryView& geom, const card_bake::data& cards,
                           const RgbaImage& albedo, const BakeParams& params,
                           const std::vector<u32>& triangle_ids, f32 spacing,
                           u32 max_samples, BoundaryBakeResult& out);

// ---- SCS1 sidecar 文件 ----
// 头（96 B）| MeshRecord × N（各带 CardPageEntry 数组）| 页平面数据
// 所有标量 little-endian memcpy 序列化。运行时读者逐字段锁定。
constexpr u32 kScsMagic{ 0x31534353 };   // "SCS1"
constexpr u32 kScsVersion{ 4 };
constexpr u32 kScsHeaderBytes{ 96 };

struct ScsHeader
{
    u32 magic{ kScsMagic };
    u32 version{ kScsVersion };
    u32 header_bytes{ kScsHeaderBytes };
    u32 flags{ 0 };                // bit0 = params, bit1 = TriangleID plane (v4 requires both)
    f32 albedo_scale{ 1.f };
    f32 texels_per_unit{ 64.f };
    u32 mesh_count{ 0 };
    u32 page_size{ 32 };
    u32 min_resolution{ 64 };
    u32 max_resolution{ 128 };
    f32 capture_near{ graphics::rhi::card_surface_identity::CaptureNear };
    f32 capture_far_scale{ graphics::rhi::card_surface_identity::CaptureFarScale };
    f32 capture_far_bias{ graphics::rhi::card_surface_identity::CaptureFarBias };
    u32 reserved[11]{};
};
static_assert(sizeof(ScsHeader) == kScsHeaderBytes, "SCS1 header must be 96 B");

struct ScsCardPageEntry
{
    u32 card_index;
    u32 resolution;
    u64 albedo_offset;   // 文件绝对偏移
    u64 normal_offset;
    u64 depth_offset;
    u64 params_offset;
    u64 triangle_offset;
};
static_assert(sizeof(ScsCardPageEntry) == 48, "SCS1 page entry must be 48 B");

struct ScsMeshRecord
{
    u64 geometry_digest;
    u64 layout_digest;
    u64 material_digest;
    u32 name_hash;
    u32 card_count;
    u64 pages_offset;
    u64 pages_bytes;
    // 后随 card_count × ScsCardPageEntry
};
static_assert(sizeof(ScsMeshRecord) == 48, "SCS1 mesh record must be 48 B");

// 把 bake_mesh 结果写成 sidecar。多 mesh 顺序追加（页数据集中写在文件尾）。
bool write_sidecar(const char* path, const ScsHeader& header,
                   const std::vector<MeshBakeResult>& meshes);

// 运行时/审计共用的读取入口：返回 header + 每 mesh 的记录与页条目
//（页数据按需 fread，不在此加载）。
bool read_sidecar_header(const char* path, ScsHeader& out_header);
bool read_sidecar_records(const char* path, const ScsHeader& header,
                          std::vector<ScsMeshRecord>& out_meshes,
                          std::vector<std::vector<ScsCardPageEntry>>& out_pages,
                          std::vector<u64>* out_record_file_offsets = nullptr);

}  // namespace primal::tools::card_surface
