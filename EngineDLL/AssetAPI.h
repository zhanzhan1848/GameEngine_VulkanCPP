#pragma once

#include "Common.h"

namespace primal::content
{
    struct TextureImportResult
    {
        char path[512];
        u64 handle;
        u32 width;
        u32 height;
        u32 success;
        char error_message[256];
    };

    static_assert(sizeof(TextureImportResult) == 792,
                  "TextureImportResult ABI must match PrimalEditor_Avalonia mirror (512 path + 8 handle + 4+4+4 w/h/s + 256 err + 4 trailing pad = 792)");
}

extern "C" {
    EDITOR_INTERFACE u32 InitializeAssetSystem();
    EDITOR_INTERFACE void ShutdownAssetSystem();
    EDITOR_INTERFACE u32 ImportTexture(const char* path, primal::content::TextureImportResult* out_result);

    EDITOR_INTERFACE u64 ImportMeshAsync(const char* path);

    EDITOR_INTERFACE void TryGetMeshImportResult(u64 request_id, u64* out_mesh_content_id, u32* out_status);

    EDITOR_INTERFACE void TickAssetSystem();
    EDITOR_INTERFACE u32 GetPendingUploadCount();

    EDITOR_INTERFACE void TickMainThreadJobs();

    EDITOR_INTERFACE u32 ImportSceneBinary(const char* path);
    EDITOR_INTERFACE u32 GetImportedMeshCount();
    EDITOR_INTERFACE u64 GetImportedMeshContentId(u32 index);
    EDITOR_INTERFACE u32 GetImportedMeshTexturePaths(u32 index,
                                                     const char** out_diffuse_path,
                                                     const char** out_normal_path,
                                                     const char** out_orm_path);
}