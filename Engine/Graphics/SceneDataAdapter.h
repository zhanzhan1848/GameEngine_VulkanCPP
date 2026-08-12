#pragma once

#include "CommonHeaders.h"
#include "RHI/Core/RHIDevice.h"
#include "RenderMesh.h"
#include "Material.h"
#include "MaterialInstance.h"
#include "Utilities/Vector.h"
#include <string>

namespace primal::graphics {

struct SceneDataMeshInfo {
    std::string name;
    u32 lodId;
    float lodThreshold;
    RenderMesh* mesh;
    s32 materialIndex{-1}; // 材质索引
    std::string diffuseTexturePath; // 漫反射贴图路径 (从文件加载)
    std::string normalTexturePath;  // 法线贴图路径 (从文件加载)
    std::string roughnessTexturePath; // 粗糙度贴图路径
    std::string metallicTexturePath;  // 金属度贴图路径
    std::string specularTexturePath;  // 高光贴图路径
    std::string ormTexturePath;       // ORM 贴图路径 (如果合并了)
    std::shared_ptr<Material> material{nullptr}; // 材质资源 (共享所有权)
    std::shared_ptr<MaterialInstance> materialInstance{nullptr}; // 关联的材质实例

    bool texturesLoaded{false};

    // Meshlet/Debug support - content system entity ID
    primal::id::id_type meshEntityId{primal::id::invalid_id};

    // 🔥 NEW: GPU Material Registry MaterialID (for GPU-driven rendering)
    primal::id::id_type gpuMaterialId{primal::id::invalid_id};
};

// Phase 4: Resource import result — content IDs only, no GPU objects
struct ImportedResources {
    struct MeshEntry {
        id::id_type mesh_content_id;   // geometry_hierarchies ID (from create_resource)
        s32 material_index;            // Index into material_paths
        std::string diffuse_path;
        std::string normal_path;
        std::string orm_path;
    };
    utl::vector<MeshEntry> meshes;
};

class SceneDataAdapter {
public:
    SceneDataAdapter() = default;
    ~SceneDataAdapter() = default;

    /**
     * @brief 加载场景数据并创建 RenderMesh 资源
     * @param device RHI设备
     * @param data 原始 scene_data 数据指针
     * @param size 数据大小
     * @return 创建的 Mesh 信息列表
     */
    utl::vector<SceneDataMeshInfo> Load(rhi::RHIDeviceBase* device, const void* data, u32 size);

    /**
     * @brief 加载 RenderItem 格式的场景数据 (ContentToEngine 格式)
     * @param device RHI设备
     * @param data 原始数据指针
     * @param size 数据大小
     * @return 创建的 Mesh 信息列表
     */
    utl::vector<SceneDataMeshInfo> LoadRenderItemData(rhi::RHIDeviceBase* device, const void* data, u32 size);

    /**
     * @brief Parse binary scene data and return content IDs (no GPU resource creation)
     * @details Parses the same binary format as LoadRenderItemData but only registers
     *          RHIMeshAssets with the content system. Returns content IDs for use with
     *          ForwardSceneRenderer::RegisterMeshResource() or
     *          StandardRenderPipeline::RegisterMeshEntity()
     * @param data Raw binary data pointer
     * @param size Data size
     * @return Imported resources with mesh content IDs and texture paths
     */
    static ImportedResources ImportResources(const void* data, u32 size);

    /**
     * @brief 预编译材质数据结构头
     */
    struct CompiledMaterialHeader {
        u32 magic;     // 'MATL'
        u32 version;   // 1
        u32 shaderCount;
        // Followed by Shader Data, then State Data
    };

    /**
     * @brief 加载预编译材质数据并创建 Material 资源
     * @param device RHI设备
     * @param data 预编译材质二进制数据
     * @param size 数据大小
     * @return 创建的 Material 对象指针 (共享所有权)
     */
    std::shared_ptr<Material> LoadMaterial(rhi::RHIDeviceBase* device, const void* data, u32 size);

private:
    // 内部辅助类和函数将在cpp中实现
};

} // namespace primal::graphics
