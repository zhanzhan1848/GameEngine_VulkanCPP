/**
 * @file RHICommand.h
 * @brief RHI命令缓冲区基类定义
 * @details 提供平台无关的命令记录和执行接口
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-29
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include "RHITypes.h"

namespace primal::graphics::rhi {

// === 前向声明 ===
class RHIDeviceBase;
class RHIBuffer;
class RHITexture;
struct Rect;
struct Viewport;
struct ColorBlendState;
struct DepthStencilState;
struct RasterizerState;



/**
 * @brief 加载操作枚举
 * @details 定义渲染目标加载时的操作类型
 */
enum class LoadAction {
    Load = 0,           ///< 加载现有内容
    Clear = 1,          ///< 清除为指定值
    DontCare = 2        ///< 不关心原有内容
};

/**
 * @brief 存储操作枚举
 * @details 定义渲染目标存储时的操作类型
 */
enum class StoreAction {
    Store = 0,          ///< 存储渲染结果
    DontCare = 1        ///< 不关心渲染结果
};

/**
 * @brief 命令缓冲区状态枚举
 * @details 描述命令缓冲区的记录状态
 */
enum class CommandBufferState : uint8_t {
    Reset = 0,          ///< 已重置，可以开始记录
    Recording = 1,       ///< 正在记录命令
    RecordingEnded = 2,  ///< 记录已结束，等待提交
    Submitted = 3,       ///< 已提交到GPU，等待执行
    Executed = 4,       ///< 执行完成，可以重置
    Invalid = 5         ///< 无效状态
};

/**
 * @brief 命令类型枚举
 * @details 标识不同类型的渲染命令
 */
enum class CommandType : uint16_t {
    Unknown = 0,
    
    // === 资源管理命令 ===
    BeginFrame = 100,
    EndFrame = 101,
    
    // === 渲染通道命令 ===
    BeginRenderPass = 200,
    EndRenderPass = 201,
    SetViewport = 202,
    SetScissor = 203,
    SetBlendConstants = 204,
    SetStencilRef = 205,
    
    // === 管线绑定命令 ===
    BindGraphicsPipeline = 300,
    BindComputePipeline = 301,
    
    // === 资源绑定命令 ===
    BindVertexBuffers = 400,
    BindIndexBuffer = 401,
    BindDescriptorSets = 402,
    
    // === 绘制命令 ===
    Draw = 500,
    DrawInstanced = 501,
    DrawIndexed = 502,
    DrawIndexedInstanced = 503,
    DrawIndirect = 504,
    DrawIndexedIndirect = 505,
    
    // === 计算命令 ===
    Dispatch = 600,
    DispatchIndirect = 601,
    
    // === 资源操作命令 ===
    CopyBuffer = 700,
    CopyTexture = 701,
    CopyBufferToTexture = 702,
    CopyTextureToBuffer = 703,
    BlitTexture = 704,
    ResolveTexture = 705,
    
    // === 清除命令 ===
    ClearRenderTarget = 800,
    ClearDepthStencil = 801,
    ClearBuffer = 802,
    ClearTexture = 803,
    
    // === 同步命令 ===
    Barrier = 900,
    InsertDebugMarker = 901,
    BeginDebugMarker = 902,
    EndDebugMarker = 903,
    
    // === 立即执行命令 ===
    ImmediateExecute = 1000
};

/**
 * @brief 渲染通道描述符
 * @details 定义渲染通道的配置参数
 */
struct RenderPassDesc {
    struct Attachment {
        ResourceHandle texture;           ///< 渲染目标纹理
        DataFormat format;                ///< 数据格式
        LoadAction loadOp;                ///< 加载操作
        StoreAction storeOp;              ///< 存储操作
        ClearValue clearValue;            ///< 清除值
        u32 sampleCount;           ///< 采样数量
        uint8_t mipLevel;                 ///< Mip层级
        uint16_t arrayLayer;              ///< 数组层级
        
        Attachment() : texture(handles::INVALID_RESOURCE), format(DataFormat::Unknown),
                      loadOp(LoadAction::DontCare), storeOp(StoreAction::Store),
                      sampleCount(1), mipLevel(0), arrayLayer(0) {}
    };
    
    utl::vector<Attachment> colorAttachments;   ///< 颜色附件
    Attachment depthAttachment;                 ///< 深度附件
    Attachment stencilAttachment;              ///< 模板附件
    
    ViewportDesc viewport;                       ///< 视口
    Rect scissor;                               ///< 裁剪矩形
    
    RenderPassDesc() {
        colorAttachments.reserve(constants::MAX_RENDER_TARGETS);
    }
};

/**
 * @brief 描述符集绑定信息
 * @details 描述如何绑定着色器资源
 */
struct DescriptorSetBinding {
    uint32_t setIndex;                          ///< 描述符集索引
    uint32_t dynamicOffsetCount;                ///< 动态偏移数量
    const uint32_t* dynamicOffsets;             ///< 动态偏移数组
    ResourceHandle descriptorSet;               ///< 描述符集句柄
    
    DescriptorSetBinding() : setIndex(0), dynamicOffsetCount(0), dynamicOffsets(nullptr),
                            descriptorSet(handles::INVALID_RESOURCE) {}
};

/**
 * @brief 资源屏障描述符
 * @details 描述资源状态转换
 */
struct ResourceBarrier {
    ResourceHandle resource;                    ///< 资源句柄
    ResourceState beforeState;                   ///< 转换前状态
    ResourceState afterState;                    ///< 转换后状态
    uint32_t subresource;                       ///< 子资源索引
    uint32_t queueFamily;                       ///< 队列族索引
    
    ResourceBarrier() : resource(handles::INVALID_RESOURCE),
                       beforeState(ResourceState::Unknown),
                       afterState(ResourceState::Unknown),
                       subresource(0xFFFFFFFF),
                       queueFamily(0xFFFFFFFF) {}
};

/**
 * @brief 命令统计信息
 * @details 用于性能分析和调试
 */
struct CommandStats {
    uint32_t drawCallCount;                      ///< 绘制调用次数
    uint32_t computeDispatchCount;               ///< 计算分派次数
    uint32_t copyCommandCount;                   ///< 复制命令次数
    uint32_t barrierCount;                       ///< 屏障命令次数
    uint32_t renderPassCount;                    ///< 渲染通道次数
    float commandRecordingTime;                  ///< 命令记录时间（毫秒）
    float commandExecutionTime;                  ///< 命令执行时间（毫秒）
    
    CommandStats() : drawCallCount(0), computeDispatchCount(0),
                    copyCommandCount(0), barrierCount(0),
                    renderPassCount(0), commandRecordingTime(0.0f),
                    commandExecutionTime(0.0f) {}
};

/**
 * @brief RHI命令缓冲区基类
 * @details 提供命令记录和执行的核心接口
 */
class RHICommandBuffer {
public:
    // === 构造函数和析构函数 ===
    
    /**
     * @brief 构造函数
     * @param device 设备引用
     * @param type 命令队列类型
     */
    explicit RHICommandBuffer(RHIDeviceBase& device, CommandQueueType type)
        : device_(device), type_(type), handle_(handles::INVALID_COMMAND_BUFFER),
          state_(CommandBufferState::Reset), stats_() {}
    
    /**
     * @brief 虚析构函数
     */
    virtual ~RHICommandBuffer() {
        if (state_ != CommandBufferState::Invalid) {
            Destroy();
        }
    }
    
    // === 禁用拷贝，支持移动 ===
    
    RHICommandBuffer(const RHICommandBuffer&) = delete;
    RHICommandBuffer& operator=(const RHICommandBuffer&) = delete;
    
    RHICommandBuffer(RHICommandBuffer&& other) noexcept
        : device_(other.device_), type_(other.type_), handle_(other.handle_),
          state_(other.state_), stats_(other.stats_) {
        other.handle_ = handles::INVALID_COMMAND_BUFFER;
        other.state_ = CommandBufferState::Invalid;
        other.stats_ = CommandStats();
    }
    
    RHICommandBuffer& operator=(RHICommandBuffer&& other) noexcept;
    
    // === 核心接口方法 ===
    
    /**
     * @brief 初始化命令缓冲区
     * @return 初始化是否成功
     */
    virtual bool Initialize() = 0;
    
    /**
     * @brief 销毁命令缓冲区
     */
    virtual void Destroy() {
        if (state_ != CommandBufferState::Invalid && handle_ != handles::INVALID_COMMAND_BUFFER) {
            destroyImpl();
            handle_ = handles::INVALID_COMMAND_BUFFER;
            state_ = CommandBufferState::Invalid;
        }
    }
    
    /**
     * @brief 重置命令缓冲区
     * @return 重置是否成功
     */
    virtual bool Reset() {
        if (state_ == CommandBufferState::Invalid) return false;
        if (state_ == CommandBufferState::Submitted) return false;
        
        bool result = resetImpl();
        if (result) {
            state_ = CommandBufferState::Reset;
            stats_ = CommandStats();
        }
        return result;
    }
    
    /**
     * @brief 开始记录命令
     * @return 记录是否成功开始
     */
    virtual bool Begin() {
        if (state_ != CommandBufferState::Reset && state_ != CommandBufferState::Executed) {
            return false;
        }
        
        bool result = beginImpl();
        if (result) {
            state_ = CommandBufferState::Recording;
        }
        return result;
    }
    
    /**
     * @brief 结束记录命令
     * @return 记录是否成功结束
     */
    virtual bool End() {
        if (state_ != CommandBufferState::Recording) {
            return false;
        }
        
        bool result = endImpl();
        if (result) {
            state_ = CommandBufferState::RecordingEnded;
        }
        return result;
    }
    
    /**
     * @brief 提交命令到GPU
     * @param waitFlags 等待标志
     * @return 提交是否成功
     */
    virtual bool Submit(uint32_t waitFlags = 0) {
        if (state_ != CommandBufferState::RecordingEnded) {
            return false;
        }
        
        bool result = submitImpl(waitFlags);
        if (result) {
            state_ = CommandBufferState::Submitted;
        }
        return result;
    }
    
    /**
     * @brief 等待执行完成
     * @return 等待是否成功
     */
    virtual bool WaitForCompletion() {
        if (state_ != CommandBufferState::Submitted) {
            return false;
        }
        
        bool result = waitForCompletionImpl();
        if (result) {
            state_ = CommandBufferState::Executed;
        }
        return result;
    }
    
    // === 渲染命令 ===
    
    /**
     * @brief 开始渲染通道
     * @param desc 渲染通道描述符
     */
    virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
    
    /**
     * @brief 结束渲染通道
     */
    virtual void EndRenderPass() = 0;
    
    /**
     * @brief 设置视口
     * @param viewport 视口描述符
     */
    virtual void SetViewport(const ViewportDesc& viewport) = 0;
    
    /**
     * @brief 设置裁剪矩形
     * @param scissor 裁剪矩形
     */
    virtual void SetScissor(const Rect& scissor) = 0;
    
    /**
     * @brief 绑定图形管线
     * @param pipeline 管线句柄
     */
    virtual void BindGraphicsPipeline(PipelineHandle pipeline) = 0;
    
    /**
     * @brief 绑定顶点缓冲区
     * @param firstSlot 起始槽位
     * @param slotCount 槽位数量
     * @param buffers 缓冲区句柄数组
     * @param offsets 偏移量数组
     */
    virtual void BindVertexBuffers(uint32_t firstSlot, uint32_t slotCount,
                                   const ResourceHandle* buffers, const uint64_t* offsets) = 0;
    
    /**
     * @brief 绑定索引缓冲区
     * @param buffer 索引缓冲区句柄
     * @param format 索引格式
     * @param offset 偏移量
     */
    virtual void BindIndexBuffer(ResourceHandle buffer, DataFormat format, uint64_t offset = 0) = 0;
    
    /**
     * @brief 绘制
     * @param vertexCount 顶点数量
     * @param startVertex 起始顶点
     * @param instanceCount 实例数量
     * @param startInstance 起始实例
     */
    virtual void Draw(uint32_t vertexCount, uint32_t startVertex = 0,
                     uint32_t instanceCount = 1, uint32_t startInstance = 0) = 0;
    
    /**
     * @brief 绘制索引
     * @param indexCount 索引数量
     * @param startIndex 起始索引
     * @param baseVertex 基础顶点
     * @param instanceCount 实例数量
     * @param startInstance 起始实例
     */
    virtual void DrawIndexed(uint32_t indexCount, uint32_t startIndex = 0,
                            uint32_t baseVertex = 0, uint32_t instanceCount = 1,
                            uint32_t startInstance = 0) = 0;
    
    /**
     * @brief 绘制间接
     * @param buffer 间接参数缓冲区
     * @param offset 偏移量
     * @param drawCount 绘制次数
     */
    virtual void DrawIndirect(ResourceHandle buffer, uint64_t offset = 0, uint32_t drawCount = 1) = 0;
    
    // === 计算命令 ===
    
    /**
     * @brief 绑定计算管线
     * @param pipeline 管线句柄
     */
    virtual void BindComputePipeline(PipelineHandle pipeline) = 0;
    
    /**
     * @brief 计算分派
     * @param groupCountX X轴工作组数量
     * @param groupCountY Y轴工作组数量
     * @param groupCountZ Z轴工作组数量
     */
    virtual void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
    
    /**
     * @brief 间接计算分派
     * @param buffer 间接参数缓冲区
     * @param offset 偏移量
     */
    virtual void DispatchIndirect(ResourceHandle buffer, uint64_t offset = 0) = 0;
    
    // === 资源操作命令 ===
    
    /**
     * @brief 复制缓冲区
     * @param src 源缓冲区
     * @param dst 目标缓冲区
     * @param srcOffset 源偏移量
     * @param dstOffset 目标偏移量
     * @param size 复制大小
     */
    virtual void CopyBuffer(ResourceHandle src, ResourceHandle dst,
                            uint64_t srcOffset = 0, uint64_t dstOffset = 0, uint64_t size = 0) = 0;
    
    /**
     * @brief 插入资源屏障
     * @param barrier 屏障描述符
     * @param barrierCount 屏障数量
     */
    virtual void InsertBarrier(const ResourceBarrier* barriers, uint32_t barrierCount) = 0;
    
    // === 访问器方法 ===
    
    /**
     * @brief 获取命令缓冲区句柄
     * @return 命令缓冲区句柄
     */
    CommandBufferHandle GetHandle() const { return handle_; }
    
    /**
     * @brief 获取命令队列类型
     * @return 命令队列类型
     */
    CommandQueueType GetType() const { return type_; }
    
    /**
     * @brief 获取当前状态
     * @return 命令缓冲区状态
     */
    CommandBufferState GetState() const { return state_; }
    
    /**
     * @brief 检查是否有效
     * @return 命令缓冲区是否有效
     */
    bool IsValid() const {
        return state_ != CommandBufferState::Invalid && 
               handle_ != handles::INVALID_COMMAND_BUFFER;
    }
    
    /**
     * @brief 检查是否可以记录命令
     * @return 是否可以记录
     */
    bool CanRecord() const {
        return state_ == CommandBufferState::Recording;
    }
    
    /**
     * @brief 检查是否可以提交
     * @return 是否可以提交
     */
    bool CanSubmit() const {
        return state_ == CommandBufferState::RecordingEnded;
    }
    
    /**
     * @brief 获取统计信息
     * @return 统计信息的常量引用
     */
    const CommandStats& GetStats() const { return stats_; }
    
protected:
    // === 派生类必须实现的虚函数 ===
    
    virtual void destroyImpl() = 0;
    virtual bool resetImpl() = 0;
    virtual bool beginImpl() = 0;
    virtual bool endImpl() = 0;
    virtual bool submitImpl(uint32_t waitFlags) = 0;
    virtual bool waitForCompletionImpl() = 0;
    
    // === 受保护的成员变量 ===
    
    RHIDeviceBase& device_;                   ///< 设备引用
    CommandQueueType type_;               ///< 命令队列类型
    CommandBufferHandle handle_;          ///< 命令缓冲区句柄
    CommandBufferState state_;            ///< 当前状态
    CommandStats stats_;                  ///< 统计信息
    
    /**
     * @brief 设置命令缓冲区句柄
     * @param handle 句柄
     */
    void SetHandle(CommandBufferHandle handle) {
        handle_ = handle;
    }
    
    /**
     * @brief 设置状态
     * @param state 新状态
     */
    void SetState(CommandBufferState state) {
        state_ = state;
    }
    
    /**
     * @brief 更新统计信息
     * @param commandType 命令类型
     */
    void UpdateStats(CommandType commandType) {
        switch (commandType) {
            case CommandType::Draw:
            case CommandType::DrawInstanced:
            case CommandType::DrawIndexed:
            case CommandType::DrawIndexedInstanced:
            case CommandType::DrawIndirect:
            case CommandType::DrawIndexedIndirect:
                stats_.drawCallCount++;
                break;
                
            case CommandType::Dispatch:
            case CommandType::DispatchIndirect:
                stats_.computeDispatchCount++;
                break;
                
            case CommandType::CopyBuffer:
            case CommandType::CopyTexture:
            case CommandType::CopyBufferToTexture:
            case CommandType::CopyTextureToBuffer:
            case CommandType::BlitTexture:
            case CommandType::ResolveTexture:
                stats_.copyCommandCount++;
                break;
                
            case CommandType::Barrier:
                stats_.barrierCount++;
                break;
                
            case CommandType::BeginRenderPass:
                stats_.renderPassCount++;
                break;
                
            default:
                break;
        }
    }
};

} // namespace primal::graphics::rhi