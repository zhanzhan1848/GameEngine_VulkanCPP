/**
 * @file RHIDevice.h
 * @brief RHI设备基类定义（CRTP模式）
 * @details 使用奇异递归模板模式(CRTP)实现零开销的设备抽象
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-29
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include "RHITypes.h"

namespace primal::graphics::rhi {

// === 前向声明 ===

class RHIResource;
class RHICommandBuffer;
class RHIShader;
class RHIPipeline;

/**
 * @brief RHI设备描述符
 * @details 包含设备创建所需的参数
 */
struct DeviceDesc {
    RHIPlatform platform;              ///< 目标平台
    bool enableDebug;                   ///< 是否启用调试层
    bool enableValidation;              ///< 是否启用验证层
    uint32_t adapterIndex;              ///< 适配器索引
    uint32_t maxFramesInFlight;         ///< 最大帧数
    
    DeviceDesc() : platform(RHIPlatform::Unknown), enableDebug(false), 
                  enableValidation(false), adapterIndex(0), maxFramesInFlight(3) {}
};

/**
 * @brief RHI设备信息
 * @details 包含设备的硬件信息和能力
 */
struct DeviceInfo {
    RHIPlatform platform;              ///< 设备平台
    char deviceName[256];               ///< 设备名称
    char driverVersion[128];            ///< 驱动版本
    uint64_t dedicatedVideoMemory;      ///< 专用显存大小（字节）
    uint64_t sharedSystemMemory;        ///< 共享系统内存大小（字节）
    uint32_t maxTexture1DSize;          ///< 1D纹理最大尺寸
    uint32_t maxTexture2DSize;          ///< 2D纹理最大尺寸
    uint32_t maxTexture3DSize;          ///< 3D纹理最大尺寸
    uint32_t maxTextureCubeSize;        ///< 立方纹理最大尺寸
    uint32_t maxRenderTargets;          ///< 最大渲染目标数
    uint32_t maxVertexAttributes;       ///< 最大顶点属性数
    uint32_t maxSamplerStates;          ///< 最大采样器状态数
    uint32_t maxConstantBufferSize;     ///< 最大常量缓冲区大小
    bool supportsRayTracing;            ///< 是否支持光线追踪
    bool supportsMeshShaders;           ///< 是否支持网格着色器
    bool supportsVariableRateShading;   ///< 是否支持可变速率着色
    
    DeviceInfo() : platform(RHIPlatform::Unknown), deviceName{0}, driverVersion{0},
                  dedicatedVideoMemory(0), sharedSystemMemory(0),
                  maxTexture1DSize(0), maxTexture2DSize(0), maxTexture3DSize(0),
                  maxTextureCubeSize(0), maxRenderTargets(0), maxVertexAttributes(0),
                  maxSamplerStates(0), maxConstantBufferSize(0),
                  supportsRayTracing(false), supportsMeshShaders(false),
                  supportsVariableRateShading(false) {}
};

/**
 * @brief RHI设备基类（CRTP模式）
 * @tparam Derived 派生类类型
 * @details 使用CRTP实现编译时多态，避免虚函数调用开销
 */
template<typename Derived>
class RHIDevice {
public:
    using DerivedType = Derived;
    
    // === 构造函数和析构函数 ===
    
    /**
     * @brief 构造函数
     * @param desc 设备描述符
     */
    explicit RHIDevice(const DeviceDesc& desc) : desc_(desc), isValid_(false) {}
    
    /**
     * @brief 虚析构函数
     */
    virtual ~RHIDevice() = default;
    
    // === 禁用拷贝和移动 ===
    
    RHIDevice(const RHIDevice&) = delete;
    RHIDevice& operator=(const RHIDevice&) = delete;
    RHIDevice(RHIDevice&&) = delete;
    RHIDevice& operator=(RHIDevice&&) = delete;
    
    // === 核心接口方法 ===
    
    /**
     * @brief 初始化设备
     * @return 初始化是否成功
     */
    bool Initialize() {
        if (isValid_) return true;
        
        // 调用派生类的初始化实现
        bool result = derived().initializeImpl();
        if (result) {
            isValid_ = true;
            derived().queryDeviceInfo(info_);
        }
        return result;
    }
    
    /**
     * @brief 销毁设备
     */
    void Shutdown() {
        if (!isValid_) return;
        
        derived().shutdownImpl();
        isValid_ = false;
    }
    
    /**
     * @brief 等待设备空闲
     */
    void WaitIdle() const {
        assert(isValid_ && "Device not initialized");
        derived().waitIdleImpl();
    }
    
    /**
     * @brief 开始新的一帧
     */
    void BeginFrame() {
        assert(isValid_ && "Device not initialized");
        derived().beginFrameImpl();
    }
    
    /**
     * @brief 结束当前帧
     */
    void EndFrame() {
        assert(isValid_ && "Device not initialized");
        derived().endFrameImpl();
    }
    
    /**
     * @brief 呈现到屏幕
     */
    void Present() {
        assert(isValid_ && "Device not initialized");
        derived().presentImpl();
    }
    
    // === 资源创建接口 ===
    
    /**
     * @brief 创建缓冲区
     * @param desc 缓冲区描述符
     * @return 资源句柄，失败返回INVALID_RESOURCE
     */
    ResourceHandle CreateBuffer(const BufferDesc& desc) {
        assert(isValid_ && "Device not initialized");
        return derived().createBufferImpl(desc);
    }
    
    /**
     * @brief 创建纹理
     * @param desc 纹理描述符
     * @return 资源句柄，失败返回INVALID_RESOURCE
     */
    ResourceHandle CreateTexture(const TextureDesc& desc) {
        assert(isValid_ && "Device not initialized");
        return derived().createTextureImpl(desc);
    }
    
    /**
     * @brief 创建着色器
     * @param data 着色器数据
     * @param size 数据大小
     * @param stage 着色器阶段
     * @param entryPoint 入口点函数名
     * @return 着色器句柄，失败返回INVALID_SHADER
     */
    ShaderHandle CreateShader(const void* data, size_t size, ShaderStage stage, const char* entryPoint = "main") {
        assert(isValid_ && "Device not initialized");
        assert(data && size > 0 && "Invalid shader data");
        return derived().createShaderImpl(data, size, stage, entryPoint);
    }
    
    /**
     * @brief 创建图形管线
     * @param desc 管线描述符
     * @return 管线句柄，失败返回INVALID_PIPELINE
     */
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
        assert(isValid_ && "Device not initialized");
        return derived().createGraphicsPipelineImpl(desc);
    }
    
    /**
     * @brief 创建计算管线
     * @param desc 管线描述符
     * @return 管线句柄，失败返回INVALID_PIPELINE
     */
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) {
        assert(isValid_ && "Device not initialized");
        return derived().createComputePipelineImpl(desc);
    }
    
    /**
     * @brief 创建命令缓冲区
     * @param type 命令队列类型
     * @return 命令缓冲区句柄，失败返回INVALID_COMMAND_BUFFER
     */
    CommandBufferHandle CreateCommandBuffer(CommandQueueType type = CommandQueueType::Graphics) {
        assert(isValid_ && "Device not initialized");
        return derived().createCommandBufferImpl(type);
    }
    
    // === 资源销毁接口 ===
    
    /**
     * @brief 销毁缓冲区
     * @param handle 缓冲区句柄
     */
    void DestroyBuffer(ResourceHandle handle) {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_RESOURCE) {
            derived().destroyBufferImpl(handle);
        }
    }
    
    /**
     * @brief 销毁纹理
     * @param handle 纹理句柄
     */
    void DestroyTexture(ResourceHandle handle) {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_RESOURCE) {
            derived().destroyTextureImpl(handle);
        }
    }
    
    /**
     * @brief 销毁着色器
     * @param handle 着色器句柄
     */
    void DestroyShader(ShaderHandle handle) {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_SHADER) {
            derived().destroyShaderImpl(handle);
        }
    }
    
    /**
     * @brief 销毁管线
     * @param handle 管线句柄
     */
    void DestroyPipeline(PipelineHandle handle) {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_PIPELINE) {
            derived().destroyPipelineImpl(handle);
        }
    }
    
    /**
     * @brief 销毁命令缓冲区
     * @param handle 命令缓冲区句柄
     */
    void DestroyCommandBuffer(CommandBufferHandle handle) {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_COMMAND_BUFFER) {
            derived().destroyCommandBufferImpl(handle);
        }
    }
    
    // === 访问器方法 ===
    
    /**
     * @brief 获取设备描述符
     * @return 设备描述符的常量引用
     */
    const DeviceDesc& GetDesc() const { return desc_; }
    
    /**
     * @brief 获取设备信息
     * @return 设备信息的常量引用
     */
    const DeviceInfo& GetInfo() const { return info_; }
    
    /**
     * @brief 检查设备是否有效
     * @return 设备是否已初始化且有效
     */
    bool IsValid() const { return isValid_; }
    
    /**
     * @brief 获取当前帧索引
     * @return 当前帧索引（0到maxFramesInFlight-1）
     */
    uint32_t GetCurrentFrameIndex() const {
        assert(isValid_ && "Device not initialized");
        return derived().getCurrentFrameIndexImpl();
    }
    
    /**
     * @brief 获取平台
     * @return 设备平台类型
     */
    RHIPlatform GetPlatform() const { return desc_.platform; }
    
protected:
    // === CRTP辅助方法 ===
    
    /**
     * @brief 获取派生类引用
     * @return 派生类的引用
     */
    Derived& derived() { return static_cast<Derived&>(*this); }
    
    /**
     * @brief 获取派生类常量引用
     * @return 派生类的常量引用
     */
    const Derived& derived() const { return static_cast<const Derived&>(*this); }
    
    // === 成员变量 ===
    
    DeviceDesc desc_;                    ///< 设备描述符
    DeviceInfo info_;                    ///< 设备信息
    bool isValid_;                       ///< 设备是否有效
    
private:
    // === 友元声明 ===
    friend Derived;
};

// === 管线描述符结构体（前向声明） ===

/**
 * @brief 图形管线描述符
 */
struct GraphicsPipelineDesc {
    ShaderHandle vertexShader;          ///< 顶点着色器
    ShaderHandle pixelShader;           ///< 像素着色器
    ShaderHandle geometryShader;        ///< 几何着色器
    ShaderHandle hullShader;            ///< 外壳着色器
    ShaderHandle domainShader;          ///< 域着色器
    
    utl::vector<VertexInputAttribute> vertexAttributes; ///< 顶点输入属性
    utl::vector<VertexInputBinding> vertexBindings;     ///< 顶点输入绑定
    
    PrimitiveTopology topology;         ///< 图元拓扑
    FillMode fillMode;                  ///< 填充模式
    CullMode cullMode;                  ///< 裁剪模式
    
    DataFormat renderTargetFormats[constants::MAX_RENDER_TARGETS]; ///< 渲染目标格式
    uint32_t renderTargetCount;         ///< 渲染目标数量
    DataFormat depthStencilFormat;       ///< 深度模板格式
    
    bool enableDepthTest;               ///< 是否启用深度测试
    bool enableDepthWrite;              ///< 是否启用深度写入
    ComparisonFunc depthFunc;           ///< 深度比较函数
    
    bool enableStencilTest;             ///< 是否启用模板测试
    uint8_t stencilReadMask;           ///< 模板读取掩码
    uint8_t stencilWriteMask;           ///< 模板写入掩码
    
    bool enableBlend;                   ///< 是否启用混合
    BlendFactor srcBlend;               ///< 源混合因子
    BlendFactor destBlend;              ///< 目标混合因子
    BlendOp blendOp;                    ///< 混合操作
    
    math::v4 blendConstants;            ///< 混合常量
    
    GraphicsPipelineDesc() : vertexShader(handles::INVALID_SHADER), 
                            pixelShader(handles::INVALID_SHADER),
                            geometryShader(handles::INVALID_SHADER),
                            hullShader(handles::INVALID_SHADER),
                            domainShader(handles::INVALID_SHADER),
                            topology(PrimitiveTopology::TriangleList),
                            fillMode(FillMode::Solid), cullMode(CullMode::Back),
                            renderTargetCount(0), depthStencilFormat(DataFormat::Unknown),
                            enableDepthTest(true), enableDepthWrite(true),
                            depthFunc(ComparisonFunc::Less), enableStencilTest(false),
                            stencilReadMask(0xFF), stencilWriteMask(0xFF),
                            enableBlend(false), srcBlend(BlendFactor::One),
                            destBlend(BlendFactor::Zero), blendOp(BlendOp::Add),
                            blendConstants{1.0f, 1.0f, 1.0f, 1.0f} {
        for (uint32_t i = 0; i < constants::MAX_RENDER_TARGETS; ++i) {
            renderTargetFormats[i] = DataFormat::Unknown;
        }
    }
};

/**
 * @brief 计算管线描述符
 */
struct ComputePipelineDesc {
    ShaderHandle computeShader;         ///< 计算着色器
    
    ComputePipelineDesc() : computeShader(handles::INVALID_SHADER) {}
};

} // namespace primal::graphics::rhi