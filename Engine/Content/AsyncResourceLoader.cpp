#include "AsyncResourceLoader.h"
#include "ContentToEngine.h"

// STB image implementation - must be defined in exactly one translation unit
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "Utilities/IOStream.h"
#include <future>
#include <fstream>

namespace primal::content {

AsyncResourceLoader* AsyncResourceLoader::s_instance = nullptr;

AsyncResourceLoader::AsyncResourceLoader() = default;

AsyncResourceLoader::~AsyncResourceLoader() = default;

AsyncResourceLoader* AsyncResourceLoader::Get()
{
    return s_instance;
}

bool AsyncResourceLoader::Initialize()
{
    if (s_instance)
    {
        return true;
    }
    
    s_instance = new AsyncResourceLoader();
    return true;
}

void AsyncResourceLoader::Shutdown()
{
    if (s_instance)
    {
        delete s_instance;
        s_instance = nullptr;
    }
}

utl::vector<TextureLoadResult> AsyncResourceLoader::LoadTexturesParallel(
    const utl::vector<std::string>& paths,
    TextureProgressCallback progress_callback)
{
    utl::vector<TextureLoadResult> results(paths.size());
    std::atomic<u32> completed_count{ 0 };
    
    if (!jobsystem::JobSystem::IsRunning())
    {
        for (size_t i = 0; i < paths.size(); ++i)
        {
            results[i].path = paths[i];
            results[i].success = false;
            results[i].error_message = "Job system not initialized";
        }
        return results;
    }
    
    auto handle = jobsystem::JobSystem::ParallelFor(static_cast<u32>(paths.size()),
        [&](u32 index)
        {
            const std::string& path = paths[index];
            TextureLoadResult& result = results[index];
            result.path = path;
            
            int width, height, channels;
            u8* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
            
            if (!pixels)
            {
                result.success = false;
                result.error_message = "Failed to load texture file";
            }
            else
            {
                result.width = static_cast<u32>(width);
                result.height = static_cast<u32>(height);
                
                u32 row_pitch = static_cast<u32>(width) * 4;
                u32 slice_pitch = static_cast<u32>(height) * row_pitch;
                
                // Create proper texture blob format expected by create_resource
                // Format: width, height, array_size, flags, mip_levels, format, row_pitch, slice_pitch, pixel_data
                size_t blob_size = (6 * sizeof(u32)) + (2 * sizeof(u32) + slice_pitch);
                utl::vector<u8> blob(blob_size);
                utl::blob_stream_writer writer(blob.data(), blob.size());
                
                writer.write(static_cast<u32>(width));
                writer.write(static_cast<u32>(height));
                writer.write(1u);   // array_size
                writer.write(0u);   // flags
                writer.write(1u);   // mip_levels
                writer.write(29u);  // format = RGBA8_sRGB (29 = sRGB, 28 = UNorm)
                writer.write(row_pitch);
                writer.write(slice_pitch);
                writer.write(pixels, slice_pitch);
                
                stbi_image_free(pixels);
                
                (void)channels; // Suppress unused variable warning
                asset_type::type type = asset_type::texture;
                result.handle = create_resource(blob.data(), type);
                result.success = result.handle != id::invalid_id;
                
                if (!result.success)
                {
                    result.error_message = "Failed to create GPU resource";
                    printf("[AsyncResourceLoader] Failed for: %s\n", path.c_str());
                }
            }
            
            u32 completed = ++completed_count;
            if (progress_callback)
            {
                progress_callback(completed, static_cast<u32>(paths.size()), path);
            }
        });
    
    handle.Wait();
    
    return results;
}

jobsystem::JobHandle AsyncResourceLoader::LoadTexturesAsync(
    const utl::vector<std::string>& paths,
    TextureCompleteCallback complete_callback,
    TextureProgressCallback progress_callback)
{
    if (!jobsystem::JobSystem::IsRunning())
    {
        if (complete_callback)
        {
            utl::vector<TextureLoadResult> empty_results;
            complete_callback(empty_results);
        }
        return jobsystem::JobHandle();
    }
    
    auto results = std::make_shared<utl::vector<TextureLoadResult>>(paths.size());
    auto completed_count = std::make_shared<std::atomic<u32>>(0);
    auto total_count = static_cast<u32>(paths.size());
    auto paths_ptr = std::make_shared<utl::vector<std::string>>();
    for (const auto& p : paths) { paths_ptr->push_back(p); }
    
    printf("[AsyncResourceLoader] LoadTexturesAsync: %zu paths, results vector size: %zu\n", 
           paths.size(), results->size());
    
    // Use ParallelFor with lambda that schedules callback when last job completes
    // This avoids deadlock from waiting in a worker thread
    auto handle = jobsystem::JobSystem::ParallelFor(static_cast<u32>(paths.size()),
        [results, completed_count, total_count, paths_ptr, progress_callback, complete_callback](u32 index)
        {
            printf("[AsyncResourceLoader] Job %u started\n", index);
            const std::string& path = (*paths_ptr)[index];
            TextureLoadResult& result = (*results)[index];
            result.path = path;
            
            int width, height, channels;
            u8* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
            
            if (!pixels)
            {
                result.success = false;
                result.error_message = "Failed to load texture file";
                printf("[AsyncResourceLoader] Job %u FAILED to load: %s\n", index, path.c_str());
            }
            else
            {
                result.width = static_cast<u32>(width);
                result.height = static_cast<u32>(height);
                
                u32 row_pitch = static_cast<u32>(width) * 4;
                u32 slice_pitch = static_cast<u32>(height) * row_pitch;
                
                // Create proper texture blob format expected by create_resource
                size_t blob_size = (6 * sizeof(u32)) + (2 * sizeof(u32) + slice_pitch);
                utl::vector<u8> blob(blob_size);
                utl::blob_stream_writer writer(blob.data(), blob.size());
                
                writer.write(static_cast<u32>(width));
                writer.write(static_cast<u32>(height));
                writer.write(1u);   // array_size
                writer.write(0u);   // flags
                writer.write(1u);   // mip_levels
                writer.write(29u);  // format = RGBA8_sRGB (29 = sRGB, 28 = UNorm)
                writer.write(row_pitch);
                writer.write(slice_pitch);
                writer.write(pixels, slice_pitch);
                
                stbi_image_free(pixels);
                
                (void)channels; // Suppress unused variable warning
                asset_type::type type = asset_type::texture;
                result.handle = create_resource(blob.data(), type);
                result.success = result.handle != id::invalid_id;
                
                if (!result.success)
                {
                    result.error_message = "Failed to create GPU resource";
                }
                printf("[AsyncResourceLoader] Job %u completed: %s (%ux%u) success=%d\n", 
                       index, path.c_str(), width, height, result.success);
            }
            
            // Increment completed count and check if we're the last one
            u32 completed = ++(*completed_count);
            
            // Call progress callback
            if (progress_callback)
            {
                progress_callback(completed, total_count, path);
            }
            
            // If this is the last job, schedule the completion callback on main thread
            // This avoids deadlock - no worker thread blocks waiting for other workers
            if (completed == total_count && complete_callback)
            {
                printf("[AsyncResourceLoader] All jobs done! results size: %zu\n", results->size());
                jobsystem::JobSystem::ScheduleOnMainThread([results, complete_callback]()
                {
                    printf("[AsyncResourceLoader] MainThread callback: results size: %zu\n", results->size());
                    complete_callback(*results);
                });
            }
        });
    
    return handle;
}

jobsystem::JobHandle AsyncResourceLoader::LoadMeshAsync(
    const std::string& path,
    MeshCompleteCallback complete_callback)
{
    if (!jobsystem::JobSystem::IsRunning())
    {
        if (complete_callback)
        {
            complete_callback(id::invalid_id);
        }
        return jobsystem::JobHandle();
    }
    
    return jobsystem::JobSystem::Schedule([path, complete_callback]()
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            if (complete_callback)
            {
                jobsystem::JobSystem::ScheduleOnMainThread([complete_callback]()
                {
                    complete_callback(id::invalid_id);
                });
            }
            return;
        }
        
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        utl::vector<u8> buffer(size);
        if (!file.read(reinterpret_cast<char*>(buffer.data()), size))
        {
            if (complete_callback)
            {
                jobsystem::JobSystem::ScheduleOnMainThread([complete_callback]()
                {
                    complete_callback(id::invalid_id);
                });
            }
            return;
        }
        
        asset_type::type type = asset_type::mesh;
        id::id_type mesh_id = create_resource(buffer.data(), type);
        
        if (complete_callback)
        {
            jobsystem::JobSystem::ScheduleOnMainThread([complete_callback, mesh_id]()
            {
                complete_callback(mesh_id);
            });
        }
    });
}

void AsyncResourceLoader::ProcessPendingUploads()
{
}

u32 AsyncResourceLoader::GetPendingUploadCount() const
{
    return _pending_upload_count.load();
}

} // namespace primal::content
