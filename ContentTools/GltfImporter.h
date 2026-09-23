#pragma once

#include "ToolsCommon.h"

#include <memory>

namespace primal::tools
{
    struct scene;
    struct scene_data;
    // NOTE: progression lives in the GLOBAL namespace (ToolsCommon.h), which
    // this header already includes — do not forward-declare it here or the
    // incomplete local declaration shadows the complete global class.

    // glTF 2.0 importer (.gltf JSON / .glb container). Handles the
    // generative-model output subset: meshes (triangles), PBR materials
    // (baseColor/normal textures), node transforms, embedded images
    // (bufferView or data URI — extracted to disk so the C# texture cook
    // can resolve them). Skins/animations/sparse accessors/extensions are
    // rejected with a clear error rather than silently mis-imported.
    //
    // Behavioral twin of ObjImporter: builds the same `scene` IR (one mesh
    // per primitive + OBJ-style recursive meshopt LOD groups), then the
    // shared process_scene/pack_data path serializes it.
    class gltf_context
    {
    public:
        gltf_context(const char* file, scene* scene, scene_data* data, progression* const progression);
        ~gltf_context();

        void get_scene();

        constexpr f32 scene_scale() const { return _scene_scale; }
        constexpr progression* get_progression() const { return _progression; }

    private:
        struct impl;
        std::unique_ptr<impl> _impl;

        scene* _scene{ nullptr };
        scene_data* _scene_data{ nullptr };
        progression* _progression{ nullptr };
        f32 _scene_scale{ 1.0f };
    };

    // ABI entry: parse a glTF 2.0 file into the canonical scene_data layout
    // (same contract as ImportFbx / ImportObjAPI).
    EDITOR_INTERFACE void ImportGltfAPI(const char* file, scene_data* data, progression::progress_callback callback);
}
