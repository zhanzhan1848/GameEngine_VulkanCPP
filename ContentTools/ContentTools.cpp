#include "ToolsCommon.h"
#include "Geometry.h"
#include "pipeline/Validator.h"
#include "pipeline/SceneBlobReader.h"
#include "pipeline/SceneBlobWriter.h"
#include "pipeline/PackedMeshDecoder.h"
#include "pipeline/AssetPipeline.h"
#include "common/ProcessableMesh.h"
#include "common/PipelineTypes.h"
#include "common/ErrorReport.h"
#include "../Engine/Utilities/IOStream.h"

#include <cstring>
#include <iostream>
#include <string>
#include <utility>

// Phase 2 M12: ProcessAIAsset pipeline C ABI.
//
// ai_asset_pipeline_params is a POD wrapper consumed by the Editor via
// P/Invoke. Phase 1 ran validation-only (legacy import → blob round-trip).
// Phase 2 wires the module flags into pipeline::Run when ANY opt-in flag is
// on (repair / remesh / subdivide / uvatlas / derive / collision). Default
// path (no opt-in flags) preserves byte-identical legacy output.

namespace primal::tools {

// Forward-declared legacy importers (defined in FbxImporter.cpp /
// ObjImporter.cpp). Same signature: (file, scene_data*, progress callback).
// Declared at file scope so the C ABI below can name them.
extern "C" void ImportFbx(const char* file, scene_data* data, void(*callback)(s32, s32));
extern "C" void ImportObjAPI(const char* file, scene_data* data, void(*callback)(s32, s32));

extern void ShutDownTextureTools();

// Detect file extension. Returns "fbx" / "obj" / "" (unknown).
static std::string get_extension(const char* path) {
    if (!path) return "";
    std::string p{path};
    const size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = p.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    }
    return ext;
}

}  // namespace primal::tools

// POD wrapper exposed to the Editor via C ABI. Kept at global scope so
// the C# P/Invoke side can match the layout without namespace mangling.
//
// Field ordering rule: append new fields at the tail to keep the C# mirror
// (PrimalEditor/ContentToolsAPI.cs) a strict superset of the previous layout.
struct ai_asset_pipeline_params
{
    const char*                 input_path;                       // .fbx or .obj
    primal::tools::scene_data*  out_data;                         // rewritten with serialized blob
    u8                          enable_repair;
    u8                          enable_remesh;
    u8                          enable_subdivide;
    u8                          enable_uvatlas;
    u8                          enable_lod;
    u8                          enable_meshlet;
    u8                          enable_collision;
    u8                          enable_derive;                     // M11: standalone derive (independent of subdivide)
    u8                          auto_derive_after_subdivide;       // M11: default 1; cascade derive after subdivide
    u8                          subdivide_scheme;                  // M11: 0=Loop, 1=CatmullClark
    u8                          derive_normal_mode;                // M11: 0=Faceted, 1=Smooth, 2=AreaWeighted, 3=AngleWeighted
    u8                          derive_tangent_mode;               // M11: 0=None, 1=AreaWeighted, 2=MikkTSpace
    u32                         subdivide_levels;                  // M11: default 1
    f32                         lod_ratio;
    f32                         derive_faceted_angle_deg;          // M11: default 60.0 (only used when derive_normal_mode=Faceted)
    u32                         mesh_count;
    u32                         meshlet_count;
    u32                         validation_errors;
    u32                         validation_warnings;
    // Remesh params (appended at tail to preserve C# mirror superset).
    // target_edge_length: 0 = use remesh::Params default (0.05); callers
    // should set a value appropriate to the asset's world-space scale.
    // remesh_iterations: 0 = use default (10).
    f32                         target_edge_length;
    u32                         remesh_iterations;
};

namespace {

using namespace primal;
using namespace primal::tools;
using namespace primal::tools::pipeline;

// Match Geometry.cpp::pack_data's allocator (CoTaskMemAlloc on MSVC, malloc
// elsewhere). Editor frees via Marshal.FreeCoTaskMem (Win) / free (other).
// Must match exactly so the Editor's free pairs with our alloc.
void free_scene_buffer(scene_data* sd) {
    if (!sd || !sd->buffer) return;
#if defined(_MSC_VER)
    CoTaskMemFree(sd->buffer);
#else
    std::free(sd->buffer);
#endif
    sd->buffer = nullptr;
    sd->buffer_size = 0;
}

u8* alloc_scene_buffer(size_t bytes) {
#if defined(_MSC_VER)
    return (u8*)CoTaskMemAlloc(bytes);
#else
    return (u8*)std::malloc(bytes);
#endif
}

// free_scene_buffer_ptr: free a raw pointer that alloc_scene_buffer returned.
// Kept separate from free_scene_buffer(scene_data*) so the alloc/swap path
// can free a half-built buffer without the scene_data* wrapper.
void free_scene_buffer_ptr(u8* buf) {
    if (!buf) return;
#if defined(_MSC_VER)
    CoTaskMemFree(buf);
#else
    std::free(buf);
#endif
}

// Map the POD params into pipeline::Config. Mirrors main.cpp's defaults
// for fields the POD doesn't carry (meshlet cap, max LOD levels, etc.).
// These defaults match AssetPipeline::Config's own defaults where possible;
// divergence is documented inline.
void ConfigurePipelineFromPOD(const ai_asset_pipeline_params& p, Config& cfg) {
    cfg.enable_repair    = p.enable_repair != 0;
    cfg.enable_remesh    = p.enable_remesh != 0;
    cfg.enable_subdivide = p.enable_subdivide != 0;
    cfg.enable_uvatlas   = p.enable_uvatlas != 0;
    cfg.enable_collision = p.enable_collision != 0;
    cfg.enable_derive    = p.enable_derive != 0;
    cfg.auto_derive_after_subdivide = p.auto_derive_after_subdivide != 0;

    cfg.enable_lod     = p.enable_lod != 0;
    cfg.enable_meshlet = p.enable_meshlet != 0;

    cfg.subdivide_params.scheme =
        (p.subdivide_scheme == 1) ? subdivide::Scheme::CatmullClark
                                  : subdivide::Scheme::Loop;
    cfg.subdivide_params.levels = p.subdivide_levels;

    cfg.derive_params.normal_mode =
        (derive::NormalMode)p.derive_normal_mode;
    cfg.derive_params.tangent_mode =
        (derive::TangentMode)p.derive_tangent_mode;
    cfg.derive_params.faceted_angle_degrees = p.derive_faceted_angle_deg;

    // Remesh params: target_edge_length <= 0 means AUTO (per-mesh bbox).
    // remesh::Params::target_edge_length defaults to 0.0f (AUTO), so we
    // only override when the caller explicitly set a positive value.
    if (p.target_edge_length > 0.f) {
        cfg.remesh_params.target_edge_length = p.target_edge_length;
    }
    if (p.remesh_iterations > 0) {
        cfg.remesh_params.iterations = p.remesh_iterations;
    }

    cfg.lod_params.ratio = (p.lod_ratio > 0.f) ? p.lod_ratio : 0.5f;
    cfg.lod_params.max_levels = 6;  // Phase 1 default; POD has no field yet.

    // Meshlet cap: matches Geometry.cpp's meshopt_buildMeshlets call (64/124).
    // Note: pipeline's meshlet module uses 256/128 by default, NOT 64/124 —
    // re-encoded output will have larger meshlets than ImportFbx's path.
    // That's expected: the new pipeline uses modern caps. Byte-identical
    // output is impossible once opt-in modules run anyway.
    cfg.meshlet_params.max_vertices  = 256;
    cfg.meshlet_params.max_triangles = 128;

    // SDF: preserve legacy behavior (ImportFbx generates SDF; pipeline
    // regenerates so the re-encoded blob carries fresh SDF data). Note this
    // is a slow path for large meshes — see plan's "已知 trade-offs".
    cfg.enable_sdf = true;
}

// True when any opt-in module flag is set (excludes lod/meshlet defaults).
// Phase 2 contract: byte-identical legacy output when this is false.
bool any_opt_in_module_on(const ai_asset_pipeline_params& p) {
    return p.enable_repair || p.enable_remesh || p.enable_subdivide ||
           p.enable_uvatlas || p.enable_derive || p.enable_collision;
}

// Re-run pipeline::Run on the imported blob, replacing `p->out_data` with
// a freshly encoded buffer. Returns true on success. On failure, leaves
// `p->out_data` unchanged and emits warnings via cerr (caller still validates
// the original buffer).
bool run_pipeline_and_reencode(ai_asset_pipeline_params* p) {
    // Step 1: decode the legacy blob into ProcessableScene IR.
    ProcessableScene scene;
    utl::vector<ErrorReport> decode_errs;
    if (!DecodeScene(*p->out_data, scene, decode_errs)) {
        std::cerr << "ProcessAIAsset: DecodeScene failed; falling back to legacy buffer\n";
        for (const auto& e : decode_errs) {
            std::cerr << "  " << (e.severity == Severity::Error ? "ERROR" : "WARN")
                      << " " << e.code << ": " << e.message << "\n";
        }
        return false;
    }

    // Step 2: configure + run pipeline.
    Config cfg;
    ConfigurePipelineFromPOD(*p, cfg);
    Result result;
    // scene is moved-in; pipeline mutates it in place.
    Run(std::move(scene), cfg, result);

    // Step 3: re-encode. AssetPipeline places one PackedMesh per (lod, mesh).
    // Re-serialize into a single lod_group containing all of them.
    const size_t new_size = GetSceneSize(result.scene.name,
                                         result.scene.materials,
                                         result.packed);
    if (new_size == 0) {
        std::cerr << "ProcessAIAsset: GetSceneSize returned 0\n";
        return false;
    }
    u8* new_buffer = alloc_scene_buffer(new_size);
    if (!new_buffer) {
        std::cerr << "ProcessAIAsset: alloc failed for " << new_size << " bytes\n";
        return false;
    }
    utl::blob_stream_writer blob{new_buffer, new_size};
    SerializeScene(result.scene.name, result.scene.materials,
                   result.packed, blob);
    if (blob.position() != new_buffer + new_size) {
        std::cerr << "ProcessAIAsset: SerializeScene wrote "
                  << (blob.position() - new_buffer) << " bytes, expected "
                  << new_size << "\n";
        free_scene_buffer_ptr(new_buffer);
        return false;
    }

    // Step 4: swap. Free the OLD buffer (Editor handed us ownership via
    // p->out_data, legacy import allocated via pack_data).
    free_scene_buffer(p->out_data);
    p->out_data->buffer = new_buffer;
    p->out_data->buffer_size = (u32)new_size;
    return true;
}

}  // namespace

EDITOR_INTERFACE void ProcessAIAsset(ai_asset_pipeline_params* p)
{
    using namespace primal::tools;
    assert(p && p->input_path && p->out_data);

    // Step 1: legacy import — fills p->out_data with the canonical
    // scene_data.buffer layout (the byte format 4 downstream readers depend
    // on). Phase 1 keeps this as the parsing source of truth.
    const std::string ext = get_extension(p->input_path);
    if (ext == "fbx") {
        ImportFbx(p->input_path, p->out_data, nullptr);
    } else if (ext == "obj") {
        ImportObjAPI(p->input_path, p->out_data, nullptr);
    } else {
        std::cerr << "ProcessAIAsset: unsupported extension '" << ext << "'\n";
        return;
    }

    if (!p->out_data->buffer || p->out_data->buffer_size == 0) return;

    // Step 2 (Phase 2 M12.4): if any opt-in module is on, decode →
    // pipeline::Run → re-encode. Default path (no opt-in flags) keeps the
    // legacy byte-identical buffer.
    if (any_opt_in_module_on(*p)) {
        run_pipeline_and_reencode(p);
        // On failure: legacy buffer is preserved; we still validate below.
    }

    // Step 3: validate the produced blob round-trips through the new
    // pipeline's SceneBlobReader.
    primal::utl::vector<ErrorReport> errs;
    pipeline::ValidationReport report;
    pipeline::ValidateSceneBlob(*p->out_data, report, errs);

    p->mesh_count          = report.mesh_count;
    p->meshlet_count       = report.meshlet_count;
    p->validation_errors   = report.total_errors;
    p->validation_warnings = report.total_warnings;
}

EDITOR_INTERFACE void ShutDownContentTools()
{
    primal::tools::ShutDownTextureTools();
}
