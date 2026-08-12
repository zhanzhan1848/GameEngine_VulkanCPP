#pragma once
#include "RHITypes.h"
#include "RHIDevice.h"
#include "RHIMeshAsset.h"

namespace primal::graphics::rhi
{
    class RHIGpuMesh
    {
    public:
        RHIGpuMesh() = default;
        ~RHIGpuMesh();

        // 禁用拷贝，允许移动
        RHIGpuMesh(const RHIGpuMesh&) = delete;
        RHIGpuMesh& operator=(const RHIGpuMesh&) = delete;
        RHIGpuMesh(RHIGpuMesh&& other) noexcept;
        RHIGpuMesh& operator=(RHIGpuMesh&& other) noexcept;

        // 初始化并上传资源到 GPU
        // 注意：此操作可能是同步阻塞的，因为它涉及创建 Staging Buffer 和执行 Copy 命令
        bool Initialize(RHIDeviceBase& device, const RHIMeshAsset& asset);
        
        // 销毁所有 GPU 资源
        void Destroy();

        [[nodiscard]] bool IsValid() const { return device_ != nullptr; }

        // Getters
        ResourceHandle GetPositionBuffer() const { return position_buffer_; }
        ResourceHandle GetElementBuffer() const { return element_buffer_; }
        ResourceHandle GetIndexBuffer() const { return index_buffer_; }
        ResourceHandle GetMeshletBuffer() const { return meshlet_buffer_; }
        ResourceHandle GetMeshletVerticesBuffer() const { return meshlet_vertices_buffer_; }
        ResourceHandle GetMeshletTrianglesBuffer() const { return meshlet_triangles_buffer_; }
        ResourceHandle GetSDFTexture() const { return sdf_texture_; }
        ResourceHandle GetVoxelTexture() const { return voxel_texture_; }
        ResourceHandle GetVectorFieldTexture() const { return vector_field_texture_; }

        const u32* GetVoxelResolution() const { return voxel_resolution_; }
        const u32* GetSDFResolution() const { return sdf_resolution_; } // Voxels share resolution with SDF
        const u32* GetVectorFieldResolution() const { return vector_field_resolution_; }

        const f32* GetBoundsMin() const { return bounds_min_; }
        const f32* GetBoundsMax() const { return bounds_max_; }

        u32 GetIndexCount() const { return index_count_; }
        u32 GetVertexCount() const { return vertex_count_; }
        u32 GetMeshletCount() const { return meshlet_count_; }
        u32 GetIndexSize() const { return index_size_; } // Returns 2 or 4

    private:
        RHIDeviceBase* device_{ nullptr };
        
        ResourceHandle position_buffer_{ handles::INVALID_RESOURCE };
        ResourceHandle element_buffer_{ handles::INVALID_RESOURCE };
        ResourceHandle index_buffer_{ handles::INVALID_RESOURCE };
        ResourceHandle meshlet_buffer_{ handles::INVALID_RESOURCE };
        ResourceHandle meshlet_vertices_buffer_{ handles::INVALID_RESOURCE };
        ResourceHandle meshlet_triangles_buffer_{ handles::INVALID_RESOURCE };
        ResourceHandle sdf_texture_{ handles::INVALID_RESOURCE };
        ResourceHandle voxel_texture_{ handles::INVALID_RESOURCE };
        ResourceHandle vector_field_texture_{ handles::INVALID_RESOURCE };

        u32 index_count_{ 0 };
        u32 vertex_count_{ 0 };
        u32 meshlet_count_{ 0 };
        u32 index_size_{ 0 }; // 2 for u16, 4 for u32
        
        u32 sdf_resolution_[3] = {0, 0, 0};
        u32 voxel_resolution_[3] = {0, 0, 0};
        u32 vector_field_resolution_[3] = {0, 0, 0};
        
        f32 bounds_min_[3] = {0.f, 0.f, 0.f};
        f32 bounds_max_[3] = {0.f, 0.f, 0.f};

        // Helper for upload
        bool CreateAndUploadBuffer(const void* data, u64 size, ResourceHandle& out_buffer, BufferUsageFlags usage, const char* name);
        bool CreateAndUploadTexture3D(const void* data, u32 width, u32 height, u32 depth, DataFormat format, ResourceHandle& out_texture, const char* name);
    };
}
