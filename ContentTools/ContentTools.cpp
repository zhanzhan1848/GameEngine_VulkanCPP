#include "ToolsCommon.h"
#include "Geometry.h"
#include "pipeline/Validator.h"

#include <cstring>
#include <iostream>
#include <string>

// Phase 1 M9: ProcessAIAsset pipeline C ABI.
//
// ai_asset_pipeline_params is a POD wrapper consumed by the Editor via
// P/Invoke. Phase 1 minimum: invokes the legacy ImportFbx/ImportObjAPI
// (single source of truth for parsing) then walks the result via
// SceneBlobReader to verify it round-trips through the new pipeline's
// serializer format. Future M10+ work re-runs pipeline modules (uvatlas /
// meshlet / collision) on the imported data before re-serializing.

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
struct ai_asset_pipeline_params
{
    const char*                 input_path;       // .fbx or .obj
    primal::tools::scene_data*  out_data;         // rewritten with serialized blob
    u8                          enable_repair;
    u8                          enable_remesh;
    u8                          enable_subdivide;
    u8                          enable_uvatlas;
    u8                          enable_lod;
    u8                          enable_meshlet;
    u8                          enable_collision;
    f32                         lod_ratio;
    u32                         mesh_count;
    u32                         meshlet_count;
    u32                         validation_errors;
    u32                         validation_warnings;
};

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

    // Step 2: validate the produced blob round-trips through the new
    // pipeline's SceneBlobReader. Phase 1 doesn't re-run modules — the
    // legacy importer's pack_data already produces runtime-ready bytes.
    // Phase 2 will re-parse into ProcessableScene, run pipeline::Run, and
    // re-serialize via SceneBlobWriter.
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
