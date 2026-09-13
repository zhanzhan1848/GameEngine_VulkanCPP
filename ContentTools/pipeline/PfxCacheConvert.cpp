// PfxCacheConvert.cpp — 见 PfxCacheConvert.h。
//
// Alembic 对象遍历：优先 IPoints（Houdini 点缓存），退化取首个 IPolyMesh 的 P 属性。
// 每个采样 = .pcache 一帧；帧号按采样序 0..N-1（时间语义由 fps 在回放侧还原）。
// .pcache 布局（小端，与 Engine/ParticleFX/interop/GeometryCache.h 契约一致）：
//   "PCH1" | u32 frame_count | u32 point_count | u32 frame_ids[N] | f32 points[N*count*3]

#include "PfxCacheConvert.h"

#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

using u32 = std::uint32_t;
using f32 = float;

namespace pfx_convert {

namespace {

namespace AbcG = Alembic::AbcGeom;
namespace AbcC = Alembic::AbcCoreOgawa;

bool write_pcache_file(const char* path, u32 point_count, const std::vector<u32>& frame_ids,
                       const std::vector<float>& data, std::string& err) {
    FILE* f = std::fopen(path, "wb");
    if (!f) {
        err = std::string("cannot write '") + path + "'";
        return false;
    }
    const u32 frame_count = (u32)frame_ids.size();
    std::fwrite("PCH1", 1, 4, f);
    std::fwrite(&frame_count, sizeof(u32), 1, f);
    std::fwrite(&point_count, sizeof(u32), 1, f);
    std::fwrite(frame_ids.data(), sizeof(u32), frame_ids.size(), f);
    if (!data.empty()) std::fwrite(data.data(), sizeof(f32), data.size(), f);
    std::fclose(f);
    return true;
}

} // namespace

bool make_test_abc(const char* abc_path, std::string& err) {
    try {
        AbcG::OArchive archive(AbcC::WriteArchive(), abc_path);
        AbcG::OPoints points(archive.getTop(), "pts");

        // 2 帧 × 3 点（确定性：帧 1 沿 +y 平移 1）
        const float frame0[9] = { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 2.f };
        const float frame1[9] = { 0.f, 1.f, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 2.f };
        const uint64_t ids[3] = { 0, 1, 2 };

        for (int f = 0; f < 2; ++f) {
            const float* pts = f == 0 ? frame0 : frame1;
            AbcG::P3fArraySample pos_sample(reinterpret_cast<const Imath::V3f*>(pts), 3);
            AbcG::UInt64ArraySample id_sample(ids, 3);
            AbcG::OPointsSchema::Sample sample(pos_sample, id_sample);
            points.getSchema().set(sample);
        }
        return true;
    } catch (std::exception& e) {
        err = e.what();
        return false;
    }
}

bool alembic_to_pcache(const char* abc_path, const char* object_path, float fps,
                       const char* out_pcache, std::string& err) {
    (void)object_path;
    try {
        AbcG::IArchive archive(AbcC::ReadArchive(), abc_path);
        if (!archive.valid()) {
            err = "cannot open archive";
            return false;
        }

        // 遍历归档：优先 IPoints，退化首个 IPolyMesh
        std::vector<AbcG::IPoints> points_list;
        std::vector<AbcG::IPolyMesh> mesh_list;
        std::vector<AbcG::IObject> stack{ archive.getTop() };
        while (!stack.empty()) {
            AbcG::IObject obj = stack.back();
            stack.pop_back();
            for (size_t i = 0; i < obj.getNumChildren(); ++i) {
                const AbcG::ObjectHeader& header = obj.getChildHeader(i);
                if (AbcG::IPoints::matches(header)) {
                    points_list.emplace_back(obj, header.getName());
                    continue;
                }
                if (mesh_list.empty() && AbcG::IPolyMesh::matches(header)) {
                    mesh_list.emplace_back(obj, header.getName());
                }
                stack.push_back(obj.getChild(i));
            }
        }

        std::vector<u32> frame_ids;
        std::vector<float> data;
        u32 point_count = 0;

        if (!points_list.empty()) {
            AbcG::IPointsSchema& schema = points_list.front().getSchema();
            const size_t samples = schema.getNumSamples();
            for (size_t s = 0; s < samples; ++s) {
                AbcG::ISampleSelector sel(static_cast<AbcG::index_t>(s));
                AbcG::P3fArraySamplePtr pos = schema.getPositionsProperty().getValue(sel);
                const size_t count = pos->size();
                if (point_count == 0) point_count = (u32)count;
                const Imath::V3f* p = pos->get();
                for (size_t i = 0; i < count; ++i) {
                    data.push_back(p[i].x);
                    data.push_back(p[i].y);
                    data.push_back(p[i].z);
                }
                frame_ids.push_back((u32)s);
            }
        } else if (!mesh_list.empty()) {
            AbcG::IPolyMeshSchema& schema = mesh_list.front().getSchema();
            const size_t samples = schema.getNumSamples();
            for (size_t s = 0; s < samples; ++s) {
                AbcG::ISampleSelector sel(static_cast<AbcG::index_t>(s));
                AbcG::P3fArraySamplePtr pos = schema.getPositionsProperty().getValue(sel);
                const size_t count = pos->size();
                if (point_count == 0) point_count = (u32)count;
                const Imath::V3f* p = pos->get();
                for (size_t i = 0; i < count; ++i) {
                    data.push_back(p[i].x);
                    data.push_back(p[i].y);
                    data.push_back(p[i].z);
                }
                frame_ids.push_back((u32)s);
            }
        } else {
            err = "no Points/PolyMesh objects in archive";
            return false;
        }

        if (point_count == 0 || frame_ids.empty()) {
            err = "archive has no position samples";
            return false;
        }
        (void)fps;   // 帧率语义在回放侧（geometry_cache::sample 的 fps 参数）
        return write_pcache_file(out_pcache, point_count, frame_ids, data, err);
    } catch (std::exception& e) {
        err = e.what();
        return false;
    }
}

int verify_pcache(const char* pcache_path, std::string& err) {
    FILE* f = std::fopen(pcache_path, "rb");
    if (!f) {
        err = "cannot open pcache";
        return -1;
    }
    char magic[4] = {};
    u32 frame_count = 0, point_count = 0;
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "PCH1", 4) == 0 &&
              std::fread(&frame_count, sizeof(u32), 1, f) == 1 &&
              std::fread(&point_count, sizeof(u32), 1, f) == 1;
    std::fclose(f);
    if (!ok) {
        err = "bad/truncated pcache";
        return -1;
    }
    return (int)frame_count;
}

} // namespace pfx_convert
