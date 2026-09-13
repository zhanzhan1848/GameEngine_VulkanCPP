// PfxCacheConvertVdb.cpp — vdb_converter 实体化（N4）：OpenVDB 只读子集 → .pgrid。
//
// 读取：openvdb::io::File（只读），Float Grid → 标量 SDF/密度场；
//       Vec3S Grid → 向量速度场。遍历 active voxels 写入 PGRD 紧凑布局。

#include "PfxCacheConvert.h"

#include <openvdb/openvdb.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pfx_convert {

namespace {

using u32 = std::uint32_t;
using f32 = float;

bool write_pgrid_file(const char* path, u32 kind, u32 nx, u32 ny, u32 nz,
                      float ox, float oy, float oz, float cell, float outside,
                      const std::vector<float>& data, std::string& err) {
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        err = std::string("cannot write '") + path + "'";
        return false;
    }
    const u32 magic = 0x44524750u;   // "PGRD" 小端
    const u32 version = 1u;
    std::fwrite(&magic, sizeof(u32), 1, f);
    std::fwrite(&version, sizeof(u32), 1, f);
    std::fwrite(&kind, sizeof(u32), 1, f);
    std::fwrite(&nx, sizeof(u32), 1, f);
    std::fwrite(&ny, sizeof(u32), 1, f);
    std::fwrite(&nz, sizeof(u32), 1, f);
    const float origin[3] = { ox, oy, oz };
    std::fwrite(origin, sizeof(f32), 3, f);
    std::fwrite(&cell, sizeof(f32), 1, f);
    std::fwrite(&outside, sizeof(f32), 1, f);
    if (!data.empty()) std::fwrite(data.data(), sizeof(f32), data.size(), f);
    std::fclose(f);
    return true;
}

} // namespace

bool make_test_vdb(const char* path, std::string& err);

// 测试辅助：写合成 VDB（FloatGrid 地面 SDF d=y + Vec3SGrid 常量上升风）
bool make_test_vdb(const char* path, std::string& err) {
    openvdb::initialize();
    openvdb::FloatGrid grid(0.0f);
    grid.setTransform(openvdb::math::Transform::createLinearTransform(0.25f));
    grid.setName("floor");
    auto acc = grid.getAccessor();
    for (int z = 0; z <= 7; ++z)
        for (int y = 0; y <= 15; ++y)
            for (int x = 0; x <= 7; ++x) {
                acc.setValue(openvdb::Coord(x, y, z), float(y) * 0.25f);
            }

    openvdb::Vec3SGrid wind(openvdb::Vec3f(0.f, 0.f, 0.f));
    wind.setTransform(openvdb::math::Transform::createLinearTransform(1.0f));
    wind.setName("wind");
    auto wacc = wind.getAccessor();
    for (int z = 0; z <= 3; ++z)
        for (int y = 0; y <= 3; ++y)
            for (int x = 0; x <= 3; ++x) {
                wacc.setValue(openvdb::Coord(x, y, z), openvdb::Vec3f(0.f, 1.f, 0.f));
            }

    std::vector<openvdb::GridBase::Ptr> grids;
    grids.push_back(grid.copy());
    grids.push_back(wind.copy());
    openvdb::io::File file(path);
    file.write(grids);
    file.close();
    return true;
}

bool vdb_to_pgrid(const char* vdb_path, const char* grid_name, const char* out_pgrid,
                  std::string& err) {
    openvdb::initialize();
    openvdb::io::File file(vdb_path);
    file.open();
    openvdb::GridPtrVecPtr grids = file.getGrids();
    file.close();
    if (!grids || grids->empty()) {
        err = "no grids in vdb";
        return false;
    }

    openvdb::GridBase::Ptr base;
    if (grid_name && grid_name[0]) {
        for (auto& g : *grids) {
            if (g->getName() == grid_name) { base = g; break; }
        }
        if (!base) {
            err = std::string("grid '") + grid_name + "' not found";
            return false;
        }
    } else {
        base = grids->front();
    }

    // 向量场（Vec3S）
    if (base->isType<openvdb::Vec3SGrid>()) {
        auto grid = openvdb::gridPtrCast<openvdb::Vec3SGrid>(base);
        const auto bbox = grid->evalActiveVoxelBoundingBox();
        if (bbox.empty()) { err = "grid has no active voxels"; return false; }
        const openvdb::Coord dims = bbox.dim();
        const double cell = grid->voxelSize()[0];
        const openvdb::Coord start = bbox.getStart();
        std::vector<float> data((size_t)dims.x() * dims.y() * dims.z() * 3, 0.f);
        for (auto iter = grid->beginValueOn(); iter; ++iter) {
            const openvdb::Coord c = iter.getCoord();
            const size_t idx = ((size_t)(c.z() - start.z()) * dims.y() + (c.y() - start.y())) * dims.x()
                               + (c.x() - start.x());
            const openvdb::Vec3f v = iter.getValue();
            data[idx * 3 + 0] = v.x();
            data[idx * 3 + 1] = v.y();
            data[idx * 3 + 2] = v.z();
        }
        const openvdb::Vec3d o = grid->indexToWorld(start);
        return write_pgrid_file(out_pgrid, 1u, (u32)dims.x(), (u32)dims.y(), (u32)dims.z(),
                                (float)o.x(), (float)o.y(), (float)o.z(),
                                (float)cell, 0.f, data, err);
    }

    // 标量场（Float，SDF/密度）
    if (base->isType<openvdb::FloatGrid>()) {
        auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
        const auto bbox = grid->evalActiveVoxelBoundingBox();
        if (bbox.empty()) { err = "grid has no active voxels"; return false; }
        const openvdb::Coord dims = bbox.dim();
        const double cell = grid->voxelSize()[0];
        const openvdb::Coord start = bbox.getStart();
        std::vector<float> data((size_t)dims.x() * dims.y() * dims.z(), 0.f);
        for (auto iter = grid->beginValueOn(); iter; ++iter) {
            const openvdb::Coord c = iter.getCoord();
            const size_t idx = ((size_t)(c.z() - start.z()) * dims.y() + (c.y() - start.y())) * dims.x()
                               + (c.x() - start.x());
            data[idx] = float(iter.getValue());
        }
        const openvdb::Vec3d o = grid->indexToWorld(start);
        return write_pgrid_file(out_pgrid, 0u, (u32)dims.x(), (u32)dims.y(), (u32)dims.z(),
                                (float)o.x(), (float)o.y(), (float)o.z(),
                                (float)cell, 0.f, data, err);
    }

    err = "unsupported grid type (only Float/Vec3 float grids)";
    return false;
}

} // namespace pfx_convert
