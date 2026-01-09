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
class RHISwapChain;
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


// === 管线描述符结构体（前向声明） ===

/**
 * @brief 模板操作描述符
 */
struct StencilOpDesc {
    StencilOp failOp;       ///< 模板测试失败操作
    StencilOp depthFailOp;  ///< 深度测试失败操作
    StencilOp passOp;       ///< 模板/深度测试通过操作
    ComparisonFunc func;    ///< 比较函数
    
    StencilOpDesc() : failOp(StencilOp::Keep), depthFailOp(StencilOp::Keep), 
                      passOp(StencilOp::Keep), func(ComparisonFunc::Always) {}
};

/**
 * @brief 图形管线描述符
 */
struct GraphicsPipelineDesc {
    ShaderHandle vertexShader;          ///< 顶点着色器
    ShaderHandle pixelShader;           ///< 像素着色器
    ShaderHandle geometryShader;        ///< 几何着色器
    ShaderHandle hullShader;            ///< 外壳着色器
    ShaderHandle domainShader;          ///< 域着色器
    
    PipelineLayoutHandle layout;        ///< 管线布局
    
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
    
    StencilOpDesc frontStencil;         ///< 正面模板操作
    StencilOpDesc backStencil;          ///< 背面模板操作
    
    bool enableBlend;                   ///< 是否启用混合
    BlendFactor srcColorBlendFactor;    ///< 源颜色混合因子
    BlendFactor dstColorBlendFactor;    ///< 目标颜色混合因子
    BlendOp colorBlendOp;               ///< 颜色混合操作
    BlendFactor srcAlphaBlendFactor;    ///< 源Alpha混合因子
    BlendFactor dstAlphaBlendFactor;    ///< 目标Alpha混合因子
    BlendOp alphaBlendOp;               ///< Alpha混合操作
    
    math::v4 blendConstants;            ///< 混合常量
    
    GraphicsPipelineDesc() : vertexShader(handles::INVALID_SHADER), 
                            pixelShader(handles::INVALID_SHADER),
                            geometryShader(handles::INVALID_SHADER),
                            hullShader(handles::INVALID_SHADER),
                            domainShader(handles::INVALID_SHADER),
                            layout(handles::INVALID_PIPELINE_LAYOUT),
                            topology(PrimitiveTopology::TriangleList),
                            fillMode(FillMode::Solid), cullMode(CullMode::Back),
                            renderTargetCount(0), depthStencilFormat(DataFormat::Unknown),
                            enableDepthTest(true), enableDepthWrite(true),
                            depthFunc(ComparisonFunc::Less), enableStencilTest(false),
                            stencilReadMask(0xFF), stencilWriteMask(0xFF),
                            enableBlend(false), 
                            srcColorBlendFactor(BlendFactor::One), dstColorBlendFactor(BlendFactor::Zero), colorBlendOp(BlendOp::Add),
                            srcAlphaBlendFactor(BlendFactor::One), dstAlphaBlendFactor(BlendFactor::Zero), alphaBlendOp(BlendOp::Add),
                            blendConstants{1.0f, 1.0f, 1.0f, 1.0f} {
        for (uint32_t i = 0; i < constants::MAX_RENDER_TARGETS; ++i) {
            renderTargetFormats[i] = DataFormat::Unknown;
        }
    }
};

/**
 * @brief 计算管线描述符
 */
// struct ComputePipelineDesc is defined in RHITypes.h

/**
 * @brief 查询池描述符
 */
struct QueryPoolDesc {
    QueryType type;                     ///< 查询类型
    uint32_t queryCount;                ///< 查询数量
    
    QueryPoolDesc() : type(QueryType::Timestamp), queryCount(0) {}
};


/**
 * @brief RHI设备接口基类
 * @details 提供非模板的设备接口，用于需要类型擦除的场景
 */
class RHIDeviceBase {
public:
    virtual ~RHIDeviceBase() = default;
    virtual bool IsValid() const = 0;
    virtual const DeviceInfo& GetDeviceInfo() const = 0;
    virtual const DeviceDesc& GetDesc() const = 0;
    virtual void WaitIdle() const = 0;
    virtual void Shutdown() = 0;
    virtual bool SubmitCommandBuffer(CommandBufferHandle handle) = 0;
    virtual SyncHandle CreateSync() = 0;
    virtual bool WaitForSync(SyncHandle handle, u32 timeoutMs) = 0;
    virtual void DestroySync(SyncHandle handle) = 0;
    virtual QueryPoolHandle CreateQueryPool(const QueryPoolDesc& desc) = 0;
    virtual void DestroyQueryPool(QueryPoolHandle handle) = 0;
    virtual SamplerHandle CreateSampler(const SamplerDesc& desc) = 0;
    virtual void DestroySampler(SamplerHandle handle) = 0;
    virtual DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) = 0;
    virtual void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle handle) = 0;
    virtual PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) = 0;
    virtual void DestroyPipelineLayout(PipelineLayoutHandle handle) = 0;
    virtual DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& desc) = 0;
    virtual void DestroyDescriptorSet(DescriptorSetHandle handle) = 0;
    virtual void UpdateDescriptorSets(uint32_t writeCount, const WriteDescriptorSet* writes) = 0;
    virtual ResourceHandle CreateBuffer(const BufferDesc& desc) = 0;
    virtual ResourceHandle CreateTexture(const TextureDesc& desc) = 0;
    virtual void DestroyBuffer(ResourceHandle handle) = 0;
    virtual void DestroyTexture(ResourceHandle handle) = 0;
};

/**
 * @brief RHI设备基类（CRTP模式）
 * @tparam Derived 派生类类型
 * @details 使用CRTP实现编译时多态，避免虚函数调用开销
 */
template<typename Derived>
class RHIDevice : public RHIDeviceBase {
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
    void Shutdown() override {
        if (!isValid_) return;
        
        derived().shutdownImpl();
        isValid_ = false;
    }
    
    /**
     * @brief 等待设备空闲
     */
    void WaitIdle() const override {
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
     * @brief 创建交换链
     * @param desc 交换链描述符
     * @return 交换链指针，失败返回nullptr
     */
    RHISwapChain* CreateSwapChain(const SwapChainDesc& desc) {
        assert(isValid_ && "Device not initialized");
        return derived().createSwapChainImpl(desc);
    }

    /**
     * @brief 销毁交换链
     * @param swapChain 交换链指针
     */
    void DestroySwapChain(RHISwapChain* swapChain) {
        assert(isValid_ && "Device not initialized");
        if (swapChain) {
            derived().destroySwapChainImpl(swapChain);
        }
    }

    /**
     * @brief 创建缓冲区
     * @param desc 缓冲区描述符
     * @return 资源句柄，失败返回INVALID_RESOURCE
     */
    ResourceHandle CreateBuffer(const BufferDesc& desc) override {
        assert(isValid_ && "Device not initialized");
        return derived().createBufferImpl(desc);
    }
    
    /**
     * @brief 创建纹理
     * @param desc 纹理描述符
     * @return 资源句柄，失败返回INVALID_RESOURCE
     */
    ResourceHandle CreateTexture(const TextureDesc& desc) override {
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
     * @brief 创建查询池
     * @param desc 查询池描述符
     * @return 查询池句柄
     */
    QueryPoolHandle CreateQueryPool(const QueryPoolDesc& desc) override {
        assert(isValid_ && "Device not initialized");
        return derived().createQueryPoolImpl(desc);
    }

    /**
     * @brief 创建采样器
     * @param desc 采样器描述符
     * @return 采样器句柄
     */
    SamplerHandle CreateSampler(const SamplerDesc& desc) override {
        assert(isValid_ && "Device not initialized");
        return derived().createSamplerImpl(desc);
    }

    /**
     * @brief 创建描述符集布局
     * @param desc 描述符集布局描述符
     * @return 描述符集布局句柄
     */
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override {
        assert(isValid_ && "Device not initialized");
        return derived().createDescriptorSetLayoutImpl(desc);
    }

    /**
     * @brief 销毁描述符集布局
     * @param handle 描述符集布局句柄
     */
    void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle handle) override {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_RESOURCE) {
            derived().destroyDescriptorSetLayoutImpl(handle);
        }
    }

    /**
     * @brief 创建管线布局
     * @param desc 管线布局描述符
     * @return 管线布局句柄
     */
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) override {
        assert(isValid_ && "Device not initialized");
        return derived().createPipelineLayoutImpl(desc);
    }

    /**
     * @brief 销毁管线布局
     * @param handle 管线布局句柄
     */
    void DestroyPipelineLayout(PipelineLayoutHandle handle) override {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_PIPELINE_LAYOUT) {
            derived().destroyPipelineLayoutImpl(handle);
        }
    }

    /**
     * @brief 创建描述符集
     * @param desc 描述符集描述符
     * @return 描述符集句柄
     */
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& desc) override {
        assert(isValid_ && "Device not initialized");
        return derived().createDescriptorSetImpl(desc);
    }

    /**
     * @brief 销毁描述符集
     * @param handle 描述符集句柄
     */
    void DestroyDescriptorSet(DescriptorSetHandle handle) override {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_RESOURCE) {
            derived().destroyDescriptorSetImpl(handle);
        }
    }

    /**
     * @brief 更新描述符集
     * @param writeCount 更新数量
     * @param writes 更新操作数组
     */
    void UpdateDescriptorSets(uint32_t writeCount, const WriteDescriptorSet* writes) override {
        assert(isValid_ && "Device not initialized");
        derived().updateDescriptorSetsImpl(writeCount, writes);
    }

    /**
     * @brief 创建渲染通道
     * @param desc 渲染通道描述符
     * @return 渲染通道句柄
     */
    RenderPassHandle CreateRenderPass(const RenderPassDesc& desc) {
        assert(isValid_ && "Device not initialized");
        return derived().createRenderPassImpl(desc);
    }

    /**
     * @brief 销毁渲染通道
     * @param handle 渲染通道句柄
     */
    void DestroyRenderPass(RenderPassHandle handle) {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_RESOURCE) {
            derived().destroyRenderPassImpl(handle);
        }
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
    
    /**
     * @brief 提交命令缓冲区
     * @param handle 命令缓冲区句柄
     * @return 提交是否成功
     */
    bool SubmitCommandBuffer(CommandBufferHandle handle) override {
        assert(isValid_ && "Device not initialized");
        return derived().submitCommandBufferImpl(handle);
    }
    
    /**
     * @brief 创建同步对象
     * @return 同步对象句柄
     */
    SyncHandle CreateSync() override {
        assert(isValid_ && "Device not initialized");
        return derived().createSyncImpl();
    }
    
    /**
     * @brief 等待同步对象
     * @param handle 同步对象句柄
     * @param timeoutMs 超时时间（毫秒）
     * @return 是否成功
     */
    bool WaitForSync(SyncHandle handle, u32 timeoutMs) override {
        assert(isValid_ && "Device not initialized");
        return derived().waitForSyncImpl(handle, timeoutMs);
    }

    // === 资源销毁接口 ===
    
    /**
     * @brief 销毁缓冲区
     * @param handle 缓冲区句柄
     */
    void DestroyBuffer(ResourceHandle handle) override {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_RESOURCE) {
            derived().destroyBufferImpl(handle);
        }
    }
    
    /**
     * @brief 销毁纹理
     * @param handle 纹理句柄
     */
    void DestroyTexture(ResourceHandle handle) override {
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

    /**
     * @brief 销毁同步对象
     * @param handle 同步对象句柄
     */
    void DestroySync(SyncHandle handle) override {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_SYNC) {
            derived().destroySyncImpl(handle);
        }
    }

    /**
     * @brief 销毁查询池
     * @param handle 查询池句柄
     */
    void DestroyQueryPool(QueryPoolHandle handle) override {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_QUERY_POOL) {
            derived().destroyQueryPoolImpl(handle);
        }
    }

    /**
     * @brief 销毁采样器
     * @param handle 采样器句柄
     */
    void DestroySampler(SamplerHandle handle) override {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_SAMPLER) {
            derived().destroySamplerImpl(handle);
        }
    }
    
    // === 访问器方法 ===
    
    /**
     * @brief 获取设备描述符
     * @return 设备描述符的常量引用
     */
    const DeviceDesc& GetDesc() const override { return desc_; }
    
    /**
     * @brief 获取设备信息
     * @return 设备信息的常量引用
     */
    const DeviceInfo& GetInfo() const { return info_; }
    
    /**
     * @brief 检查设备是否有效
     * @return 设备是否已初始化且有效
     */
    bool IsValid() const override { return isValid_; }
    
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
    
    // === 基础接口实现 ===
    
    const DeviceInfo& GetDeviceInfo() const override { return info_; }
    
    // === 命令提交接口 ===
    
    // 移至 public 区域

    
    // === 成员变量 ===
    
    DeviceDesc desc_;                    ///< 设备描述符
    DeviceInfo info_;                    ///< 设备信息
    bool isValid_;                       ///< 设备是否有效
    
private:
    // === 友元声明 ===
    friend Derived;
};

/**
 * @brief RHI设备管理器
 * @details 管理所有RHI设备的注册和注销
 */
class RHIDeviceManager {
public:
    /**
     * @brief 注册设备
     * @param device 设备指针
     * @return 设备ID
     */
    uint32_t RegisterDevice(RHIDeviceBase* device);
    
    /**
     * @brief 注销设备
     * @param deviceId 设备ID
     */
    void UnregisterDevice(uint32_t deviceId);
    
    /**
     * @brief 获取设备数量
     * @return 设备数量
     */
    size_t GetDeviceCount() const;
    
    /**
     * @brief 获取设备
     * @param deviceId 设备ID
     * @return 设备指针
     */
    RHIDeviceBase* GetDevice(uint32_t deviceId) const;
    
private:
    utl::vector<std::pair<uint32_t, RHIDeviceBase*>> devices_;
    uint32_t nextDeviceId_ = 1;
};

// 全局设备管理器实例
extern RHIDeviceManager g_deviceManager;

} // namespace primal::graphics::rhi