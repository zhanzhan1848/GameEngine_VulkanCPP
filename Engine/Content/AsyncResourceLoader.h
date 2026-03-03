#pragma once

#include "CommonHeaders.h"
#include "../JobSystem/JobSystem.h"
#include <functional>
#include <future>
#include <string>
#include <string>

namespace primal::content {

struct TextureLoadResult
{
    std::string path;
    id::id_type handle{ id::invalid_id };
    u32 width{ 0 };
    u32 height{ 0 };
    bool success{ false };
    std::string error_message;
};

using TextureProgressCallback = std::function<void(u32 completed, u32 total, const std::string& current_file)>;
using TextureCompleteCallback = std::function<void(const utl::vector<TextureLoadResult>& results)>;
using MeshCompleteCallback = std::function<void(id::id_type mesh_id)>;

class AsyncResourceLoader
{
public:
    static AsyncResourceLoader* Get();
    
    static bool Initialize();
    static void Shutdown();
    
    // Load multiple textures in parallel
    // Returns a vector of results, blocks until all are complete
    utl::vector<TextureLoadResult> LoadTexturesParallel(
        const utl::vector<std::string>& paths,
        TextureProgressCallback progress_callback = nullptr);
    
    // Load multiple textures asynchronously
    // Returns immediately with a JobHandle
    // Completion callback is called when all textures are loaded
    jobsystem::JobHandle LoadTexturesAsync(
        const utl::vector<std::string>& paths,
        TextureCompleteCallback complete_callback,
        TextureProgressCallback progress_callback = nullptr);
    
    // Load a single mesh asynchronously
    // Mesh is loaded in background, callback is invoked on main thread
    jobsystem::JobHandle LoadMeshAsync(
        const std::string& path,
        MeshCompleteCallback complete_callback);
    
    // Process any pending GPU uploads (call from main thread each frame)
    void ProcessPendingUploads();
    
    // Get number of pending uploads
    u32 GetPendingUploadCount() const;
    
private:
    AsyncResourceLoader();
    ~AsyncResourceLoader();
    
    DISABLE_COPY(AsyncResourceLoader);
    DISABLE_MOVE(AsyncResourceLoader);
    
    static AsyncResourceLoader* s_instance;
    
    struct PendingTextureUpload
    {
        utl::vector<u8> data;
        u32 width;
        u32 height;
        std::string path;
        std::promise<TextureLoadResult> promise;
    };
    
    struct PendingMeshUpload
    {
        utl::vector<u8> data;
        std::string path;
        std::promise<id::id_type> promise;
    };
    
    std::queue<PendingTextureUpload> _pending_texture_uploads;
    std::queue<PendingMeshUpload> _pending_mesh_uploads;
    std::mutex _upload_mutex;
    std::atomic<u32> _pending_upload_count{ 0 };
};

#define g_AsyncResourceLoader primal::content::AsyncResourceLoader::Get()

} // namespace primal::content
