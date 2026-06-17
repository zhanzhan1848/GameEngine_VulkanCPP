#if defined(_MSC_VER)
#include "Common.h"
#include "CommonHeaders.h"
#include "../Engine/Content/AsyncResourceLoader.h"
#include "../Engine/Content/ContentLoader.h"
#include "../Content/ContentToEngine.h"
#include "../Engine/Graphics/SceneDataAdapter.h"
#include <fstream>
#include <vector>

#pragma comment(lib, "Engine.lib")

namespace {
    struct TextureImportResult {
        char path[512];
        u64 handle;
        u32 width;
        u32 height;
        u32 success;
        char error_message[256];
    };

    // Single-graph scene import cache. PCG-style: one global state, overwritten per ImportSceneBinary call.
    primal::graphics::ImportedResources g_imported_scene{};
    std::vector<std::string> g_imported_path_storage;  // keeps c_str() alive for GetImportedMeshTexturePaths
}

extern "C" {

    EDITOR_INTERFACE u32 InitializeAssetSystem()
    {
        return primal::content::AsyncResourceLoader::Initialize() ? 1u : 0u;
    }

    EDITOR_INTERFACE void ShutdownAssetSystem()
    {
        primal::content::AsyncResourceLoader::Shutdown();
    }

    EDITOR_INTERFACE u32 ImportTexture(const char* path, TextureImportResult* out_result)
    {
        if (!path || !out_result) return 0u;
        memset(out_result, 0, sizeof(TextureImportResult));
        strncpy(out_result->path, path, sizeof(out_result->path) - 1);

        primal::utl::vector<std::string> paths;
        paths.push_back(std::string(path));
        auto results = primal::content::AsyncResourceLoader::Get()
            ->LoadTexturesParallel(paths);
        if (results.empty() || !results[0].success) {
            if (!results.empty()) {
                strncpy(out_result->error_message,
                    results[0].error_message.c_str(),
                    sizeof(out_result->error_message) - 1);
            }
            return 0u;
        }

        const auto& r = results[0];
        out_result->handle = r.handle;
        out_result->width = r.width;
        out_result->height = r.height;
        out_result->success = 1u;
        return 1u;
    }

    EDITOR_INTERFACE u64 ImportMeshAsync(const char* path)
    {
        if (!path) return 0u;
        auto handle = primal::content::AsyncResourceLoader::Get()
            ->LoadMeshAsync(std::string(path), nullptr);
        return handle ? 1u : 0u;
    }

    EDITOR_INTERFACE void TickAssetSystem()
    {
        if (auto* loader = primal::content::AsyncResourceLoader::Get()) {
            loader->ProcessPendingUploads();
        }
    }

    EDITOR_INTERFACE u32 GetPendingUploadCount()
    {
        auto* loader = primal::content::AsyncResourceLoader::Get();
        return loader ? loader->GetPendingUploadCount() : 0u;
    }

    // Import a scene binary file. Replaces any previously imported scene.
    // Returns 1 on success, 0 on failure (file missing / parse error).
    EDITOR_INTERFACE u32 ImportSceneBinary(const char* path) {
        if (!path) return 0u;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return 0u;
        const auto sz = f.tellg();
        if (sz <= 0) return 0u;
        f.seekg(0, std::ios::beg);
        std::vector<char> buf(static_cast<size_t>(sz));
        if (!f.read(buf.data(), sz)) return 0u;

        g_imported_scene = primal::graphics::SceneDataAdapter::ImportResources(buf.data(), static_cast<u32>(sz));
        return g_imported_scene.meshes.empty() ? 0u : 1u;
    }

    EDITOR_INTERFACE u32 GetImportedMeshCount() {
        return static_cast<u32>(g_imported_scene.meshes.size());
    }

    EDITOR_INTERFACE u64 GetImportedMeshContentId(u32 index) {
        if (index >= g_imported_scene.meshes.size()) return 0u;
        return static_cast<u64>(g_imported_scene.meshes[index].mesh_content_id);
    }

    EDITOR_INTERFACE u32 GetImportedMeshTexturePaths(u32 index,
                                                     const char** out_diffuse_path,
                                                     const char** out_normal_path,
                                                     const char** out_orm_path) {
        if (index >= g_imported_scene.meshes.size()) return 0u;
        const auto& m = g_imported_scene.meshes[index];
        if (out_diffuse_path) *out_diffuse_path = m.diffuse_path.c_str();
        if (out_normal_path)  *out_normal_path  = m.normal_path.c_str();
        if (out_orm_path)     *out_orm_path     = m.orm_path.c_str();
        return 1u;
    }
}

#elif defined(__clang__)
#include "Common.h"
#include "CommonHeaders.h"
#include "../Engine/Content/AsyncResourceLoader.h"
#include "../Engine/Content/ContentLoader.h"
#include "../Engine/Graphics/SceneDataAdapter.h"
#include <cstring>
#include <fstream>
#include <vector>

namespace {
    struct TextureImportResult {
        char path[512];
        u64 handle;
        u32 width;
        u32 height;
        u32 success;
        char error_message[256];
    };

    primal::graphics::ImportedResources g_imported_scene{};
    std::vector<std::string> g_imported_path_storage;
}

extern "C" {

    EDITOR_INTERFACE u32 InitializeAssetSystem()
    {
        return primal::content::AsyncResourceLoader::Initialize() ? 1u : 0u;
    }

    EDITOR_INTERFACE void ShutdownAssetSystem()
    {
        primal::content::AsyncResourceLoader::Shutdown();
    }

    EDITOR_INTERFACE u32 ImportTexture(const char* path, TextureImportResult* out_result)
    {
        if (!path || !out_result) return 0u;
        std::memset(out_result, 0, sizeof(TextureImportResult));
        std::strncpy(out_result->path, path, sizeof(out_result->path) - 1);

        primal::utl::vector<std::string> paths;
        paths.push_back(std::string(path));
        auto results = primal::content::AsyncResourceLoader::Get()
            ->LoadTexturesParallel(paths);
        if (results.empty() || !results[0].success) {
            if (!results.empty()) {
                std::strncpy(out_result->error_message,
                    results[0].error_message.c_str(),
                    sizeof(out_result->error_message) - 1);
            }
            return 0u;
        }

        const auto& r = results[0];
        out_result->handle = r.handle;
        out_result->width = r.width;
        out_result->height = r.height;
        out_result->success = 1u;
        return 1u;
    }

    EDITOR_INTERFACE u64 ImportMeshAsync(const char* path)
    {
        if (!path) return 0u;
        auto handle = primal::content::AsyncResourceLoader::Get()
            ->LoadMeshAsync(std::string(path), nullptr);
        return handle ? 1u : 0u;
    }

    EDITOR_INTERFACE void TickAssetSystem()
    {
        if (auto* loader = primal::content::AsyncResourceLoader::Get()) {
            loader->ProcessPendingUploads();
        }
    }

    EDITOR_INTERFACE u32 GetPendingUploadCount()
    {
        auto* loader = primal::content::AsyncResourceLoader::Get();
        return loader ? loader->GetPendingUploadCount() : 0u;
    }

    EDITOR_INTERFACE u32 ImportSceneBinary(const char* path) {
        if (!path) return 0u;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return 0u;
        const auto sz = f.tellg();
        if (sz <= 0) return 0u;
        f.seekg(0, std::ios::beg);
        std::vector<char> buf(static_cast<size_t>(sz));
        if (!f.read(buf.data(), sz)) return 0u;

        g_imported_scene = primal::graphics::SceneDataAdapter::ImportResources(buf.data(), static_cast<u32>(sz));
        return g_imported_scene.meshes.empty() ? 0u : 1u;
    }

    EDITOR_INTERFACE u32 GetImportedMeshCount() {
        return static_cast<u32>(g_imported_scene.meshes.size());
    }

    EDITOR_INTERFACE u64 GetImportedMeshContentId(u32 index) {
        if (index >= g_imported_scene.meshes.size()) return 0u;
        return static_cast<u64>(g_imported_scene.meshes[index].mesh_content_id);
    }

    EDITOR_INTERFACE u32 GetImportedMeshTexturePaths(u32 index,
                                                     const char** out_diffuse_path,
                                                     const char** out_normal_path,
                                                     const char** out_orm_path) {
        if (index >= g_imported_scene.meshes.size()) return 0u;
        const auto& m = g_imported_scene.meshes[index];
        if (out_diffuse_path) *out_diffuse_path = m.diffuse_path.c_str();
        if (out_normal_path)  *out_normal_path  = m.normal_path.c_str();
        if (out_orm_path)     *out_orm_path     = m.orm_path.c_str();
        return 1u;
    }
}

#endif
