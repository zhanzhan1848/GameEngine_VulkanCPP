/**
 * @file IBLPrecomputer.h
 * @brief IBL 预计算工具类
 * @details 负责生成 Irradiance Map, Prefiltered Environment Map 和 BRDF Integration LUT
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-28
 * @version 0.1.0
 */

#pragma once

#include "../Core/RHIDevice.h"
#include "../Core/RHITypes.h"
#include <string>
#include <vector>

namespace primal::graphics::rhi {

/**
 * @brief IBL 预计算工具类
 */
class IBLPrecomputer {
public:
    /**
     * @brief 构造函数
     * @param device RHI设备指针
     */
    explicit IBLPrecomputer(RHIDeviceBase* device);

    /**
     * @brief 析构函数
     */
    ~IBLPrecomputer();

    /**
     * @brief 初始化预计算所需的管线和资源
     * @return 是否成功
     */
    bool Initialize();

    /**
     * @brief 清理资源
     */
    void Shutdown();

    /**
     * @brief 生成漫反射辐照度贴图 (Irradiance Map)
     * @param envMap 输入的环境贴图 (Cubemap)，必须是 ShaderResource 状态
     * @param outputSize 输出贴图尺寸 (默认 32x32)
     * @return 生成的 Irradiance Map 句柄
     */
    ResourceHandle ComputeIrradianceMap(ResourceHandle envMap, uint32_t outputSize = 32);

    /**
     * @brief 生成镜面反射预滤波贴图 (Prefiltered Environment Map)
     * @param envMap 输入的环境贴图 (Cubemap)，必须是 ShaderResource 状态
     * @param outputSize 输出贴图尺寸 (默认 512x512)
     * @return 生成的 Prefiltered Map 句柄
     */
    ResourceHandle ComputePrefilteredEnvironmentMap(ResourceHandle envMap, uint32_t outputSize = 512);

    /**
     * @brief 生成 BRDF 积分 LUT
     * @param outputSize 输出贴图尺寸 (默认 512x512)
     * @return 生成的 BRDF LUT 句柄
     */
    ResourceHandle ComputeBRDFIntegrationMap(uint32_t outputSize = 512);

private:
    RHIDeviceBase* device_;
    
    // 管线对象
    PipelineHandle irradiancePipeline_{handles::INVALID_PIPELINE};
    PipelineHandle prefilterPipeline_{handles::INVALID_PIPELINE};
    PipelineHandle brdfPipeline_{handles::INVALID_PIPELINE};
    
    // 管线布局
    PipelineLayoutHandle irradiancePipelineLayout_{handles::INVALID_PIPELINE_LAYOUT};
    PipelineLayoutHandle prefilterPipelineLayout_{handles::INVALID_PIPELINE_LAYOUT};
    PipelineLayoutHandle brdfPipelineLayout_{handles::INVALID_PIPELINE_LAYOUT};

    // 描述符集布局
    DescriptorSetLayoutHandle irradianceDescLayout_{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    DescriptorSetLayoutHandle prefilterDescLayout_{handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    DescriptorSetLayoutHandle brdfDescLayout_{handles::INVALID_DESCRIPTOR_SET_LAYOUT}; // BRDF 可能不需要输入纹理，或者需要采样器

    // 内部辅助函数
    bool CreatePipelines();
    void DestroyPipelines();
    
    /**
     * @brief 加载 Shader 字节码
     * @param filename Shader 文件名 (相对于 Shader 目录)
     * @param entryPoint 入口点函数名
     * @return Shader 句柄
     */
    ShaderHandle CreateComputeShader(const std::string& filename, const char* entryPoint);

    /**
     * @brief 处理 Shader include
     * @param filename 文件名
     * @return 处理后的源码
     */
    std::string LoadShaderSource(const std::string& filename);
};

} // namespace primal::graphics::rhi
