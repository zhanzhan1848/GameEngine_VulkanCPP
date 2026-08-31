#if defined(_MSC_VER)
#include "Common.h"
#include "CommonHeaders.h"
#include "AssetAPI.h"
#include "../Engine/Content/AsyncResourceLoader.h"
#include "../Engine/Content/ContentToEngine.h"
#include "../Engine/Content/ContentLoader.h"
#include "../Content/ContentToEngine.h"
#include "../Engine/Graphics/SceneDataAdapter.h"
#include <cstring>
#include <fstream>
#include <string>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "Engine.lib")

namespace {
    primal::graphics::ImportedResources g_imported_scene{};
    std::vector<std::string> g_imported_path_storage;

    struct MeshImportRequest
    {
        std::atomic<u32> status{ 0 };
        std::atomic<u64> mesh_id{ 0 };
    };

    std::mutex g_mesh_requests_mutex;
    std::unordered_map<u64, std::shared_ptr<MeshImportRequest>> g_mesh_requests;
    std::atomic<u64> g_next_mesh_request_id{ 1 };

    void OnMeshLoadComplete(std::shared_ptr<MeshImportRequest> req, primal::id::id_type mesh_id)
    {
        if (mesh_id != primal::id::invalid_id)
        {
            req->mesh_id.store(static_cast<u64>(mesh_id));
            req->status.store(1u);
        }
        else
        {
            req->status.store(2u);
        }
    }
}

extern "C" {

    EDITOR_INTERFACE u32 InitializeAssetSystem()
    {
        return primal::content::AsyncResourceLoader::Initialize() ? 1u : 0u;
    }

    EDITOR_INTERFACE void ShutdownAssetSystem()
    {
        primal::content::AsyncResourceLoader::Shutdown();
        {
            std::lock_guard<std::mutex> lock(g_mesh_requests_mutex);
            g_mesh_requests.clear();
            g_next_mesh_request_id.store(1u);
        }
        // Content registries own GPU meshes and RHIMeshAsset free-list slots.
        // Release them while the graphics device is still alive; otherwise
        // process-exit static destructors abort on non-empty free lists.
        primal::content::shutdown();
    }

    EDITOR_INTERFACE u32 ImportTexture(const char* path, primal::content::TextureImportResult* out_result)
    {
        if (!path || !out_result) return 0u;
        memset(out_result, 0, sizeof(primal::content::TextureImportResult));
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

    EDITOR_INTERFACE u64 ImportCookedTexture(const char* path)
    {
        if (!path) return static_cast<u64>(primal::id::invalid_id);
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return static_cast<u64>(primal::id::invalid_id);
        const auto size = file.tellg();
        if (size <= 0) return static_cast<u64>(primal::id::invalid_id);
        file.seekg(0, std::ios::beg);
        std::vector<u8> bytes(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
            return static_cast<u64>(primal::id::invalid_id);
        return static_cast<u64>(primal::content::create_resource(
            bytes.data(), primal::content::asset_type::texture));
    }

    EDITOR_INTERFACE u64 ImportMeshAsync(const char* path)
    {
        if (!path) return 0u;

        const u64 request_id = g_next_mesh_request_id.fetch_add(1u);
        auto req = std::make_shared<MeshImportRequest>();

        {
            std::lock_guard<std::mutex> lock(g_mesh_requests_mutex);
            g_mesh_requests[request_id] = req;
        }

        auto callback = [req](primal::id::id_type mesh_id)
        {
            OnMeshLoadComplete(req, mesh_id);
        };

        auto handle = primal::content::AsyncResourceLoader::Get()
            ->LoadMeshAsync(std::string(path), callback);

        if (!handle)
        {
            std::lock_guard<std::mutex> lock(g_mesh_requests_mutex);
            g_mesh_requests.erase(request_id);
            return 0u;
        }

        return request_id;
    }

    EDITOR_INTERFACE void TryGetMeshImportResult(u64 request_id, u64* out_mesh_content_id, u32* out_status)
    {
        if (!out_mesh_content_id || !out_status) return;

        std::shared_ptr<MeshImportRequest> req;
        {
            std::lock_guard<std::mutex> lock(g_mesh_requests_mutex);
            auto it = g_mesh_requests.find(request_id);
            if (it == g_mesh_requests.end())
            {
                *out_mesh_content_id = 0u;
                *out_status = 3u;
                return;
            }
            req = it->second;
        }

        *out_mesh_content_id = req->mesh_id.load();
        *out_status = req->status.load();
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

    EDITOR_INTERFACE void TickMainThreadJobs()
    {
        primal::jobsystem::JobSystem::ProcessMainThreadJobs();
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
#include "AssetAPI.h"
#include "../Engine/Content/AsyncResourceLoader.h"
#include "../Engine/Content/ContentToEngine.h"
#include "../Engine/Content/ContentLoader.h"
#include "../Engine/Graphics/SceneDataAdapter.h"
#include <atomic>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
    primal::graphics::ImportedResources g_imported_scene{};
    std::vector<std::string> g_imported_path_storage;

    struct MeshImportRequest
    {
        std::atomic<u32> status{ 0 };
        std::atomic<u64> mesh_id{ 0 };
    };

    std::mutex g_mesh_requests_mutex;
    std::unordered_map<u64, std::shared_ptr<MeshImportRequest>> g_mesh_requests;
    std::atomic<u64> g_next_mesh_request_id{ 1 };

    void OnMeshLoadComplete(std::shared_ptr<MeshImportRequest> req, primal::id::id_type mesh_id)
    {
        if (mesh_id != primal::id::invalid_id)
        {
            req->mesh_id.store(static_cast<u64>(mesh_id));
            req->status.store(1u);
        }
        else
        {
            req->status.store(2u);
        }
    }
}

extern "C" {

    EDITOR_INTERFACE u32 InitializeAssetSystem()
    {
        return primal::content::AsyncResourceLoader::Initialize() ? 1u : 0u;
    }

    EDITOR_INTERFACE void ShutdownAssetSystem()
    {
        primal::content::AsyncResourceLoader::Shutdown();
        {
            std::lock_guard<std::mutex> lock(g_mesh_requests_mutex);
            g_mesh_requests.clear();
            g_next_mesh_request_id.store(1u);
        }
        // Content registries own GPU meshes and RHIMeshAsset free-list slots.
        // Release them while the graphics device is still alive; otherwise
        // process-exit static destructors abort on non-empty free lists.
        primal::content::shutdown();
    }

    EDITOR_INTERFACE u32 ImportTexture(const char* path, primal::content::TextureImportResult* out_result)
    {
        if (!path || !out_result) return 0u;
        std::memset(out_result, 0, sizeof(primal::content::TextureImportResult));
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

    EDITOR_INTERFACE u64 ImportCookedTexture(const char* path)
    {
        if (!path) return static_cast<u64>(primal::id::invalid_id);
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return static_cast<u64>(primal::id::invalid_id);
        const auto size = file.tellg();
        if (size <= 0) return static_cast<u64>(primal::id::invalid_id);
        file.seekg(0, std::ios::beg);
        std::vector<u8> bytes(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
            return static_cast<u64>(primal::id::invalid_id);
        return static_cast<u64>(primal::content::create_resource(
            bytes.data(), primal::content::asset_type::texture));
    }

    EDITOR_INTERFACE u64 ImportMeshAsync(const char* path)
    {
        if (!path) return 0u;

        const u64 request_id = g_next_mesh_request_id.fetch_add(1u);
        auto req = std::make_shared<MeshImportRequest>();

        {
            std::lock_guard<std::mutex> lock(g_mesh_requests_mutex);
            g_mesh_requests[request_id] = req;
        }

        auto callback = [req](primal::id::id_type mesh_id)
        {
            OnMeshLoadComplete(req, mesh_id);
        };

        auto handle = primal::content::AsyncResourceLoader::Get()
            ->LoadMeshAsync(std::string(path), callback);

        if (!handle)
        {
            std::lock_guard<std::mutex> lock(g_mesh_requests_mutex);
            g_mesh_requests.erase(request_id);
            return 0u;
        }

        return request_id;
    }

    EDITOR_INTERFACE void TryGetMeshImportResult(u64 request_id, u64* out_mesh_content_id, u32* out_status)
    {
        if (!out_mesh_content_id || !out_status) return;

        std::shared_ptr<MeshImportRequest> req;
        {
            std::lock_guard<std::mutex> lock(g_mesh_requests_mutex);
            auto it = g_mesh_requests.find(request_id);
            if (it == g_mesh_requests.end())
            {
                *out_mesh_content_id = 0u;
                *out_status = 3u;
                return;
            }
            req = it->second;
        }

        *out_mesh_content_id = req->mesh_id.load();
        *out_status = req->status.load();
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

    EDITOR_INTERFACE void TickMainThreadJobs()
    {
        primal::jobsystem::JobSystem::ProcessMainThreadJobs();
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
