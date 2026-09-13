// PfxCacheConvertMain.cpp — pfx_converter 离线工具入口。
//
//   pfx_converter alembic --in a.abc [--object /obj/pts] --fps 24 --out a.pcache
//   pfx_converter --make-test-abc a.abc        写合成测试归档（2 帧 × 3 点）
//   pfx_converter --verify a.pcache            头校验

#include "PfxCacheConvert.h"

namespace pfx_convert {
extern bool make_test_vdb(const char* path, std::string& err);
extern bool vdb_to_pgrid(const char* vdb_path, const char* grid_name,
                         const char* out_pgrid, std::string& err);
}

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    std::string err;
    if (argc >= 3 && std::strcmp(argv[1], "--make-test-abc") == 0) {
        if (!pfx_convert::make_test_abc(argv[2], err)) {
            std::fprintf(stderr, "make_test_abc failed: %s\n", err.c_str());
            return 2;
        }
        std::printf("test archive written: %s\n", argv[2]);
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--make-test-vdb") == 0) {
        extern bool make_test_vdb(const char* path, std::string& err);
        std::string vdb_err;
        if (!pfx_convert::make_test_vdb(argv[2], vdb_err)) {
            std::fprintf(stderr, "make_test_vdb failed: %s\n", vdb_err.c_str());
            return 2;
        }
        std::printf("test vdb written: %s\n", argv[2]);
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "vdb") == 0) {
        extern bool vdb_to_pgrid(const char* vdb_path, const char* grid_name,
                                 const char* out_pgrid, std::string& err);
        const char* grid = nullptr;
        const char* out = nullptr;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--grid") == 0 && i + 1 < argc) grid = argv[++i];
            else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
        }
        std::string vdb_err;
        if (!pfx_convert::vdb_to_pgrid(argv[2], grid, out, vdb_err)) {
            std::fprintf(stderr, "vdb_to_pgrid failed: %s\n", vdb_err.c_str());
            return 2;
        }
        std::printf("converted: %s\n", out);
        return 0;
    }
    if (argc >= 3 && std::strcmp(argv[1], "--verify") == 0) {
        const int frames = pfx_convert::verify_pcache(argv[2], err);
        if (frames < 0) {
            std::fprintf(stderr, "verify failed: %s\n", err.c_str());
            return 2;
        }
        std::printf("pcache ok: %d frames\n", frames);
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "alembic") == 0) {
        const char* in = nullptr;
        const char* out = nullptr;
        float fps = 24.f;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--in") == 0 && i + 1 < argc) in = argv[++i];
            else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
            else if (std::strcmp(argv[i], "--fps") == 0 && i + 1 < argc) fps = (float)std::atof(argv[++i]);
        }
        if (!in || !out) {
            std::fprintf(stderr, "usage: pfx_converter alembic --in a.abc --out a.pcache [--fps 24]\n");
            return 2;
        }
        if (!pfx_convert::alembic_to_pcache(in, "", fps, out, err)) {
            std::fprintf(stderr, "alembic_to_pcache failed: %s\n", err.c_str());
            return 2;
        }
        std::printf("converted: %s\n", out);
        return 0;
    }
    std::fprintf(stderr, "usage:\n  pfx_converter alembic --in a.abc --out a.pcache [--fps 24]\n"
                         "  pfx_converter --make-test-abc a.abc\n"
                         "  pfx_converter --verify a.pcache\n");
    return 2;
}
