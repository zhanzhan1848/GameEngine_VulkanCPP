#if defined(_MSC_VER)
#include "Common.h"
#include "CommonHeaders.h"
#include "../Engine/Content/AsyncResourceLoader.h"
#include "../Engine/Content/ContentLoader.h"
#include "../Content/ContentToEngine.h"

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
}

#elif defined(__clang__)
#include "Common.h"
#include "CommonHeaders.h"
#include "../Engine/Content/AsyncResourceLoader.h"
#include "../Engine/Content/ContentLoader.h"
#include <cstring>

namespace {
    struct TextureImportResult {
        char path[512];
        u64 handle;
        u32 width;
        u32 height;
        u32 success;
        char error_message[256];
    };
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
}

#endif
