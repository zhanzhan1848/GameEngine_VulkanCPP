// ContentToolsCLI: command-line frontend for the Phase 1 asset pipeline.
//
// Subcommands:
//   ContentToolsCLI <input_fbx> <output_bin>           — legacy default
//                                                          (ImportFbx + write)
//   ContentToolsCLI --pipeline <input> <output> [...]  — M9 ProcessAIAsset path
//   ContentToolsCLI --validate <input_bin>             — M9 meshlet validator
//
// The legacy 2-arg path is preserved so existing scripts keep working.
// `--pipeline` enables per-asset toggling of new pipeline modules.

#include "ToolsCommon.h"
#include "Geometry.h"
#include "pipeline/SceneBlobReader.h"
#include "pipeline/Validator.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

// ProcessAIAsset is defined in ContentTools.cpp. This local mirror must
// match the canonical layout in ContentTools.cpp exactly — both are kept
// in sync manually (Phase 1 has no shared header for the POD). The C#
// Editor mirror is the third copy at PrimalEditor/ContentToolsAPI.cs.
struct ai_asset_pipeline_params {
    const char*                 input_path;
    primal::tools::scene_data*  out_data;
    u8                          enable_repair;
    u8                          enable_remesh;
    u8                          enable_subdivide;
    u8                          enable_uvatlas;
    u8                          enable_lod;
    u8                          enable_meshlet;
    u8                          enable_collision;
    u8                          enable_derive;
    u8                          auto_derive_after_subdivide;
    u8                          subdivide_scheme;     // 0=Loop, 1=CatmullClark
    u8                          derive_normal_mode;   // 0=Faceted, 1=Smooth, 2=AreaWeighted, 3=AngleWeighted
    u8                          derive_tangent_mode;  // 0=None, 1=AreaWeighted, 2=MikkTSpace
    u32                         subdivide_levels;
    f32                         lod_ratio;
    f32                         derive_faceted_angle_deg;
    u32                         mesh_count;
    u32                         meshlet_count;
    u32                         validation_errors;
    u32                         validation_warnings;
    // Remesh params (must match ContentTools.cpp's POD layout exactly).
    f32                         target_edge_length;
    u32                         remesh_iterations;
};
extern "C" void ProcessAIAsset(ai_asset_pipeline_params*);

// Legacy importers (used by the default 2-arg path).
extern "C" void ImportFbx(const char* file, primal::tools::scene_data* data, void(*cb)(s32, s32));

namespace {

void configure_defaults(primal::tools::scene_data& data) {
    data.settings.calculate_normals = true;
    data.settings.calculate_tangents = true;
    data.settings.reverse_handedness = false;
    data.settings.import_embedded_textures = true;
    data.settings.import_animations = false;
}

bool write_file(const char* path, const primal::tools::scene_data& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open output file: " << path << "\n";
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.buffer), data.buffer_size);
    std::cout << "Wrote " << data.buffer_size << " bytes to " << path << "\n";
    return true;
}

// Read a binary file into a scene_data buffer. Caller frees data.buffer.
bool read_file(const char* path, primal::tools::scene_data& data) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::cerr << "Failed to open input file: " << path << "\n";
        return false;
    }
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    data.buffer = (u8*)std::malloc((size_t)size);
    data.buffer_size = (u32)size;
    if (!in.read(reinterpret_cast<char*>(data.buffer), size)) {
        std::cerr << "Failed to read " << size << " bytes from " << path << "\n";
        std::free(data.buffer);
        data.buffer = nullptr;
        data.buffer_size = 0;
        return false;
    }
    return true;
}

int run_legacy_default(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: ContentToolsCLI <input_fbx> <output_bin>\n";
        return 1;
    }
    const char* input_file = argv[1];
    const char* output_file = argv[2];

    primal::tools::scene_data data{};
    configure_defaults(data);

    std::cout << "Importing FBX: " << input_file << "...\n";
    ImportFbx(input_file, &data, [](s32 val, s32 max) {
        if (max > 0 && (val % 10 == 0 || val == max)) {
            std::cout << "\rProgress: " << val << "/" << max << " ("
                      << (val * 100 / max) << "%)" << std::flush;
        }
    });
    std::cout << "\nImport finished.\n";

    if (!data.buffer || data.buffer_size == 0) {
        std::cerr << "Failed to import FBX or empty result.\n";
        return 1;
    }
    if (!write_file(output_file, data)) {
        std::free(data.buffer);
        return 1;
    }
    std::free(data.buffer);
    return 0;
}

int run_pipeline(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: ContentToolsCLI --pipeline <input> <output> "
                     "[--repair] [--remesh] [--subdivide] [--uvatlas] "
                     "[--no-lod] [--no-meshlet] [--collision] "
                     "[--lod-ratio <f>]\n"
                     "  Subdivide:    --subdivide-levels N (default 1)\n"
                     "                --subdivide-scheme loop|catmull (default loop)\n"
                     "  Derive (M11): --derive (standalone; auto-cascade after --subdivide is on by default)\n"
                     "                --no-auto-derive (turn off post-subdivide cascade)\n"
                     "                --derive-normal faceted|smooth|area|angle (default area)\n"
                     "                --derive-tangent none|area|mikkt (default mikkt)\n"
                     "                --faceted-angle <deg> (default 60)\n"
                     "  Remesh:       --target-edge-length <f> (default 0.05; MUST match asset scale!)\n"
                     "                --remesh-iters N (default 10)\n";
        return 1;
    }
    const char* input_file = argv[2];
    const char* output_file = argv[3];

    ai_asset_pipeline_params params{};
    params.input_path = input_file;
    // Phase 1 defaults: lod + meshlet + remesh on.
    // remesh uses split-only long-edge bisection (no collapse): breaks giant
    // triangles into smaller pieces without losing detail. Smart-skip skips
    // meshes that are already fine. Use --no-remesh to disable.
    params.enable_lod = 1;
    params.enable_meshlet = 1;
    params.enable_remesh = 1;
    params.lod_ratio = 0.5f;
    // M11 defaults: match derive::Params and subdivide::Params invariants.
    // auto_derive_after_subdivide defaults to ON so callers opting into
    // --subdivide don't ship a mesh with zeroed normals.
    params.auto_derive_after_subdivide = 1;
    params.subdivide_levels = 1;
    params.subdivide_scheme = 0;  // Loop
    params.derive_normal_mode = 2;  // AreaWeighted (matches derive::Params default)
    params.derive_tangent_mode = 2;  // MikkTSpace (matches derive::Params default)
    params.derive_faceted_angle_deg = 60.f;

    for (int i = 4; i < argc; ++i) {
        const std::string arg = argv[i];
        if      (arg == "--repair")     params.enable_repair = 1;
        else if (arg == "--remesh")     params.enable_remesh = 1;
        else if (arg == "--no-remesh")  params.enable_remesh = 0;
        else if (arg == "--subdivide")  params.enable_subdivide = 1;
        else if (arg == "--uvatlas")    params.enable_uvatlas = 1;
        else if (arg == "--no-lod")     params.enable_lod = 0;
        else if (arg == "--no-meshlet") params.enable_meshlet = 0;
        else if (arg == "--collision")  params.enable_collision = 1;
        else if (arg == "--derive")     params.enable_derive = 1;
        else if (arg == "--no-auto-derive") params.auto_derive_after_subdivide = 0;
        else if (arg == "--lod-ratio" && i + 1 < argc) {
            params.lod_ratio = (f32)std::stof(argv[++i]);
        } else if (arg == "--subdivide-levels" && i + 1 < argc) {
            params.subdivide_levels = (u32)std::stoul(argv[++i]);
        } else if (arg == "--subdivide-scheme" && i + 1 < argc) {
            const std::string s = argv[++i];
            if      (s == "loop")    params.subdivide_scheme = 0;
            else if (s == "catmull") params.subdivide_scheme = 1;
            else {
                std::cerr << "Unknown --subdivide-scheme value: " << s
                          << " (expected loop|catmull)\n";
                return 1;
            }
        } else if (arg == "--derive-normal" && i + 1 < argc) {
            const std::string s = argv[++i];
            if      (s == "faceted") params.derive_normal_mode = 0;
            else if (s == "smooth")  params.derive_normal_mode = 1;
            else if (s == "area")    params.derive_normal_mode = 2;
            else if (s == "angle")   params.derive_normal_mode = 3;
            else {
                std::cerr << "Unknown --derive-normal value: " << s
                          << " (expected faceted|smooth|area|angle)\n";
                return 1;
            }
        } else if (arg == "--derive-tangent" && i + 1 < argc) {
            const std::string s = argv[++i];
            if      (s == "none")   params.derive_tangent_mode = 0;
            else if (s == "area")   params.derive_tangent_mode = 1;
            else if (s == "mikkt")  params.derive_tangent_mode = 2;
            else {
                std::cerr << "Unknown --derive-tangent value: " << s
                          << " (expected none|area|mikkt)\n";
                return 1;
            }
        } else if (arg == "--faceted-angle" && i + 1 < argc) {
            params.derive_faceted_angle_deg = (f32)std::stof(argv[++i]);
        } else if (arg == "--target-edge-length" && i + 1 < argc) {
            params.target_edge_length = (f32)std::stof(argv[++i]);
        } else if (arg == "--remesh-iters" && i + 1 < argc) {
            params.remesh_iterations = (u32)std::stoul(argv[++i]);
        } else {
            std::cerr << "Unknown --pipeline flag: " << arg << "\n";
            return 1;
        }
    }

    primal::tools::scene_data data{};
    configure_defaults(data);
    params.out_data = &data;

    std::cout << "ProcessAIAsset: " << input_file << " → " << output_file << "\n";
    ProcessAIAsset(&params);

    if (!data.buffer || data.buffer_size == 0) {
        std::cerr << "ProcessAIAsset produced empty result.\n";
        return 1;
    }
    if (!write_file(output_file, data)) {
        std::free(data.buffer);
        return 1;
    }
    std::cout << "Meshes: " << params.mesh_count
              << ", meshlets: " << params.meshlet_count
              << ", validation errors: " << params.validation_errors
              << ", warnings: " << params.validation_warnings << "\n";
    std::free(data.buffer);
    return params.validation_errors > 0 ? 2 : 0;
}

int run_validate(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: ContentToolsCLI --validate <input_bin>\n";
        return 1;
    }
    const char* input_file = argv[2];

    primal::tools::scene_data data{};
    if (!read_file(input_file, data)) return 1;

    primal::tools::pipeline::ValidationReport report;
    primal::utl::vector<primal::tools::ErrorReport> errors;
    const bool ok = primal::tools::pipeline::ValidateSceneBlob(data, report, errors);

    std::cout << "=== Validation: " << input_file << " ===\n";
    std::cout << "Meshes:           " << report.mesh_count << "\n";
    std::cout << "Meshlets:         " << report.meshlet_count << "\n";
    std::cout << "Over vertex cap:  " << report.meshlets_over_vertex_cap << "\n";
    std::cout << "Over triangle cap:" << report.meshlets_over_triangle_cap << "\n";
    std::cout << "Zero bounds:      " << report.meshlets_zero_bounds << "\n";
    std::cout << "Invalid offsets:  " << report.meshlets_invalid_offsets << "\n";
    std::cout << "Total errors:     " << report.total_errors << "\n";
    std::cout << "Total warnings:   " << report.total_warnings << "\n";

    // Print up to 10 detailed error reports.
    u32 shown = 0;
    for (const auto& e : errors) {
        if (e.severity == primal::tools::Severity::Error) {
            std::cout << "  ERROR  " << e.code << ": " << e.message << "\n";
            if (++shown >= 10) { std::cout << "  ... (truncated)\n"; break; }
        }
    }

    std::free(data.buffer);
    std::cout << (ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage:\n"
                  << "  ContentToolsCLI <input_fbx> <output_bin>\n"
                  << "  ContentToolsCLI --pipeline <input> <output> [...]\n"
                  << "  ContentToolsCLI --validate <input_bin>\n";
        return 1;
    }

    const std::string first = argv[1];
    if (first == "--pipeline") return run_pipeline(argc, argv);
    if (first == "--validate") return run_validate(argc, argv);
    return run_legacy_default(argc, argv);
}
