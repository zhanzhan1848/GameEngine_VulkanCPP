// 文件说明: ContentTools 模型格式 ↔ 引擎解析端到端回归测试。
// 按 ContentTools/Geometry.cpp pack_mesh_data（新管线 SceneBlobWriter::Serialize
// 与其逐字节一致）的布局手工构造 blob，走 content::create_resource 真实解析路径，
// 校验注册资产字段。重点锁两份契约：
//   1. meshlet wire 结构 60 字节（RHIMeshlet 为 64B，读侧逐字段转换）；
//   2. 子网格段完整跳过（header + 3 buffers + MSHL + SDF），多子网格定位精准。
// 写端改动（pack_mesh_data / Serialize / pack_geometry.py）若破坏此契约，本测试先于
// 运行时崩溃报错。
#include "TestFramework.h"
#include "Content/ContentToEngine.h"
#include "Common/CommonHeaders.h"
#include <cstring>
#include <iostream>
#include <vector>

using namespace primal;
using Engine::Test::TestResult;

namespace {

// Byte-exact mirror of ContentTools mesh::meshlet (ContentTools/Geometry.h) as
// written by pack_mesh_data and SceneBlobWriter::Serialize. Keep in sync with
// meshlet_wire in Engine/Content/ContentToEngine.cpp.
struct MeshletWire {
    u32 vertex_offset, triangle_offset, vertex_count, triangle_count;
    f32 cone_apex[3], cone_axis[3], cone_cutoff, center[3], radius;
};
static_assert(sizeof(MeshletWire) == 60, "ContentTools mesh::meshlet wire size");

std::vector<u8> g_buf;
void w32(u32 v) { g_buf.insert(g_buf.end(), (u8*)&v, (u8*)&v + 4); }
void wf(f32 v) { w32(*(u32*)&v); }
void wbytes(const void* p, size_t n) {
    const u8* b = (const u8*)p;
    g_buf.insert(g_buf.end(), b, b + n);
}

// Serialize one submesh in the exact pack_mesh_data field order
// (Geometry.cpp:661-745). SDF grid resolution is 2 (res³ = 8).
void WriteSubmesh(const char* name, u32 num_vertices, u32 num_indices,
                  u32 elem_size, u32 elements_type, u32 meshlet_count) {
    w32((u32)strlen(name)); wbytes(name, strlen(name));
    w32(0);                                     // lod_id
    w32(7);                                     // material_idx
    w32(elem_size);                             // elements_size
    w32(elements_type);                         // elements_type
    w32(num_vertices);
    w32(num_vertices < (1 << 16) ? 2u : 4u);    // index_size
    w32(num_indices);
    wf(0.5f);                                   // lod_threshold
    std::vector<f32> pos(num_vertices * 3);
    for (u32 i = 0; i < pos.size(); ++i) pos[i] = (f32)i * 0.25f;
    wbytes(pos.data(), pos.size() * 4);         // position buffer (12B/vertex)
    std::vector<u8> elems(elem_size * num_vertices, 0xAB);
    wbytes(elems.data(), elems.size());         // element buffer
    std::vector<u16> idx(num_indices);
    for (u32 i = 0; i < num_indices; ++i) idx[i] = (u16)(i % num_vertices);
    wbytes(idx.data(), idx.size() * 2);         // index buffer (u16)

    w32(0x4C48534D);                            // "MSHL"
    w32(meshlet_count);
    for (u32 m = 0; m < meshlet_count; ++m) {
        MeshletWire ml{};
        ml.vertex_offset = m * 3 + 11;
        ml.triangle_offset = m * 5 + 22;
        ml.vertex_count = 3 + m;
        ml.triangle_count = 1 + m;
        ml.cone_apex[0] = 1.5f * (m + 1);
        ml.cone_axis[1] = -0.5f;
        ml.cone_cutoff = 0.75f;
        ml.center[2] = 2.25f;
        ml.radius = 3.5f + m;
        wbytes(&ml, sizeof(ml));
    }
    u32 mv = meshlet_count * 3;
    w32(mv);
    std::vector<u32> mvv(mv);
    for (u32 i = 0; i < mv; ++i) mvv[i] = i * 7;
    wbytes(mvv.data(), mv * 4);
    u32 mt = meshlet_count * 3;
    w32(mt);
    std::vector<u8> mtt(mt, 3);
    wbytes(mtt.data(), mt);

    w32(0x20464453);                            // "SDF "
    u32 res[3] = {2, 2, 2}; wbytes(res, 12);
    f32 bmin[3] = {-1, -1, -1}, bmax[3] = {1, 1, 1};
    wbytes(bmin, 12); wbytes(bmax, 12);
    w32(8);  std::vector<u16> sdf(8, 1234);  wbytes(sdf.data(), 16);
    w32(8);  std::vector<u8>  vox(8, 255);   wbytes(vox.data(), 8);
    w32(32); std::vector<u16> vf(32, 777);   wbytes(vf.data(), 64);
}

// Geometry wrapper expected by create_resource:
// [lod_count][per-LOD: threshold, submesh_count, size_of_submeshes][submeshes...]
// All submeshes go into one LOD (single-LOD layout used by the engine converters).
std::vector<u8> WrapGeometry(std::vector<std::vector<u8>> submeshes) {
    size_t total = 0;
    for (auto& s : submeshes) total += s.size();
    g_buf.clear();
    w32(1);                                 // lod_count
    wf(0.5f);                               // lod threshold
    w32((u32)submeshes.size());             // submesh_count
    w32((u32)total);                        // size_of_submeshes
    std::vector<u8> blob = g_buf;
    for (auto& s : submeshes) blob.insert(blob.end(), s.begin(), s.end());
    return blob;
}

std::vector<u8> MakeSubmesh(const char* name, u32 v, u32 i, u32 es, u32 et, u32 ml) {
    g_buf.clear();
    WriteSubmesh(name, v, i, es, et, ml);
    return g_buf;
}

} // namespace

class TestContentMeshFormatRoundTrip {
public:
    // Multi-submesh single-LOD: exercises skip_mesh_in_blob (hierarchy sizing
    // + walk) and parse_mesh_to_asset on both submeshes, including the second
    // meshlet of mesh A (proves the 60-byte stride stays in sync).
    TestResult MultiSubmesh_Hierarchy_Parse() {
        std::vector<u8> blob = WrapGeometry({
            MakeSubmesh("meshA", 8, 24, 20, 3 /*static_normal_texture*/, 2),
            MakeSubmesh("meshB", 4, 12, 8, 1 /*static_normal*/, 1),
        });

        id::id_type geom = content::create_resource(blob.data(), content::asset_type::mesh);
        if (!id::is_valid(geom)) return TestResult::Failed;

        id::id_type subs[2] = {id::invalid_id, id::invalid_id};
        content::get_submesh_gpu_ids(geom, 2, subs);
        if (!id::is_valid(subs[0]) || !id::is_valid(subs[1])) return TestResult::Failed;

        graphics::rhi::RHIMeshAsset a{};
        if (!content::get_rhi_mesh_asset(subs[1], a)) return TestResult::Failed;
        if (a.num_vertices != 4 || a.num_indices != 12) return TestResult::Failed;
        if (a.index_size != 2 || a.elements_type != 1) return TestResult::Failed;
        if (a.material_idx != 7) return TestResult::Failed;
        if (a.meshlets.size() != 1) return TestResult::Failed;
        if (a.meshlets[0].vertex_offset != 11 || a.meshlets[0].triangle_offset != 22) return TestResult::Failed;
        if (a.meshlets[0].vertex_count != 3 || a.meshlets[0].triangle_count != 1) return TestResult::Failed;
        if (a.meshlets[0].radius != 3.5f || a.meshlets[0].cone_cutoff != 0.75f) return TestResult::Failed;
        if (a.meshlets[0].padding != 0) return TestResult::Failed;
        if (a.meshlet_vertices.size() != 3 || a.meshlet_vertices[2] != 14) return TestResult::Failed;
        if (a.meshlet_triangles.size() != 3) return TestResult::Failed;
        if (a.sdf.resolution[0] != 2 || a.sdf.resolution[2] != 2) return TestResult::Failed;
        if (a.sdf.data.size() != 8 || a.sdf.data[0] != 1234) return TestResult::Failed;
        if (a.sdf.voxels.size() != 8 || a.sdf.voxels[0] != 255) return TestResult::Failed;
        if (a.sdf.vector_field.size() != 32 || a.sdf.vector_field[0] != 777) return TestResult::Failed;
        if (a.position_buffer.size() != 48 || a.element_buffer.size() != 32) return TestResult::Failed;
        if (*(f32*)a.position_buffer.data() != 0.0f) return TestResult::Failed;

        // First submesh: 2 meshlets — the second one only lands correctly if
        // the reader consumed meshlet #1 with the 60-byte wire stride.
        graphics::rhi::RHIMeshAsset b{};
        if (!content::get_rhi_mesh_asset(subs[0], b) || b.meshlets.size() != 2) return TestResult::Failed;
        if (b.meshlets[1].vertex_offset != 14 || b.meshlets[1].radius != 4.5f) return TestResult::Failed;

        content::destroy_resource(geom, content::asset_type::mesh);
        return TestResult::Passed;
    }

    // Single submesh: exercises create_rhi_single_submesh (fake-pointer path).
    TestResult SingleSubmesh_Parse() {
        std::vector<u8> blob = WrapGeometry({MakeSubmesh("single", 6, 18, 20, 3, 1)});

        id::id_type geom = content::create_resource(blob.data(), content::asset_type::mesh);
        if (!id::is_valid(geom)) return TestResult::Failed;

        id::id_type sub[1] = {id::invalid_id};
        content::get_submesh_gpu_ids(geom, 1, sub);
        graphics::rhi::RHIMeshAsset a{};
        if (!content::get_rhi_mesh_asset(sub[0], a)) return TestResult::Failed;
        if (a.num_vertices != 6 || a.meshlets.size() != 1) return TestResult::Failed;
        if (a.meshlets[0].cone_apex[0] != 1.5f) return TestResult::Failed;

        content::destroy_resource(geom, content::asset_type::mesh);
        return TestResult::Passed;
    }
};

int main() {
    TestContentMeshFormatRoundTrip test;
    int result = 0;

    if (test.MultiSubmesh_Hierarchy_Parse() == TestResult::Failed) {
        std::cerr << "MultiSubmesh_Hierarchy_Parse Failed" << std::endl;
        result = 1;
    } else {
        std::cout << "PASS MultiSubmesh_Hierarchy_Parse" << std::endl;
    }
    if (test.SingleSubmesh_Parse() == TestResult::Failed) {
        std::cerr << "SingleSubmesh_Parse Failed" << std::endl;
        result = 1;
    } else {
        std::cout << "PASS SingleSubmesh_Parse" << std::endl;
    }

    // create_resource registers meshes in the Content-layer static free_list
    // registry; drain before exit or the static destructor asserts !_size.
    primal::content::shutdown();
    return result;
}
