#pragma once

#include "CommonHeaders.h"
#include "RHI/Core/RHIDevice.h"
#include "RenderMesh.h"
#include "Material.h"
#include "MaterialInstance.h"
#include <vector>
#include <string>

namespace primal::graphics {

struct SceneDataMeshInfo {
    std::string name;
    uint32_t lodId;
    float lodThreshold;
    RenderMesh* mesh;
    std::shared_ptr<Material> material{nullptr}; // 材质资源 (共享所有权)
    std::shared_ptr<MaterialInstance> materialInstance{nullptr}; // 关联的材质实例
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
    std::vector<SceneDataMeshInfo> Load(rhi::RHIDeviceBase* device, const void* data, uint32_t size);

    /**
     * @brief 加载 RenderItem 格式的场景数据 (ContentToEngine 格式)
     * @param device RHI设备
     * @param data 原始数据指针
     * @param size 数据大小
     * @return 创建的 Mesh 信息列表
     */
    std::vector<SceneDataMeshInfo> LoadRenderItemData(rhi::RHIDeviceBase* device, const void* data, uint32_t size);

    /**
     * @brief 预编译材质数据结构头
     */
    struct CompiledMaterialHeader {
        uint32_t magic;     // 'MATL'
        uint32_t version;   // 1
        uint32_t shaderCount;
        // Followed by Shader Data, then State Data
    };

    /**
     * @brief 加载预编译材质数据并创建 Material 资源
     * @param device RHI设备
     * @param data 预编译材质二进制数据
     * @param size 数据大小
     * @return 创建的 Material 对象指针 (共享所有权)
     */
    std::shared_ptr<Material> LoadMaterial(rhi::RHIDeviceBase* device, const void* data, uint32_t size);

private:
    // 内部辅助类和函数将在cpp中实现
};

} // namespace primal::graphics
