// ResourceAPI.cpp - Handle-based wrappers around engine camera/render_item/light_set APIs.
// All handles are u32 (camera) or u64 (render_item, light_set). Invalid = 0 or ~0u.
#include "Common.h"
#include "CommonHeaders.h"
#include "../Graphics/Renderer.h"
#include "../Graphics/EditorShaders.h"
#include "../Content/ContentToEngine.h"
// R13-B（Windows 十二测 MetalCommonHeaders.h:8 C1083）：MetalContent.h 无条件
// include 拉入 <Metal/Metal.hpp>（仅 Apple 存在）。Metal 后端只在 APPLE 编译
// （Engine/CMakeLists.txt 仅 APPLE 分支 glob Graphics/Metal），本 TU 的两处
// metal::content::submesh::add 调用点同门控、非 Apple fail-closed（见下）。
#if defined(__APPLE__)
#include "../Graphics/Metal/MetalContent.h"
#endif

#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

// Cannot `using namespace primal;` at file scope because MetalContent.h pulls
// in Renderer.h which adds a nested primal::graphics::id namespace; that
// makes bare `id_eng::invalid_id` ambiguous. Use fully qualified names instead.
// using namespace primal;

// Objective-C's `id` type (from Metal/Metal.hpp via objc_object) further
// shadows any `id_eng::` access at file scope. Use the alias `id_eng::` for the
// engine's id namespace throughout this file.
namespace id_eng = ::primal::id;

EDITOR_INTERFACE u32 CreateCamera(u32 entity_id, f32 fov, f32 aspect, f32 near_z, f32 far_z)
{
    std::fprintf(stderr, "[CreateCamera] entry: entity_id=0x%x fov=%.3f aspect=%.3f near=%.3f far=%.3f\n",
                 entity_id, fov, aspect, near_z, far_z);
    if (entity_id == id_eng::invalid_id) {
        std::fprintf(stderr, "[CreateCamera] invalid entity_id\n");
        return id_eng::invalid_id;
    }
    if (fov <= 0.f || aspect <= 0.f || near_z <= 0.f || far_z <= near_z) {
        std::fprintf(stderr, "[CreateCamera] invalid params (fov=%.3f aspect=%.3f near=%.3f far=%.3f)\n",
                     fov, aspect, near_z, far_z);
        return id_eng::invalid_id;
    }

    primal::graphics::perspective_camera_init_info info{ entity_id };
    info.field_of_view = fov;
    info.aspect_ratio = aspect;
    info.near_z = near_z;
    info.far_z = far_z;

    primal::graphics::camera cam = primal::graphics::create_camera(info);
    auto cid = static_cast<u32>(cam.get_id());
    std::fprintf(stderr, "[CreateCamera] result cam.get_id()=0x%x is_valid=%d\n",
                 cid, cam.is_valid() ? 1 : 0);
    return cid;
}

EDITOR_INTERFACE void RemoveCamera(u32 cam_id)
{
    if (cam_id == id_eng::invalid_id) return;
    primal::graphics::remove_camera(primal::graphics::camera_id{ cam_id });
}

EDITOR_INTERFACE u64 AddRenderItem(u32 entity_id, u64 geometry_content_id, u32 material_count, const u64* material_ids)
{
    if (entity_id == id_eng::invalid_id) {
        std::fprintf(stderr, "[AddRenderItem] invalid entity_id\n");
        return id_eng::invalid_id;
    }
    if (geometry_content_id == id_eng::invalid_id) {
        std::fprintf(stderr, "[AddRenderItem] invalid geometry_content_id\n");
        return id_eng::invalid_id;
    }
    if (material_count > 0 && !material_ids) {
        std::fprintf(stderr, "[AddRenderItem] material_count>0 but material_ids is null\n");
        return id_eng::invalid_id;
    }

    id_eng::id_type result = primal::graphics::add_render_item(
        entity_id,
        static_cast<id_eng::id_type>(geometry_content_id),
        material_count,
        reinterpret_cast<const id_eng::id_type* const>(material_ids));

    return static_cast<u64>(result);
}

EDITOR_INTERFACE void RemoveRenderItem(u64 render_item_id)
{
    if (render_item_id == id_eng::invalid_id) return;
    primal::graphics::remove_render_item(static_cast<id_eng::id_type>(render_item_id));
}

// --- Light sets ---

namespace {
std::atomic<u64> g_light_set_counter{ 1 };
}

EDITOR_INTERFACE u64 CreateLightSet()
{
    u64 key = g_light_set_counter.fetch_add(1, std::memory_order_relaxed);
    primal::graphics::create_light_set(key);
    return key;
}

EDITOR_INTERFACE void DestroyLightSet(u64 key)
{
    if (key == 0) return;
    primal::graphics::remove_light_set(key);
}

// --- Default material (lazy-compiled sphere VS+FS + opauqe material) ---
//
// This closes the API gap that previously blocked Phase 7 of the Avalonia
// migration: AddRenderItem requires material_count>0 with at least one valid
// material_id, but the C ABI had no CreateMaterial export. Without this
// function the C# test scene can only create entities + lights, never
// geometry.
//
// On first call we shell out to `xcrun metal` to compile the inline MSL
// strings in Engine/Graphics/EditorShaders.h into a metallib, then wrap
// each compiled blob in the legacy `compiled_shader` format and feed it
// into primal::content::add_shader_group. The resulting group_id is cached
// statically and a material_init_info is constructed with
//   shader_ids[vertex] = group_id
//   shader_ids[pixel]  = group_id
//   texture_count      = 0
// matching the layout lookup that create_pso() performs in
// Engine/Graphics/Metal/MetalContent.cpp.
//
// The vertex shader's [[attribute(N)]] indices must match the interleave
// layout produced by Engine/Content/ProceduralMesh.h::create_sphere_mesh
// (pos12 + normal12 + uv8 = 32B stride). See EditorShaders.h for the
// contract.
//
// RegisterProceduralMeshSubmesh closes a *third* API gap: ProceduralMeshCreate
// only registers the geometry with primal::content::register_mesh_asset
// (rhi_mesh_assets + geometry_hierarchies), but the legacy AddRenderItem
// path also requires a submesh entry in MetalContent::submesh_views /
// submesh_buffers. Without that registration submesh::get_views dereferences
// a null entry in submesh_views and the first AddRenderItem call
// segfaults. We build the legacy submesh::add binary blob here from the
// already-registered RHIMeshAsset and feed it to the legacy submesh::add,
// which populates both submesh_buffers and submesh_views. The returned
// submesh id is stable for the lifetime of the geometry (we re-add on
// every call and the free_list hands out new slots; in practice the C#
// test scene calls this once at startup so there's no leak).

namespace {

// Compiles an inline MSL source string into a metallib blob by writing it
// to a temp .metal file, invoking `xcrun metal -c` then `xcrun metallib`,
// and reading the result. Returns empty vector on failure (logged to
// stderr). The compile step is intentionally a separate function from the
// shader cache so that tests can inject pre-compiled metallib bytes.
std::vector<u8> compile_msl_to_metallib(const char* source, const char* tag)
{
    namespace fs = std::filesystem;
    fs::path base = fs::temp_directory_path() / "primal_editor_shaders";
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) {
        std::fprintf(stderr, "[CreateDefaultMaterial] failed to create %s: %s\n",
                     base.string().c_str(), ec.message().c_str());
        return {};
    }

    const auto metal_path = base / (std::string(tag) + ".metal");
    const auto air_path   = base / (std::string(tag) + ".air");
    const auto lib_path   = base / (std::string(tag) + ".metallib");

    {
        std::ofstream f(metal_path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "[CreateDefaultMaterial] cannot write %s\n", metal_path.string().c_str());
            return {};
        }
        f.write(source, std::strlen(source));
    }

    const std::string compile_cmd =
        std::string("xcrun -sdk macosx metal -c \"") + metal_path.string() +
        "\" -o \"" + air_path.string() + "\" -Wno-unused-variable 2>&1";
    if (std::system(compile_cmd.c_str()) != 0) {
        std::fprintf(stderr, "[CreateDefaultMaterial] xcrun metal -c failed for %s\n", tag);
        return {};
    }

    const std::string lib_cmd =
        std::string("xcrun -sdk macosx metallib -o \"") + lib_path.string() +
        "\" \"" + air_path.string() + "\" 2>&1";
    if (std::system(lib_cmd.c_str()) != 0) {
        std::fprintf(stderr, "[CreateDefaultMaterial] xcrun metallib failed for %s\n", tag);
        return {};
    }

    std::ifstream f(lib_path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "[CreateDefaultMaterial] cannot read %s\n", lib_path.string().c_str());
        return {};
    }
    const std::streamsize size = f.tellg();
    if (size <= 0) {
        std::fprintf(stderr, "[CreateDefaultMaterial] %s is empty\n", lib_path.string().c_str());
        return {};
    }
    std::vector<u8> out(static_cast<std::size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), size);
    return out;
}

// Wraps a metallib blob in the legacy `compiled_shader` format expected
// by primal::content::add_shader / primal::content::add_shader_group:
//   [u64 byte_code_size][u8 hash[16]][u8 byte_code...]
// The hash is intentionally left zeroed — the engine never validates it
// for compile correctness, only for cache identity on hot-reload which
// the test scene doesn't exercise.
std::vector<u8> wrap_as_compiled_shader(const std::vector<u8>& metallib)
{
    using compiled_shader = primal::content::compiled_shader;
    const u64 size = metallib.size();
    std::vector<u8> out(sizeof(u64) + compiled_shader::hash_length + size);
    std::memcpy(out.data(), &size, sizeof(u64));
    std::memset(out.data() + sizeof(u64), 0, compiled_shader::hash_length);
    std::memcpy(out.data() + sizeof(u64) + compiled_shader::hash_length,
                metallib.data(), size);
    return out;
}

struct default_material_cache {
    u64 vs_group_id{id_eng::invalid_id};
    u64 ps_group_id{id_eng::invalid_id};
    u64 material_id{id_eng::invalid_id};
    bool compile_failed{false};
};

default_material_cache g_default_material;

// [PassDebugView 排查] 原 std::call_once 实现：编辑器项目自动加载
// （PRIMAL_OPEN_PROJECT）会早于 viewport/引擎 legacy 初始化触发首次
// CreateDefaultMaterial，此时 gfx.resources.add_material 尚为 NULL，
// 失败结果被 call_once 永久缓存 → 引擎就绪后的所有重试都返回 invalid
// → materialID 无效 → meshlet PS material_words 越界读零 → albedo 全黑。
// 改为"成功才落缓存"的可重试实现。
void build_default_material_once()
{
    static std::mutex s_mutex;
    static bool s_done = false;
    std::lock_guard lock{ s_mutex };
    if (s_done) return;

    auto vs_metallib = compile_msl_to_metallib(
        primal::editor_shaders::kEditorSphereVertexShader, "primal_sphere_vs");
    auto ps_metallib = compile_msl_to_metallib(
        primal::editor_shaders::kEditorSpherePixelShader, "primal_sphere_ps");
    if (vs_metallib.empty() || ps_metallib.empty()) {
        g_default_material.compile_failed = true;
        return; // 下次调用重试（xcrun 失败可能是暂态）
    }
    g_default_material.compile_failed = false;
    auto vs_blob = wrap_as_compiled_shader(vs_metallib);
    auto ps_blob = wrap_as_compiled_shader(ps_metallib);

    // We split vertex and pixel into two separate shader_groups because
    // Engine/Graphics/Metal/MetalContent.cpp's create_pso looks up the
    // entry-point function name per group via
    // primal::content::get_shader_function_name, and that API returns a single
    // name per group — vertex and pixel obviously have different
    // entry points (sphere_vs vs sphere_ps) so a single group would
    // resolve both to whichever name was registered last. Two groups
    // also let create_pso apply the vertex-only elements_type key
    // (static_normal_texture = 3) to the VS group while the PS group
    // uses the u32_invalid_id sentinel that create_pso expects for
    // non-vertex shader types.
    const u8* vs_ptr = vs_blob.data();
    const u32 vs_key = 3u; // static_normal_texture
    g_default_material.vs_group_id = primal::content::add_shader_group(
        &vs_ptr, 1, &vs_key);
    if (g_default_material.vs_group_id == id_eng::invalid_id) {
        g_default_material.compile_failed = true;
        return;
    }
    primal::content::add_shader_function_name(
        static_cast<id_eng::id_type>(g_default_material.vs_group_id), "sphere_vs");

    const u8* ps_ptr = ps_blob.data();
    const u32 ps_key = 0xFFFFFFFFu; // u32_invalid_id sentinel
    g_default_material.ps_group_id = primal::content::add_shader_group(
        &ps_ptr, 1, &ps_key);
    if (g_default_material.ps_group_id == id_eng::invalid_id) {
        g_default_material.compile_failed = true;
        return;
    }
    primal::content::add_shader_function_name(
        static_cast<id_eng::id_type>(g_default_material.ps_group_id), "sphere_ps");

    primal::graphics::material_init_info info{};
    info.type = primal::graphics::material_type::opauqe;
    info.texture_count = 0;
    info.texture_ids = nullptr;
    // shader_ids[] layout: bit 0 = vertex, bit 1 = pixel (others
    // unused). create_pso iterates these in shader_type enum order so
    // [vertex] must come first.
    for (u32 i = 0; i < primal::graphics::shader_type::count; ++i) {
        info.shader_ids[i] = id_eng::invalid_id;
    }
    info.shader_ids[primal::graphics::shader_type::vertex] =
        static_cast<id_eng::id_type>(g_default_material.vs_group_id);
    info.shader_ids[primal::graphics::shader_type::pixel] =
        static_cast<id_eng::id_type>(g_default_material.ps_group_id);

    g_default_material.material_id =
        static_cast<u64>(primal::graphics::add_material(info));

    // 只有成功才落缓存；legacy gfx 未就绪时（add_material 返回 invalid）
    // 留待下次调用重试。
    if (g_default_material.material_id != id_eng::invalid_id) {
        s_done = true;
    }
}

} // namespace

EDITOR_INTERFACE u64 CreateDefaultMaterial()
{
    build_default_material_once();
    if (g_default_material.compile_failed) {
        std::fprintf(stderr, "[CreateDefaultMaterial] compile failed; returning invalid\n");
        return id_eng::invalid_id;
    }
    if (g_default_material.material_id == id_eng::invalid_id) {
        std::fprintf(stderr, "[CreateDefaultMaterial] add_material returned invalid\n");
        return id_eng::invalid_id;
    }
    return g_default_material.material_id;
}

// Register the submesh (vertex/index buffers + element metadata) for a
// geometry previously created via ProceduralMeshCreate. Idempotent only
// at the call-site level — every call allocates a new submesh slot; the
// C# test scene calls this once at startup so the leak is bounded to
// startup.
//
// The legacy submesh::add takes a binary blob:
//   u32 element_size
//   u32 vertex_count
//   u32 index_count
//   u32 elements_type
//   u32 primitive_topology
//   u8  positions[12 * vertex_count]
//   u8  elements[element_size * vertex_count]
//   u8  indices[4 * index_count]
// We reconstruct that from the RHIMeshAsset that ProceduralMeshCreate
// already registered. The element_size / elements_type / primitive_topology
// values must match what Engine/Content/ProceduralMesh.h wrote (PROC_ELEM_STRIDE,
// PROC_ELEMENTS_TYPE, triangle_list).
//
// After registering the submesh view, we ALSO insert a fake_pointer entry
// into content::geometry_hierarchies with the single_mesh_marker bit set.
// This is how the legacy add_render_item path looks up submesh GPU ids —
// content::get_submesh_gpu_ids dereferences the geometry_content_id slot
// and decodes the pointer. Without this, add_render_item would crash when
// trying to render this geometry.
//
// Returns: the NEW legacy geometry_content_id (NOT the submesh view id).
// Pass this id to AddRenderItem.
EDITOR_INTERFACE u64 RegisterProceduralMeshSubmesh(u64 rhi_geometry_content_id)
{
    if (rhi_geometry_content_id == id_eng::invalid_id) {
        std::fprintf(stderr, "[RegisterProceduralMeshSubmesh] invalid rhi geometry_id\n");
        return id_eng::invalid_id;
    }

    const id_eng::id_type rhi_id = primal::content::get_rhi_mesh_id(
        static_cast<id_eng::id_type>(rhi_geometry_content_id));
    if (rhi_id == id_eng::invalid_id) {
        std::fprintf(stderr, "[RegisterProceduralMeshSubmesh] get_rhi_mesh_id returned invalid\n");
        return id_eng::invalid_id;
    }

    primal::graphics::rhi::RHIMeshAsset asset;
    if (!primal::content::get_rhi_mesh_asset(rhi_id, asset)) {
        std::fprintf(stderr, "[RegisterProceduralMeshSubmesh] get_rhi_mesh_asset failed for rhi_id=%u\n",
                     static_cast<u32>(rhi_id));
        return id_eng::invalid_id;
    }
    if (asset.num_vertices == 0 || asset.position_buffer.empty() || asset.index_buffer.empty()) {
        std::fprintf(stderr, "[RegisterProceduralMeshSubmesh] empty asset (verts=%u pos=%zu idx=%zu)\n",
                     asset.num_vertices, asset.position_buffer.size(), asset.index_buffer.size());
        return id_eng::invalid_id;
    }

    // PROC_ELEMENTS_TYPE = 0x03 (static_normal_texture) from
    // Engine/Content/ProceduralMesh.h; triangle_list = 4 from
    // Engine/Graphics/Renderer.h::primitive_topology. element_size = 20B
    // (normal 12 + uv 8) from PROC_ELEM_STRIDE.
    constexpr u32 element_size = 20u;
    constexpr u32 elements_type = 0x03u;
    constexpr u32 primitive_topology = 4u; // triangle_list
    const u32 vertex_count = asset.num_vertices;
    const u32 index_count = asset.num_indices;
    const u32 index_size = sizeof(u32);

    // 5 header u32 + 12*verts + 20*verts + 4*indices. The legacy blob has
    // no trailing alignment padding documented, but submesh::add does
    // math::align_size_up<4> on position+element buffer sizes, so we
    // don't need to pad.
    const u64 total = sizeof(u32) * 5
        + static_cast<u64>(12) * vertex_count
        + static_cast<u64>(element_size) * vertex_count
        + static_cast<u64>(index_size) * index_count;
    std::vector<u8> blob;
    blob.resize(static_cast<std::size_t>(total));

    u8* p = blob.data();
    auto write_u32 = [&](u32 v) {
        std::memcpy(p, &v, sizeof(u32));
        p += sizeof(u32);
    };
    write_u32(element_size);
    write_u32(vertex_count);
    write_u32(index_count);
    write_u32(elements_type);
    write_u32(primitive_topology);
    std::memcpy(p, asset.position_buffer.data(),
                static_cast<std::size_t>(12) * vertex_count);
    p += 12 * vertex_count;
    if (!asset.element_buffer.empty()) {
        std::memcpy(p, asset.element_buffer.data(),
                    static_cast<std::size_t>(element_size) * vertex_count);
        p += element_size * vertex_count;
    }
    std::memcpy(p, asset.index_buffer.data(),
                static_cast<std::size_t>(index_size) * index_count);

    const u8* blob_data = blob.data();
#if defined(__APPLE__)
    const id_eng::id_type submesh_view_id =
        primal::graphics::metal::content::submesh::add(blob_data);
    if (submesh_view_id == id_eng::invalid_id) {
        std::fprintf(stderr, "[RegisterProceduralMeshSubmesh] submesh::add failed\n");
        return id_eng::invalid_id;
    }

    // Mirror ContentToEngine.cpp::create_single_submesh: pack the
    // submesh_view_id into a fake pointer tagged with single_mesh_marker,
    // then add it to geometry_hierarchies so legacy add_render_item can
    // resolve the geometry_content_id back to this submesh's GPU buffer.
    // This is the bridge from RHI-registered mesh -> legacy render path.
    constexpr uintptr_t single_mesh_marker{ (uintptr_t)0x01 };
    constexpr u8 shift_bits{ (sizeof(uintptr_t) - sizeof(id_eng::id_type)) << 3 };
    u8 *const fake_pointer{ (u8 *const)((((uintptr_t)submesh_view_id) << shift_bits) | single_mesh_marker) };

    // Add a new entry to geometry_hierarchies with the fake pointer.
    // The function is defined in Engine/Content/ContentToEngine.cpp inside
    // the primal::content namespace; we call it through that namespace.
    const id_eng::id_type new_geometry_id =
        primal::content::add_legacy_geometry_hierarchy(fake_pointer);
    if (new_geometry_id == id_eng::invalid_id) {
        std::fprintf(stderr, "[RegisterProceduralMeshSubmesh] add_legacy_geometry_hierarchy failed\n");
        return id_eng::invalid_id;
    }

    return static_cast<u64>(new_geometry_id);
#else
    // R13-B：非 Apple 编译域无 Metal submesh 注册器（D3D12/Vulkan 后端不经此
    // 桥），fail-closed 返回 invalid_id；导出面保留（C# 桥按名 P/Invoke）。
    // 尾段 fake_pointer 桥接编码 submesh_view_id（Apple 门内变量），一并入门。
    (void)blob_data;
    std::fprintf(stderr, "[RegisterProceduralMeshSubmesh] metal submesh registry unavailable on this platform\n");
    return id_eng::invalid_id;
#endif // defined(__APPLE__)
}

// Create a new sphere mesh with its vertices translated by (offset_x, offset_y,
// offset_z) in world space, and register it as a legacy submesh + geometry.
//
// This is a workaround for the engine's per-entity World matrix propagation
// being broken on this system (constant_buffer::offset() returns corrupt byte
// offsets for per-object data). By baking the world position into the mesh
// vertices, the shader's World matrix becomes identity and the spheres appear
// at distinct screen positions.
//
// Returns: legacy geometry_content_id (use with AddRenderItem), or invalid_id
// on failure.
EDITOR_INTERFACE u64 CreateTranslatedProceduralMesh(u64 rhi_geometry_content_id,
                                                    f32 offset_x, f32 offset_y, f32 offset_z)
{
    if (rhi_geometry_content_id == id_eng::invalid_id) {
        std::fprintf(stderr, "[CreateTranslatedProceduralMesh] invalid rhi geometry_id\n");
        return id_eng::invalid_id;
    }

    const id_eng::id_type rhi_id = primal::content::get_rhi_mesh_id(
        static_cast<id_eng::id_type>(rhi_geometry_content_id));
    if (rhi_id == id_eng::invalid_id) {
        std::fprintf(stderr, "[CreateTranslatedProceduralMesh] get_rhi_mesh_id returned invalid\n");
        return id_eng::invalid_id;
    }

    primal::graphics::rhi::RHIMeshAsset asset;
    if (!primal::content::get_rhi_mesh_asset(rhi_id, asset)) {
        std::fprintf(stderr, "[CreateTranslatedProceduralMesh] get_rhi_mesh_asset failed\n");
        return id_eng::invalid_id;
    }
    if (asset.num_vertices == 0 || asset.position_buffer.empty() || asset.index_buffer.empty()) {
        std::fprintf(stderr, "[CreateTranslatedProceduralMesh] empty asset\n");
        return id_eng::invalid_id;
    }

    // Translate the position buffer in place.
    const u32 vertex_count = asset.num_vertices;
    if (asset.position_buffer.size() < (size_t)vertex_count * 12) {
        std::fprintf(stderr, "[CreateTranslatedProceduralMesh] position buffer too small\n");
        return id_eng::invalid_id;
    }
    f32* pos = reinterpret_cast<f32*>(asset.position_buffer.data());
    for (u32 i = 0; i < vertex_count; ++i) {
        pos[i * 3 + 0] += offset_x;
        pos[i * 3 + 1] += offset_y;
        pos[i * 3 + 2] += offset_z;
    }

    // Build the legacy submesh blob: 5 header u32 + positions + elements + indices.
    constexpr u32 element_size = 20u;
    constexpr u32 elements_type = 0x03u;
    constexpr u32 primitive_topology = 4u;
    const u32 index_count = asset.num_indices;
    const u32 index_size = sizeof(u32);

    const u64 total = sizeof(u32) * 5
        + static_cast<u64>(12) * vertex_count
        + static_cast<u64>(element_size) * vertex_count
        + static_cast<u64>(index_size) * index_count;
    std::vector<u8> blob;
    blob.resize(static_cast<std::size_t>(total));

    u8* p = blob.data();
    auto write_u32 = [&](u32 v) {
        std::memcpy(p, &v, sizeof(u32));
        p += sizeof(u32);
    };
    write_u32(element_size);
    write_u32(vertex_count);
    write_u32(index_count);
    write_u32(elements_type);
    write_u32(primitive_topology);
    std::memcpy(p, asset.position_buffer.data(),
                static_cast<std::size_t>(12) * vertex_count);
    p += 12 * vertex_count;
    if (!asset.element_buffer.empty()) {
        std::memcpy(p, asset.element_buffer.data(),
                    static_cast<std::size_t>(element_size) * vertex_count);
        p += element_size * vertex_count;
    }
    std::memcpy(p, asset.index_buffer.data(),
                static_cast<std::size_t>(index_size) * index_count);

    const u8* blob_data = blob.data();
#if defined(__APPLE__)
    const id_eng::id_type submesh_view_id =
        primal::graphics::metal::content::submesh::add(blob_data);
    if (submesh_view_id == id_eng::invalid_id) {
        std::fprintf(stderr, "[CreateTranslatedProceduralMesh] submesh::add failed\n");
        return id_eng::invalid_id;
    }

    // Bridge RHI -> legacy add_render_item via fake_pointer with single_mesh_marker
    // encoding the submesh_view_id (NOT the rhi_id — we want the legacy
    // submesh_views slot, not the rhi_mesh_assets slot).
    constexpr uintptr_t single_mesh_marker{ (uintptr_t)0x01 };
    constexpr u8 shift_bits{ (sizeof(uintptr_t) - sizeof(id_eng::id_type)) << 3 };
    u8 *const fake_pointer{ (u8 *const)((((uintptr_t)submesh_view_id) << shift_bits) | single_mesh_marker) };
    const id_eng::id_type new_geometry_id = primal::content::add_legacy_geometry_hierarchy(fake_pointer);
    if (new_geometry_id == id_eng::invalid_id) {
        std::fprintf(stderr, "[CreateTranslatedProceduralMesh] add_legacy_geometry_hierarchy failed\n");
        return id_eng::invalid_id;
    }

    return static_cast<u64>(new_geometry_id);
#else
    // R13-B：同 RegisterProceduralMeshSubmesh——非 Apple fail-closed（见上注，
    // fake_pointer 尾段编码 submesh_view_id，一并入门）。
    (void)blob_data;
    std::fprintf(stderr, "[CreateTranslatedProceduralMesh] metal submesh registry unavailable on this platform\n");
    return id_eng::invalid_id;
#endif // defined(__APPLE__)
}
