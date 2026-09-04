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
#include <vector>

// ProcessAIAsset is defined in ContentTools.cpp. This local mirror must
// match the canonical layout in ContentTools.cpp exactly — both are kept
// in sync manually (Phase 1 has no shared header for the POD). The C#
// Editor mirror is the third copy at PrimalEditor/ContentToolsAPI.cs.
#include "AIAssetPipelineParams.h"

extern "C" void ProcessAIAsset(ai_asset_pipeline_params*);
extern "C" void ProcessSceneData(ai_asset_pipeline_params*);

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

bool repair_normal_z_signs(primal::tools::scene_data& data) {
    struct Vec3 { f32 x, y, z; };
    struct Vec2 { f32 x, y; };
    auto sub = [](const Vec3& a, const Vec3& b) {
        return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
    };
    auto cross = [](const Vec3& a, const Vec3& b) {
        return Vec3{a.y * b.z - a.z * b.y,
                    a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x};
    };
    auto dot = [](const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };

    u64 positive = 0;
    u64 negative = 0;
    u64 changed = 0;
    u64 tangentPositive = 0;
    u64 negativeHandedness = 0;
    primal::utl::vector<primal::tools::ErrorReport> errors;
    const u32 meshes = primal::tools::pipeline::WalkSceneBlob(
        data,
        [&](u32, const primal::tools::pipeline::PackedMeshView& view) {
            using primal::tools::elements::elements_type;
            if (view.elements_type != elements_type::static_normal &&
                view.elements_type != elements_type::static_normal_texture) {
                return;
            }

            std::vector<Vec3> positions(view.num_vertices);
            std::vector<Vec3> areaNormals(view.num_vertices, Vec3{0.f, 0.f, 0.f});
            std::vector<Vec3> areaTangents(view.num_vertices, Vec3{0.f, 0.f, 0.f});
            std::vector<Vec3> areaBitangents(view.num_vertices, Vec3{0.f, 0.f, 0.f});
            std::vector<Vec2> uvs(view.num_vertices, Vec2{0.f, 0.f});
            for (u32 i = 0; i < view.num_vertices; ++i) {
                std::memcpy(&positions[i], view.position_buffer + (size_t)i * 12, 12);
                if (view.elements_type == elements_type::static_normal_texture) {
                    const size_t uvOffset = (size_t)view.elements_size - sizeof(Vec2);
                    std::memcpy(&uvs[i], view.element_buffer + (size_t)i * view.elements_size + uvOffset,
                                sizeof(Vec2));
                }
            }
            auto indexAt = [&](u32 i) -> u32 {
                if (view.index_size == 2) {
                    u16 value;
                    std::memcpy(&value, view.index_buffer + (size_t)i * 2, 2);
                    return value;
                }
                u32 value;
                std::memcpy(&value, view.index_buffer + (size_t)i * 4, 4);
                return value;
            };
            for (u32 i = 0; i + 2 < view.num_indices; i += 3) {
                const u32 ia = indexAt(i);
                const u32 ib = indexAt(i + 1);
                const u32 ic = indexAt(i + 2);
                if (ia >= view.num_vertices || ib >= view.num_vertices || ic >= view.num_vertices) continue;
                const Vec3 face = cross(sub(positions[ib], positions[ia]),
                                        sub(positions[ic], positions[ia]));
                Vec3 tangent{0.f, 0.f, 0.f};
                Vec3 bitangent{0.f, 0.f, 0.f};
                if (view.elements_type == elements_type::static_normal_texture) {
                    const Vec3 e1 = sub(positions[ib], positions[ia]);
                    const Vec3 e2 = sub(positions[ic], positions[ia]);
                    const f32 du1 = uvs[ib].x - uvs[ia].x;
                    const f32 dv1 = uvs[ib].y - uvs[ia].y;
                    const f32 du2 = uvs[ic].x - uvs[ia].x;
                    const f32 dv2 = uvs[ic].y - uvs[ia].y;
                    const f32 det = du1 * dv2 - dv1 * du2;
                    if (std::abs(det) > 1e-12f) {
                        const f32 inv = 1.f / det;
                        tangent = Vec3{(e1.x * dv2 - e2.x * dv1) * inv,
                                       (e1.y * dv2 - e2.y * dv1) * inv,
                                       (e1.z * dv2 - e2.z * dv1) * inv};
                        bitangent = Vec3{(e2.x * du1 - e1.x * du2) * inv,
                                         (e2.y * du1 - e1.y * du2) * inv,
                                         (e2.z * du1 - e1.z * du2) * inv};
                    }
                }
                for (const u32 vertex : {ia, ib, ic}) {
                    areaNormals[vertex].x += face.x;
                    areaNormals[vertex].y += face.y;
                    areaNormals[vertex].z += face.z;
                    areaTangents[vertex].x += tangent.x;
                    areaTangents[vertex].y += tangent.y;
                    areaTangents[vertex].z += tangent.z;
                    areaBitangents[vertex].x += bitangent.x;
                    areaBitangents[vertex].y += bitangent.y;
                    areaBitangents[vertex].z += bitangent.z;
                }
            }

            u8* elements = const_cast<u8*>(view.element_buffer);
            for (u32 i = 0; i < view.num_vertices; ++i) {
                u8& tSign = elements[(size_t)i * view.elements_size + 3];
                const u8 before = tSign;
                if (areaNormals[i].z > 0.f) {
                    tSign |= 0x02;
                    ++positive;
                } else {
                    tSign &= (u8)~0x02;
                    ++negative;
                }
                if (view.elements_type == elements_type::static_normal_texture) {
                    if (areaTangents[i].z > 0.f) {
                        tSign |= 0x04;
                        ++tangentPositive;
                    } else {
                        tSign &= (u8)~0x04;
                    }
                    const Vec3 crossNT = cross(areaNormals[i], areaTangents[i]);
                    if (dot(crossNT, areaBitangents[i]) < 0.f) {
                        tSign |= 0x01;
                        ++negativeHandedness;
                    } else {
                        tSign &= (u8)~0x01;
                    }
                }
                if (tSign != before) ++changed;
            }
        },
        errors);

    if (!errors.empty()) {
        for (const auto& error : errors) {
            std::cerr << error.code << ": " << error.message << "\n";
        }
        return false;
    }
    std::cout << "Repaired normal.z signs: meshes=" << meshes
              << ", positive=" << positive << ", negative=" << negative
              << ", tangent_z_positive=" << tangentPositive
              << ", negative_handedness=" << negativeHandedness
              << ", changed=" << changed << "\n";
    return meshes > 0;
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
                     "[--uvatlas-lightmap] "
                     "[--repair-normal-signs] [--no-lod] [--no-meshlet] [--collision] "
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
    bool repairNormalSigns = false;

    for (int i = 4; i < argc; ++i) {
        const std::string arg = argv[i];
        if      (arg == "--repair")     params.enable_repair = 1;
        else if (arg == "--repair-normal-signs") repairNormalSigns = true;
        else if (arg == "--remesh")     params.enable_remesh = 1;
        else if (arg == "--no-remesh")  params.enable_remesh = 0;
        else if (arg == "--subdivide")  params.enable_subdivide = 1;
        else if (arg == "--uvatlas")    params.enable_uvatlas = 1;
        else if (arg == "--uvatlas-lightmap") {
            // A9: second unwrap with Lightmap purpose (strict unique pack,
            // gutter=4). Its output lands in the blob's UVLM sidecar
            // section instead of being dropped. The POD fields mirror the
            // uvatlas::ForLightmap() preset via the ContentTools.cpp bridge.
            params.enable_uvatlas = 1;
            params.uvatlas_target_purpose = 1;      // UVSetPurpose::Lightmap
            params.uvatlas_gutter = 4.0f;
            params.uvatlas_strict_unique_pack = 1;
        }
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

    const std::string inputPath = input_file;
    const auto hasSuffix = [](const std::string& value, const char* suffix) {
        const size_t suffixSize = std::strlen(suffix);
        return value.size() >= suffixSize &&
               value.compare(value.size() - suffixSize, suffixSize, suffix) == 0;
    };
    const bool serializedInput =
        hasSuffix(inputPath, ".model") || hasSuffix(inputPath, ".bin");

    if (serializedInput) {
        if (!read_file(input_file, data)) return 1;
        if (repairNormalSigns && !repair_normal_z_signs(data)) {
            std::free(data.buffer);
            return 1;
        }
        std::cout << "ProcessSceneData: " << input_file << " → " << output_file << "\n";
        ProcessSceneData(&params);
    } else {
        std::cout << "ProcessAIAsset: " << input_file << " → " << output_file << "\n";
        ProcessAIAsset(&params);
    }

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
    // A9 Lightmap UV sidecar (0/0 for legacy blobs).
    std::cout << "Lightmap meshes:  " << report.lightmap_uv_mesh_count << "\n";
    std::cout << "Lightmap UVs:     " << report.lightmap_uv_coord_count << "\n";
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
