// Stubs + side-table implementations for content APIs excluded from the
// Emscripten build (ContentToEngine.cpp uses a fake_pointer encoding that
// assumes sizeof(uintptr_t) > sizeof(id::id_type), which fails on wasm32).
#ifdef __EMSCRIPTEN__

#include "Engine/Common/CommonHeaders.h"
#include "Engine/Content/ContentToEngine.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHIMeshAsset.h"
#include "Engine/Graphics/RHI/Core/RHIGpuMesh.h"
#include "Engine/Graphics/RHI/Core/RHISystem.h"
#include "Engine/Graphics/RHI/Platforms/Dawn/DawnDevice.h"
#include "Engine/Graphics/Nanite/NaniteResourceManager.h"
#include "Engine/Utilities/IOStream.h"

#include <unordered_map>
#include <memory>
#include <mutex>

namespace primal::content {
namespace {

// Side-tables replacing ContentToEngine.cpp's free_list + pointer-tagging
// scheme. Plain maps keyed by the id register_mesh_asset hands out.
std::mutex& asset_mutex() {
    static std::mutex m;
    return m;
}
std::unordered_map<id::id_type, graphics::rhi::RHIMeshAsset>& asset_table() {
    static std::unordered_map<id::id_type, graphics::rhi::RHIMeshAsset> t;
    return t;
}
std::unordered_map<id::id_type, std::unique_ptr<graphics::rhi::RHIGpuMesh>>& gpu_mesh_table() {
    static std::unordered_map<id::id_type, std::unique_ptr<graphics::rhi::RHIGpuMesh>> t;
    return t;
}
id::id_type next_mesh_id{1};

// Texture side-table: id::id_type -> RHI texture handle. Mirrors the mesh
// pattern. Without this, create_resource / get_rhi_texture_handle are no-ops
// and every material falls back to white_texture_.
std::unordered_map<id::id_type, graphics::rhi::ResourceHandle>& texture_table() {
    static std::unordered_map<id::id_type, graphics::rhi::ResourceHandle> t;
    return t;
}
id::id_type next_texture_id{0x80000001};

graphics::rhi::RHIDeviceBase* first_device() {
    if (graphics::rhi::g_deviceManager.GetDeviceCount() == 0) return nullptr;
    // nextDeviceId_ starts at 1, so the first registered device is id 1.
    auto* d = graphics::rhi::g_deviceManager.GetDevice(1);
    return d ? d : graphics::rhi::g_deviceManager.GetDevice(0);
}

} // anonymous namespace

id::id_type register_mesh_asset(graphics::rhi::RHIMeshAsset& asset) {
    std::lock_guard<std::mutex> lock(asset_mutex());
    id::id_type id = next_mesh_id++;
    asset_table()[id] = std::move(asset);
    return id;
}

void foreach_gpu_mesh(GpuMeshCallback callback) {
    if (!callback) return;
    std::lock_guard<std::mutex> lock(asset_mutex());
    for (auto& [id, mesh] : gpu_mesh_table()) {
        callback(id, mesh.get());
    }
}

bool get_rhi_mesh_asset(id::id_type id, graphics::rhi::RHIMeshAsset& out) {
    std::lock_guard<std::mutex> lock(asset_mutex());
    auto it = asset_table().find(id);
    if (it == asset_table().end()) return false;
    out = it->second;
    return true;
}

graphics::rhi::RHIGpuMesh* get_rhi_gpu_mesh(id::id_type id) {
    {
        std::lock_guard<std::mutex> lock(asset_mutex());
        auto it = gpu_mesh_table().find(id);
        if (it != gpu_mesh_table().end()) {
            return it->second.get();
        }
    }

    // Lazily create from CPU asset. Mirrors ContentToEngine.cpp:926-955.
    if (graphics::rhi::g_deviceManager.GetDeviceCount() == 0) return nullptr;

    graphics::rhi::RHIMeshAsset asset;
    if (!get_rhi_mesh_asset(id, asset)) return nullptr;

    // nextDeviceId_ starts at 1, so the first registered device is id 1.
    auto* device = graphics::rhi::g_deviceManager.GetDevice(1);
    if (!device) {
        device = graphics::rhi::g_deviceManager.GetDevice(0);
    }
    if (!device) return nullptr;

    auto mesh = std::make_unique<graphics::rhi::RHIGpuMesh>();
    if (!mesh->Initialize(*device, asset)) {
        return nullptr;
    }

    auto* result = mesh.get();
    std::lock_guard<std::mutex> lock(asset_mutex());
    gpu_mesh_table()[id] = std::move(mesh);
    return result;
}

graphics::rhi::ResourceHandle get_rhi_texture_handle(id::id_type id) {
    std::lock_guard<std::mutex> lock(asset_mutex());
    auto it = texture_table().find(id);
    if (it == texture_table().end()) return graphics::rhi::handles::INVALID_RESOURCE;
    return it->second;
}

// The metal ContentToEngine encodes RHI id into a fake pointer; on WASM the side-table
// scheme uses the same id for both layers, so get_rhi_mesh_id is the identity function.
id::id_type get_rhi_mesh_id(id::id_type geometry_id) {
    return geometry_id;
}

id::id_type create_resource(const void* data, asset_type::type type, GraphicsAPI api) {
    if (!data) return id::invalid_id;
    if (type != asset_type::texture) return id::invalid_id;

    auto* device = first_device();
    if (!device) {
        std::cerr << "[StubsEmscripten] create_resource: no device registered" << std::endl;
        return id::invalid_id;
    }

    // Mirror ContentToEngine.cpp Dawn branch (880-948).
    utl::blob_stream_reader blob((const u8*)data);
    const u32 width{ blob.read<u32>() };
    const u32 height{ blob.read<u32>() };
    const u32 array_size{ blob.read<u32>() };
    [[maybe_unused]] const u32 flags{ blob.read<u32>() };
    const u32 mip_levels{ blob.read<u32>() };
    const u32 format_u32{ blob.read<u32>() };

    graphics::rhi::TextureDesc desc{};
    desc.size = { width, height, 1 };
    desc.arraySize = array_size;
    desc.mipLevels = mip_levels;
    desc.type = (array_size > 1) ? graphics::rhi::TextureType::Texture2DArray
                                 : graphics::rhi::TextureType::Texture2D;

    if (format_u32 == 28) desc.format = graphics::rhi::DataFormat::RGBA8_UNorm;
    else if (format_u32 == 29) desc.format = graphics::rhi::DataFormat::RGBA8_sRGB;
    else if (format_u32 == 71) desc.format = graphics::rhi::DataFormat::BC1_UNorm;
    else if (format_u32 == 72) desc.format = graphics::rhi::DataFormat::BC1_sRGB;
    else if (format_u32 == 98) desc.format = graphics::rhi::DataFormat::BC7_UNorm;
    else if (format_u32 == 99) desc.format = graphics::rhi::DataFormat::BC7_sRGB;
    else desc.format = graphics::rhi::DataFormat::RGBA8_UNorm;

    desc.usage = graphics::rhi::TextureUsage::ShaderResource
               | graphics::rhi::TextureUsage::CopyDest
               | graphics::rhi::TextureUsage::CopySource;

    auto handle = device->CreateTexture(desc);
    if (handle == graphics::rhi::handles::INVALID_RESOURCE) {
        std::cerr << "[StubsEmscripten] CreateTexture failed w=" << width << " h=" << height
                  << " fmt=" << format_u32 << std::endl;
        return id::invalid_id;
    }

    auto* dawnDevice = static_cast<graphics::rhi::DawnDevice*>(device);
    for (u32 i{0}; i < array_size; ++i) {
        for (u32 j{0}; j < mip_levels; ++j) {
            const u32 row_pitch{ blob.read<u32>() };
            const u32 slice_pitch{ blob.read<u32>() };
            u32 mipWidth = std::max(1u, width >> j);
            u32 mipHeight = std::max(1u, height >> j);
            dawnDevice->UpdateTextureData(handle, blob.position(),
                0, 0, i, mipWidth, mipHeight, 1, row_pitch, j);
            blob.skip(slice_pitch);
        }
    }

    id::id_type new_id;
    {
        std::lock_guard<std::mutex> lock(asset_mutex());
        new_id = next_texture_id++;
        texture_table()[new_id] = handle;
    }
    return new_id;
}

void shutdown() {
    // Stub: WASM side-tables (asset_table, gpu_mesh_table) tear down at process
    // exit. Native ContentToEngine.cpp::shutdown() drains those maps under
    // mutex; here the maps are static locals in accessor functions and are
    // cleaned up by their destructors at static-storage teardown.
}

} // namespace primal::content

// NaniteResourceManager is now compiled for WASM (CMakeLists filter removed);
// AddGeometryRef / ReleaseGeometryRef / GetOrCreateResource have real impls.

#endif // __EMSCRIPTEN__
