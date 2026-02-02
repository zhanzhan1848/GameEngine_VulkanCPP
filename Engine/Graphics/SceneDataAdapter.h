#pragma once

#include "CommonHeaders.h"
#include "RHI/Core/RHIDevice.h"
#include "RenderMesh.h"
#include <vector>
#include <string>

namespace primal::graphics {

struct SceneDataMeshInfo {
    std::string name;
    uint32_t lodId;
    float lodThreshold;
    RenderMesh* mesh;
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

private:
    // 内部辅助类和函数将在cpp中实现
};

} // namespace primal::graphics
