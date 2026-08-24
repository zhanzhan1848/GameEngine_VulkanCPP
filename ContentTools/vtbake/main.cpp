/**
 * @file main.cpp
 * @brief vtbake — VT M1 离线烘焙 CLI(跨平台,独立于 ContentTools 主工程)
 * @details 用法:
 *   vtbake <input.png> <output.vtex> [--kind color|normal|scalar|auto]
 *          [--no-bcn] [--no-astc] [--zlib] [--tile 128] [--border 4]
 *           [--quality 25]
 *   --kind auto:按文件名推断(_normal→normal,_roughness/_metallic/_height
 *               →scalar,其余 color)
 */

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "../common/VTBaker.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace primal::content::vt;

static channel_kind infer_kind(const std::string& path) {
    std::string p = path;
    for (auto& c : p)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (p.find("_normal") != std::string::npos || p.find("_nrm") != std::string::npos)
        return channel_kind::normal;
    if (p.find("_roughness") != std::string::npos || p.find("_metallic") != std::string::npos ||
        p.find("_height") != std::string::npos || p.find("_mask") != std::string::npos)
        return channel_kind::scalar;
    return channel_kind::color;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: vtbake <input.png> <output.vtex> [--kind color|normal|scalar|auto]"
                     " [--no-bcn] [--no-astc] [--zlib] [--tile N] [--border N] [--quality F]"
                  << std::endl;
        return 1;
    }
    bake_config cfg;
    bool kind_auto = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--kind" && i + 1 < argc) {
            std::string k = argv[++i];
            kind_auto = (k == "auto");
            if (k == "normal") cfg.kind = channel_kind::normal;
            else if (k == "scalar") cfg.kind = channel_kind::scalar;
            else cfg.kind = channel_kind::color;
        } else if (a == "--no-bcn") cfg.use_bcn = false;
        else if (a == "--no-astc") cfg.use_astc = false;
        else if (a == "--zlib") cfg.zlib_tiles = true;
        else if (a == "--tile" && i + 1 < argc) cfg.tile_size = uint32_t(atoi(argv[++i]));
        else if (a == "--border" && i + 1 < argc) cfg.border = uint32_t(atoi(argv[++i]));
        else if (a == "--quality" && i + 1 < argc) cfg.astc_quality = float(atof(argv[++i]));
        else { std::cerr << "unknown arg: " << a << std::endl; return 1; }
    }
    if (kind_auto) cfg.kind = infer_kind(argv[1]);

    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(argv[1], &w, &h, &comp, 4);
    if (!pixels) {
        std::cerr << "vtbake: cannot load " << argv[1] << " — " << stbi_failure_reason() << std::endl;
        return 1;
    }
    std::cout << "vtbake: " << argv[1] << " " << w << "x" << h << " kind="
              << int(cfg.kind) << std::endl;

    std::vector<uint8_t> vt_file;
    bake_stats stats;
    if (!bake(pixels, uint32_t(w), uint32_t(h), cfg, vt_file, &stats)) {
        stbi_image_free(pixels);
        std::cerr << "vtbake: bake failed" << std::endl;
        return 1;
    }
    stbi_image_free(pixels);

    std::ofstream f(argv[2], std::ios::binary);
    if (!f) {
        std::cerr << "vtbake: cannot write " << argv[2] << std::endl;
        return 1;
    }
    f.write(reinterpret_cast<const char*>(vt_file.data()),
            std::streamsize(vt_file.size()));
    f.close();

    std::cout << "vtbake: " << argv[2] << " mips=" << stats.mip_count
              << " pages=" << stats.page_count
              << " bcn=" << stats.bcn_bytes << "B astc=" << stats.astc_bytes
              << "B total=" << stats.total_bytes << "B avg=0x" << std::hex
              << stats.avg_color << std::dec << std::endl;
    return 0;
}
