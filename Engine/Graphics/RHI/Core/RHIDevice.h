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
#include "RHIGarbageCollector.h"

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
    u32 adapterIndex;              ///< 适配器索引
    u32 maxFramesInFlight;         ///< 最大帧数
    
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
    u64 dedicatedVideoMemory;      ///< 专用显存大小（字节）
    u64 sharedSystemMemory;        ///< 共享系统内存大小（字节）
    u32 maxTexture1DSize;          ///< 1D纹理最大尺寸
    u32 maxTexture2DSize;          ///< 2D纹理最大尺寸
    u32 maxTexture3DSize;          ///< 3D纹理最大尺寸
    u32 maxTextureCubeSize;        ///< 立方纹理最大尺寸
    u32 maxRenderTargets;          ///< 最大渲染目标数
    u32 maxVertexAttributes;       ///< 最大顶点属性数
    u32 maxSamplerStates;          ///< 最大采样器状态数
    u32 maxConstantBufferSize;     ///< 最大常量缓冲区大小
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
    
    float depthBias;                    ///< 深度偏差常数因子
    float depthBiasClamp;               ///< 深度偏差截断
    float slopeScaledDepthBias;         ///< 深度偏差斜率因子
    
    DataFormat renderTargetFormats[constants::MAX_RENDER_TARGETS]; ///< 渲染目标格式
    u32 renderTargetCount;         ///< 渲染目标数量
    DataFormat depthStencilFormat;       ///< 深度模板格式
    
    bool enableDepthTest;               ///< 是否启用深度测试
    bool enableDepthWrite;              ///< 是否启用深度写入
    ComparisonFunc depthFunc;           ///< 深度比较函数
    
    bool enableStencilTest;             ///< 是否启用模板测试
    u8 stencilReadMask;           ///< 模板读取掩码
    u8 stencilWriteMask;           ///< 模板写入掩码
    
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
                            depthBias(0.0f), depthBiasClamp(0.0f), slopeScaledDepthBias(0.0f),
                            renderTargetCount(0), depthStencilFormat(DataFormat::Unknown),
                            enableDepthTest(true), enableDepthWrite(true),
                            depthFunc(ComparisonFunc::Less), enableStencilTest(false),
                            stencilReadMask(0xFF), stencilWriteMask(0xFF),
                            enableBlend(false), 
                            srcColorBlendFactor(BlendFactor::One), dstColorBlendFactor(BlendFactor::Zero), colorBlendOp(BlendOp::Add),
                            srcAlphaBlendFactor(BlendFactor::One), dstAlphaBlendFactor(BlendFactor::Zero), alphaBlendOp(BlendOp::Add),
                            blendConstants{1.0f, 1.0f, 1.0f, 1.0f} {
        for (u32 i = 0; i < constants::MAX_RENDER_TARGETS; ++i) {
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
    u32 queryCount;                ///< 查询数量
    
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
    virtual bool Submit(const QueueSubmitInfo& info) = 0;
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
    virtual void UpdateDescriptorSets(u32 writeCount, const WriteDescriptorSet* writes) = 0;
    virtual RHISwapChain* CreateSwapChain(const SwapChainDesc& desc) = 0;
    virtual void DestroySwapChain(RHISwapChain* swapChain) = 0;
    virtual ResourceHandle CreateBuffer(const BufferDesc& desc) = 0;
    virtual ResourceHandle CreateTexture(const TextureDesc& desc) = 0;
    virtual ResourceHandle CreateTextureView(const TextureViewDesc& desc) = 0;
    virtual ShaderHandle CreateShader(const void* data, size_t size, ShaderStage stage, const char* entryPoint = "main") = 0;
    virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) = 0;
    virtual RenderPassHandle CreateRenderPass(const RenderPassDesc& desc) = 0;
    virtual void DestroyRenderPass(RenderPassHandle handle) = 0;
    virtual CommandBufferHandle CreateCommandBuffer(CommandQueueType type) = 0;
    virtual void DestroyCommandBuffer(CommandBufferHandle handle) = 0;
    virtual void DestroyBuffer(ResourceHandle handle) = 0;
    virtual void DestroyTexture(ResourceHandle handle) = 0;
    virtual void DestroyShader(ShaderHandle handle) = 0;
    virtual void DestroyPipeline(PipelineHandle handle) = 0;
    virtual bool GetQueryPoolResults(QueryPoolHandle handle, u32 firstQuery, u32 queryCount, void* data, size_t stride) = 0;
    virtual void* MapBuffer(ResourceHandle handle, u64 offset = 0, u64 size = 0) = 0;
    virtual void UnmapBuffer(ResourceHandle handle) = 0;
    virtual void SetBufferDirtySize(ResourceHandle handle, u64 size) = 0;
    virtual double GetTimestampPeriod() const = 0;

    /**
     * @brief 获取设备平台类型
     * @return 设备运行的平台
     */
    virtual RHIPlatform GetPlatform() const = 0;

    /**
     * @brief 获取垃圾回收器
     * @return 垃圾回收器引用
     */
    virtual RHIGarbageCollector& GetGarbageCollector() = 0;
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
        
        frameCount_ = 0;
        gc_.Initialize();

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
        
        // 先清理所有待销毁的资源，因为它们可能依赖于设备对象
        gc_.Shutdown();
        derived().shutdownImpl();
        isValid_ = false;
    }
    
    /**
     * @brief 等待设备空闲
     */
    void WaitIdle() const override {
        assert(isValid_ && "Device not initialized");
        derived().waitIdleImpl();
        const_cast<RHIDevice*>(this)->gc_.Flush();
    }
    
    /**
     * @brief 开始新的一帧
     */
    void BeginFrame() {
        assert(isValid_ && "Device not initialized");
        frameCount_++;
        gc_.SetCurrentFrame(frameCount_);
        derived().beginFrameImpl();
    }
    
    /**
     * @brief 结束当前帧
     */
    void EndFrame() {
        assert(isValid_ && "Device not initialized");
        derived().endFrameImpl();
        
        // 假设最大飞行帧数为 2 (MaxFramesInFlight - 1)
        u64 completedFrame = frameCount_ > 2 ? frameCount_ - 2 : 0;
        gc_.Update(completedFrame);
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
    RHISwapChain* CreateSwapChain(const SwapChainDesc& desc) override {
        assert(isValid_ && "Device not initialized");
        return derived().createSwapChainImpl(desc);
    }

    /**
     * @brief 销毁交换链
     * @param swapChain 交换链指针
     */
    void DestroySwapChain(RHISwapChain* swapChain) override {
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
     * @brief 创建纹理视图
     * @param desc 纹理视图描述符
     * @return 资源句柄，失败返回INVALID_RESOURCE
     */
    ResourceHandle CreateTextureView(const TextureViewDesc& desc) override {
        assert(isValid_ && "Device not initialized");
        return derived().createTextureViewImpl(desc);
    }

    /**
     * @brief 创建着色器
     * @param data 着色器数据
     * @param size 数据大小
     * @param stage 着色器阶段
     * @param entryPoint 入口点函数名
     * @return 着色器句柄，失败返回INVALID_SHADER
     */
    ShaderHandle CreateShader(const void* data, size_t size, ShaderStage stage, const char* entryPoint = "main") override {
        assert(isValid_ && "Device not initialized");
        assert(data && size > 0 && "Invalid shader data");
        return derived().createShaderImpl(data, size, stage, entryPoint);
    }
    
    /**
     * @brief 创建图形管线
     * @param desc 管线描述符
     * @return 管线句柄，失败返回INVALID_PIPELINE
     */
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override {
        assert(isValid_ && "Device not initialized");
        return derived().createGraphicsPipelineImpl(desc);
    }
    
    /**
     * @brief 创建计算管线
     * @param desc 管线描述符
     * @return 管线句柄，失败返回INVALID_PIPELINE
     */
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override {
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
            gc_.DeferredDestroy([this, handle]() {
                derived().destroyDescriptorSetLayoutImpl(handle);
            });
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
            gc_.DeferredDestroy([this, handle]() {
                derived().destroyPipelineLayoutImpl(handle);
            });
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
            gc_.DeferredDestroy([this, handle]() {
                derived().destroyDescriptorSetImpl(handle);
            });
        }
    }

    /**
     * @brief 更新描述符集
     * @param writeCount 更新数量
     * @param writes 更新操作数组
     */
    void UpdateDescriptorSets(u32 writeCount, const WriteDescriptorSet* writes) override {
        assert(isValid_ && "Device not initialized");
        derived().updateDescriptorSetsImpl(writeCount, writes);
    }

    /**
     * @brief 创建渲染通道
     * @param desc 渲染通道描述符
     * @return 渲染通道句柄
     */
    RenderPassHandle CreateRenderPass(const RenderPassDesc& desc) override {
        assert(isValid_ && "Device not initialized");
        return derived().createRenderPassImpl(desc);
    }

    /**
     * @brief 销毁渲染通道
     * @param handle 渲染通道句柄
     */
    void DestroyRenderPass(RenderPassHandle handle) override {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_RESOURCE) {
            gc_.DeferredDestroy([this, handle]() {
                derived().destroyRenderPassImpl(handle);
            });
        }
    }

    /**
     * @brief 创建命令缓冲区
     * @param type 命令队列类型
     * @return 命令缓冲区句柄，失败返回INVALID_COMMAND_BUFFER
     */
    CommandBufferHandle CreateCommandBuffer(CommandQueueType type = CommandQueueType::Graphics) override {
        assert(isValid_ && "Device not initialized");
        return derived().createCommandBufferImpl(type);
    }
    
    /**
     * @brief 提交命令缓冲区
     * @param info 提交信息
     * @return 提交是否成功
     */
    bool Submit(const QueueSubmitInfo& info) override {
        assert(isValid_ && "Device not initialized");
        return derived().submitImpl(info);
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
    void DestroyShader(ShaderHandle handle) override {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_SHADER) {
            derived().destroyShaderImpl(handle);
        }
    }
    
    /**
     * @brief 销毁管线
     * @param handle 管线句柄
     */
    void DestroyPipeline(PipelineHandle handle) override {
        assert(isValid_ && "Device not initialized");
        if (handle != handles::INVALID_PIPELINE) {
            derived().destroyPipelineImpl(handle);
        }
    }

    bool GetQueryPoolResults(QueryPoolHandle handle, u32 firstQuery, u32 queryCount, void* data, size_t stride) override {
        assert(isValid_ && "Device not initialized");
        return derived().getQueryPoolResultsImpl(handle, firstQuery, queryCount, data, stride);
    }

    /**
     * @brief 映射缓冲区
     */
    void* MapBuffer(ResourceHandle handle, u64 offset = 0, u64 size = 0) override {
        assert(isValid_ && "Device not initialized");
        return derived().mapBufferImpl(handle, offset, size);
    }

    /**
     * @brief 取消映射缓冲区
     */
   void UnmapBuffer(ResourceHandle handle) override {
        assert(isValid_ && "Device not initialized");
        derived().unmapBufferImpl(handle);
    }

    void SetBufferDirtySize(ResourceHandle handle, u64 size) override {
        assert(isValid_ && "Device not initialized");
        derived().setBufferDirtySizeImpl(handle, size);
    }

    double GetTimestampPeriod() const override {
        assert(isValid_ && "Device not initialized");
        return derived().getTimestampPeriodImpl();
    }
    
    // === 辅助方法 ===
    /**
     * @brief 销毁命令缓冲区
     * @param handle 命令缓冲区句柄
     */
    void DestroyCommandBuffer(CommandBufferHandle handle) override {
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
    u32 GetCurrentFrameIndex() const {
        assert(isValid_ && "Device not initialized");
        return derived().getCurrentFrameIndexImpl();
    }
    
    /**
     * @brief 获取平台
     * @return 设备平台类型
     */
    RHIPlatform GetPlatform() const { return desc_.platform; }
    
    /**
     * @brief 获取垃圾回收器
     * @return 垃圾回收器引用
     */
    RHIGarbageCollector& GetGarbageCollector() override { return gc_; }

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
    RHIGarbageCollector gc_;             ///< 垃圾回收器
    u64 frameCount_ = 0;            ///< 帧计数器
    
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
    u32 RegisterDevice(RHIDeviceBase* device);
    
    /**
     * @brief 注销设备
     * @param deviceId 设备ID
     */
    void UnregisterDevice(u32 deviceId);
    
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
    RHIDeviceBase* GetDevice(u32 deviceId) const;
    
private:
    utl::vector<std::pair<u32, RHIDeviceBase*>> devices_;
    u32 nextDeviceId_ = 1;
};

// 全局设备管理器实例
extern RHIDeviceManager g_deviceManager;

} // namespace primal::graphics::rhi