#include "RHIGpuMesh.h"
#include "RHICommand.h"
#include <cstring>
#include <iostream>

namespace primal::graphics::rhi
{
    RHIGpuMesh::RHIGpuMesh(RHIGpuMesh&& other) noexcept
        : device_(other.device_)
        , position_buffer_(other.position_buffer_)
        , element_buffer_(other.element_buffer_)
        , index_buffer_(other.index_buffer_)
        , meshlet_buffer_(other.meshlet_buffer_)
        , meshlet_vertices_buffer_(other.meshlet_vertices_buffer_)
        , meshlet_triangles_buffer_(other.meshlet_triangles_buffer_)
        , sdf_texture_(other.sdf_texture_)
        , voxel_texture_(other.voxel_texture_)
        , vector_field_texture_(other.vector_field_texture_)
        , index_count_(other.index_count_)
        , vertex_count_(other.vertex_count_)
        , meshlet_count_(other.meshlet_count_)
    {
        memcpy(sdf_resolution_, other.sdf_resolution_, sizeof(sdf_resolution_));
        memcpy(vector_field_resolution_, other.vector_field_resolution_, sizeof(vector_field_resolution_));

        other.device_ = nullptr;
        other.position_buffer_ = handles::INVALID_RESOURCE;
        other.element_buffer_ = handles::INVALID_RESOURCE;
        other.index_buffer_ = handles::INVALID_RESOURCE;
        other.meshlet_buffer_ = handles::INVALID_RESOURCE;
        other.meshlet_vertices_buffer_ = handles::INVALID_RESOURCE;
        other.meshlet_triangles_buffer_ = handles::INVALID_RESOURCE;
        other.sdf_texture_ = handles::INVALID_RESOURCE;
        other.voxel_texture_ = handles::INVALID_RESOURCE;
        other.vector_field_texture_ = handles::INVALID_RESOURCE;
        other.index_count_ = 0;
        other.vertex_count_ = 0;
        other.meshlet_count_ = 0;
        memset(other.sdf_resolution_, 0, sizeof(other.sdf_resolution_));
        memset(other.vector_field_resolution_, 0, sizeof(other.vector_field_resolution_));
    }

    RHIGpuMesh& RHIGpuMesh::operator=(RHIGpuMesh&& other) noexcept
    {
        if (this != &other)
        {
            Destroy();

            device_ = other.device_;
            position_buffer_ = other.position_buffer_;
            element_buffer_ = other.element_buffer_;
            index_buffer_ = other.index_buffer_;
            meshlet_buffer_ = other.meshlet_buffer_;
            meshlet_vertices_buffer_ = other.meshlet_vertices_buffer_;
            meshlet_triangles_buffer_ = other.meshlet_triangles_buffer_;
            sdf_texture_ = other.sdf_texture_;
            voxel_texture_ = other.voxel_texture_;
            vector_field_texture_ = other.vector_field_texture_;
            index_count_ = other.index_count_;
            vertex_count_ = other.vertex_count_;
            meshlet_count_ = other.meshlet_count_;
            memcpy(sdf_resolution_, other.sdf_resolution_, sizeof(sdf_resolution_));
            memcpy(vector_field_resolution_, other.vector_field_resolution_, sizeof(vector_field_resolution_));
            memcpy(bounds_min_, other.bounds_min_, sizeof(bounds_min_));
            memcpy(bounds_max_, other.bounds_max_, sizeof(bounds_max_));

            other.device_ = nullptr;
            other.position_buffer_ = handles::INVALID_RESOURCE;
            other.element_buffer_ = handles::INVALID_RESOURCE;
            other.index_buffer_ = handles::INVALID_RESOURCE;
            other.meshlet_buffer_ = handles::INVALID_RESOURCE;
            other.meshlet_vertices_buffer_ = handles::INVALID_RESOURCE;
            other.meshlet_triangles_buffer_ = handles::INVALID_RESOURCE;
            other.sdf_texture_ = handles::INVALID_RESOURCE;
            other.voxel_texture_ = handles::INVALID_RESOURCE;
            other.vector_field_texture_ = handles::INVALID_RESOURCE;
            other.index_count_ = 0;
            other.vertex_count_ = 0;
            other.meshlet_count_ = 0;
            memset(other.sdf_resolution_, 0, sizeof(other.sdf_resolution_));
            memset(other.vector_field_resolution_, 0, sizeof(other.vector_field_resolution_));
            memset(other.bounds_min_, 0, sizeof(other.bounds_min_));
            memset(other.bounds_max_, 0, sizeof(other.bounds_max_));
        }
        return *this;
    }

    RHIGpuMesh::~RHIGpuMesh()
    {
        Destroy();
    }

    void RHIGpuMesh::Destroy()
    {
        if (device_)
        {
            if (position_buffer_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(position_buffer_);
            if (element_buffer_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(element_buffer_);
            if (index_buffer_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(index_buffer_);
            if (meshlet_buffer_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(meshlet_buffer_);
            if (meshlet_vertices_buffer_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(meshlet_vertices_buffer_);
            if (meshlet_triangles_buffer_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(meshlet_triangles_buffer_);
            if (sdf_texture_ != handles::INVALID_RESOURCE) device_->DestroyTexture(sdf_texture_);
            if (voxel_texture_ != handles::INVALID_RESOURCE) device_->DestroyTexture(voxel_texture_);
            if (vector_field_texture_ != handles::INVALID_RESOURCE) device_->DestroyTexture(vector_field_texture_);
        }

        position_buffer_ = handles::INVALID_RESOURCE;
        element_buffer_ = handles::INVALID_RESOURCE;
        index_buffer_ = handles::INVALID_RESOURCE;
        meshlet_buffer_ = handles::INVALID_RESOURCE;
        meshlet_vertices_buffer_ = handles::INVALID_RESOURCE;
        meshlet_triangles_buffer_ = handles::INVALID_RESOURCE;
        sdf_texture_ = handles::INVALID_RESOURCE;
        voxel_texture_ = handles::INVALID_RESOURCE;
        vector_field_texture_ = handles::INVALID_RESOURCE;
        device_ = nullptr;
        index_count_ = 0;
        vertex_count_ = 0;
        meshlet_count_ = 0;
        memset(sdf_resolution_, 0, sizeof(sdf_resolution_));
        memset(vector_field_resolution_, 0, sizeof(vector_field_resolution_));
        memset(bounds_min_, 0, sizeof(bounds_min_));
        memset(bounds_max_, 0, sizeof(bounds_max_));
    }

    bool RHIGpuMesh::Initialize(RHIDeviceBase& device, const RHIMeshAsset& asset)
    {
        Destroy();
        device_ = &device;

        index_count_ = asset.num_indices;
        vertex_count_ = asset.num_vertices;
        meshlet_count_ = (u32)asset.meshlets.size();

        // Position Buffer
        if (!asset.position_buffer.empty())
        {
            if (!CreateAndUploadBuffer(asset.position_buffer.data(), asset.position_buffer.size(), position_buffer_, BufferUsageFlags::Vertex | BufferUsageFlags::Storage, "MeshPositionBuffer")) return false;
        }

        // Element Buffer
        if (!asset.element_buffer.empty())
        {
            if (!CreateAndUploadBuffer(asset.element_buffer.data(), asset.element_buffer.size(), element_buffer_, BufferUsageFlags::Vertex, "MeshElementBuffer")) return false;
        }

        // Index Buffer
        if (!asset.index_buffer.empty())
        {
            if (!CreateAndUploadBuffer(asset.index_buffer.data(), asset.index_buffer.size(), index_buffer_, BufferUsageFlags::Index, "MeshIndexBuffer")) return false;
        }

        // 1. Meshlet Buffer
        if (!asset.meshlets.empty())
        {
            if (!CreateAndUploadBuffer(asset.meshlets.data(), asset.meshlets.size() * sizeof(RHIMeshlet), meshlet_buffer_, BufferUsageFlags::Storage, "MeshletBuffer")) return false;
            meshlet_count_ = (u32)asset.meshlets.size();
        }

        // 2. Meshlet Vertices Buffer
        if (!asset.meshlet_vertices.empty())
        {
            if (!CreateAndUploadBuffer(asset.meshlet_vertices.data(), asset.meshlet_vertices.size() * sizeof(u32), meshlet_vertices_buffer_, BufferUsageFlags::Storage, "MeshletVerticesBuffer")) return false;
        }

        // 3. Meshlet Triangles Buffer
        if (!asset.meshlet_triangles.empty())
        {
            if (!CreateAndUploadBuffer(asset.meshlet_triangles.data(), asset.meshlet_triangles.size() * sizeof(u8), meshlet_triangles_buffer_, BufferUsageFlags::Storage, "MeshletTrianglesBuffer")) return false;
        }

        // Initialize bounds and resolutions
        memcpy(sdf_resolution_, asset.sdf.resolution, sizeof(sdf_resolution_));
        memcpy(voxel_resolution_, asset.sdf.resolution, sizeof(voxel_resolution_));
        memcpy(vector_field_resolution_, asset.sdf.resolution, sizeof(vector_field_resolution_));
        
        memcpy(bounds_min_, asset.sdf.bounds_min, sizeof(bounds_min_));
        memcpy(bounds_max_, asset.sdf.bounds_max, sizeof(bounds_max_));

        // SDF Texture
        if (!asset.sdf.data.empty())
        {
            // Assume 16-bit float format (Half Float)
            if (CreateAndUploadTexture3D(asset.sdf.data.data(), asset.sdf.resolution[0], asset.sdf.resolution[1], asset.sdf.resolution[2], DataFormat::R16_Float, sdf_texture_, "SDFTexture")) {
                sdf_resolution_[0] = asset.sdf.resolution[0];
                sdf_resolution_[1] = asset.sdf.resolution[1];
                sdf_resolution_[2] = asset.sdf.resolution[2];
                std::cout << "RHIGpuMesh: SDF Texture uploaded successfully. Res: " << sdf_resolution_[0] << "x" << sdf_resolution_[1] << "x" << sdf_resolution_[2] << std::endl;
            } else {
                 std::cerr << "RHIGpuMesh: Failed to upload SDF Texture." << std::endl;
                 return false;
            }
        }
        else 
        {
            // std::cout << "RHIGpuMesh: No SDF data to upload." << std::endl;
        }

        // Voxel Texture
        if (!asset.sdf.voxels.empty())
        {
            // Assume R8_UNorm for voxel density/occupancy
            if (CreateAndUploadTexture3D(asset.sdf.voxels.data(), asset.sdf.resolution[0], asset.sdf.resolution[1], asset.sdf.resolution[2], DataFormat::R8_UNorm, voxel_texture_, "VoxelTexture")) {
                voxel_resolution_[0] = asset.sdf.resolution[0];
                voxel_resolution_[1] = asset.sdf.resolution[1];
                voxel_resolution_[2] = asset.sdf.resolution[2];
                std::cout << "RHIGpuMesh: Voxel Texture uploaded successfully." << std::endl;
            } else {
                return false;
            }
        }

        // Vector Field Texture
        if (!asset.sdf.vector_field.empty())
        {
            u32 total_elements = (u32)asset.sdf.vector_field.size();
            u32 voxel_count = asset.sdf.resolution[0] * asset.sdf.resolution[1] * asset.sdf.resolution[2];
            u32 channels = (voxel_count > 0) ? (total_elements / voxel_count) : 0;

            DataFormat format = DataFormat::Unknown;
            if (channels == 1) format = DataFormat::R16_Float;
            else if (channels == 2) format = DataFormat::RG16_Float;
            else if (channels == 4) format = DataFormat::RGBA16_Float;

            if (format != DataFormat::Unknown)
            {
                if (CreateAndUploadTexture3D(asset.sdf.vector_field.data(), asset.sdf.resolution[0], asset.sdf.resolution[1], asset.sdf.resolution[2], format, vector_field_texture_, "VectorFieldTexture")) {
                     vector_field_resolution_[0] = asset.sdf.resolution[0];
                     vector_field_resolution_[1] = asset.sdf.resolution[1];
                     vector_field_resolution_[2] = asset.sdf.resolution[2];
                } else {
                    return false;
                }
            }
            else
            {
                std::cerr << "Unsupported vector field channel count: " << channels << std::endl;
            }
        }

        return true;
    }

    bool RHIGpuMesh::CreateAndUploadBuffer(const void* data, u64 size, ResourceHandle& out_buffer, BufferUsageFlags usage, const char* name)
    {
        if (!data || size == 0) return false;

        BufferDesc desc{};
        desc.size = size;
        desc.bindFlags = (u32)(usage | BufferUsageFlags::TransferDst); // Allow transfer to this buffer
        desc.memoryUsage = GPUMemoryUsage::Static; // GPU Local
        desc.usage = GPUMemoryUsage::Static;
        desc.name = name;

        out_buffer = device_->CreateBuffer(desc);
        if (out_buffer == handles::INVALID_RESOURCE)
        {
            std::cerr << "Failed to create GPU buffer: " << name << std::endl;
            return false;
        }

        // Staging Buffer
        BufferDesc stagingDesc{};
        stagingDesc.size = size;
        stagingDesc.bindFlags = (u32)BufferUsageFlags::TransferSrc;
        stagingDesc.memoryUsage = GPUMemoryUsage::Staging; // CPU Write, GPU Read
        stagingDesc.usage = GPUMemoryUsage::Staging;
        stagingDesc.name = "StagingBuffer";

        ResourceHandle stagingBuffer = device_->CreateBuffer(stagingDesc);
        if (stagingBuffer == handles::INVALID_RESOURCE)
        {
            std::cerr << "Failed to create Staging buffer for: " << name << std::endl;
            return false;
        }

        // Map and Copy
        void* mappedData = device_->MapBuffer(stagingBuffer, 0, size);
        if (mappedData)
        {
            memcpy(mappedData, data, size);
            device_->UnmapBuffer(stagingBuffer);
        }
        else
        {
            std::cerr << "Failed to map Staging buffer for: " << name << std::endl;
            device_->DestroyBuffer(stagingBuffer);
            return false;
        }

        // Command Buffer
        CommandBufferHandle cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics); // Use Graphics queue for simplicity, should use Transfer if available and distinct
        if (cmdHandle == handles::INVALID_COMMAND_BUFFER)
        {
            std::cerr << "Failed to create Command Buffer for upload: " << name << std::endl;
            device_->DestroyBuffer(stagingBuffer);
            return false;
        }

        RHICommandBuffer* cmd = GetCommandBuffer(cmdHandle);
        if (!cmd)
        {
             device_->DestroyCommandBuffer(cmdHandle);
             device_->DestroyBuffer(stagingBuffer);
             return false;
        }

        cmd->Begin();
        cmd->CopyBuffer(stagingBuffer, out_buffer, 0, 0, size);
        
        // Optional: Barrier to ensure transfer is done before usage? 
        // Submit will wait for completion anyway in this blocking implementation.
        
        cmd->End();
        cmd->Submit();
        cmd->WaitForCompletion();

        device_->DestroyCommandBuffer(cmdHandle);
        device_->DestroyBuffer(stagingBuffer);

        return true;
    }

    bool RHIGpuMesh::CreateAndUploadTexture3D(const void* data, u32 width, u32 height, u32 depth, DataFormat format, ResourceHandle& out_texture, const char* name)
    {
        if (!data || width == 0 || height == 0 || depth == 0) return false;

        TextureDesc desc{};
        desc.size = {width, height, depth};
        desc.type = TextureType::Texture3D;
        desc.format = format;
        desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
        desc.memoryUsage = GPUMemoryUsage::Static;
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.name = name;

        out_texture = device_->CreateTexture(desc);
        if (out_texture == handles::INVALID_RESOURCE)
        {
            std::cerr << "Failed to create GPU Texture3D: " << name << std::endl;
            return false;
        }

        // Calculate size
        u32 bytesPerPixel = 0;
        switch (format)
        {
        case DataFormat::R8_UNorm:
        case DataFormat::R8_UInt:
            bytesPerPixel = 1; break;
        case DataFormat::R16_Float:
        case DataFormat::R16_UInt:
            bytesPerPixel = 2; break;
        case DataFormat::RG16_Float:
        case DataFormat::RGBA8_UNorm:
            bytesPerPixel = 4; break;
        case DataFormat::RGBA16_Float:
            bytesPerPixel = 8; break;
        default:
            std::cerr << "Unsupported texture format for upload: " << (int)format << std::endl;
            return false;
        }

        u32 srcRowPitch = width * bytesPerPixel;
        u32 alignedBytesPerRow = srcRowPitch;
        
        // Align bytesPerRow to 256 bytes for Metal compatibility
        if (alignedBytesPerRow % 256 != 0) {
            alignedBytesPerRow = (alignedBytesPerRow + 255) & ~255;
        }
        
        u64 slicePitch = alignedBytesPerRow * height;
        u64 totalSize = slicePitch * depth;

        // Staging Buffer
        BufferDesc stagingDesc{};
        stagingDesc.size = totalSize;
        stagingDesc.bindFlags = (u32)BufferUsageFlags::TransferSrc;
        stagingDesc.memoryUsage = GPUMemoryUsage::Staging;
        stagingDesc.usage = GPUMemoryUsage::Staging;
        stagingDesc.name = "TextureStagingBuffer";

        ResourceHandle stagingBuffer = device_->CreateBuffer(stagingDesc);
        if (stagingBuffer == handles::INVALID_RESOURCE)
        {
             std::cerr << "Failed to create Staging buffer for texture: " << name << std::endl;
             return false;
        }

        void* mappedData = device_->MapBuffer(stagingBuffer, 0, totalSize);
        if (mappedData)
        {
            if (alignedBytesPerRow == srcRowPitch) {
                // Tightly packed, single copy
                memcpy(mappedData, data, width * height * depth * bytesPerPixel);
            } else {
                // Need padding
                u8* dst = (u8*)mappedData;
                const u8* src = (const u8*)data;
                for (u32 z = 0; z < depth; ++z) {
                    for (u32 y = 0; y < height; ++y) {
                        memcpy(dst, src, srcRowPitch);
                        dst += alignedBytesPerRow;
                        src += srcRowPitch;
                    }
                }
            }
            device_->UnmapBuffer(stagingBuffer);
        }
        else
        {
            std::cerr << "Failed to map Staging buffer for texture: " << name << std::endl;
            device_->DestroyBuffer(stagingBuffer);
            return false;
        }

        CommandBufferHandle cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
        if (cmdHandle == handles::INVALID_COMMAND_BUFFER)
        {
            device_->DestroyBuffer(stagingBuffer);
            return false;
        }
        RHICommandBuffer* cmd = GetCommandBuffer(cmdHandle);
        
        cmd->Begin();

        BufferTextureCopyRegion region{};
        region.bufferOffset = 0;
        region.bufferRowLength = alignedBytesPerRow / bytesPerPixel; // Specify padded row length
        region.bufferImageHeight = height; 
        
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent = { width, height, depth };

        cmd->CopyBufferToTexture(stagingBuffer, out_texture, &region, 1);
        cmd->End();
        cmd->Submit();
        cmd->WaitForCompletion();

        device_->DestroyCommandBuffer(cmdHandle);
        device_->DestroyBuffer(stagingBuffer);

        return true;
    }
}
