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
template<typename T> class RHIAllocator;
class RHITexture;
struct Rect;
struct Viewport;
struct ColorBlendState;
struct DepthStencilState;
struct RasterizerState;

// === 管线阶段和访问标志枚举 ===

/**
 * @brief 管线阶段标志
 */
enum class PipelineStage : u32 {
    TopOfPipe = 0x00000001,
    DrawIndirect = 0x00000002,
    VertexInput = 0x00000004,
    VertexShader = 0x00000008,
    FragmentShader = 0x00000010,
    EarlyFragmentTests = 0x00000020,
    LateFragmentTests = 0x00000040,
    ColorAttachmentOutput = 0x00000080,
    ComputeShader = 0x00000100,
    Transfer = 0x00000200,
    BottomOfPipe = 0x00000400,
    Host = 0x00000800,
    AllGraphics = 0x00001000,
    AllCommands = 0x00002000
};

/**
 * @brief 访问标志
 */
enum class AccessFlag : u32 {
    IndirectCommandRead = 0x00000001,
    IndexRead = 0x00000002,
    VertexAttributeRead = 0x00000004,
    UniformRead = 0x00000008,
    InputAttachmentRead = 0x00000010,
    ShaderRead = 0x00000020,
    ShaderWrite = 0x00000040,
    ColorAttachmentRead = 0x00000080,
    ColorAttachmentWrite = 0x00000100,
    DepthStencilAttachmentRead = 0x00000200,
    DepthStencilAttachmentWrite = 0x00000400,
    TransferRead = 0x00000800,
    TransferWrite = 0x00001000,
    HostRead = 0x00002000,
    HostWrite = 0x00004000,
    MemoryRead = 0x00008000,
    MemoryWrite = 0x00010000
};

// 位运算操作符重载
constexpr AccessFlag operator|(AccessFlag lhs, AccessFlag rhs) {
    return static_cast<AccessFlag>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

constexpr PipelineStage operator|(PipelineStage lhs, PipelineStage rhs) {
    return static_cast<PipelineStage>(static_cast<u32>(lhs) | static_cast<u32>(rhs));
}

/**
 * @brief 获取命令缓冲区实例
 * @param handle 命令缓冲区句柄
 * @return 命令缓冲区指针，如果无效则返回nullptr
 */
RHICommandBuffer* GetCommandBuffer(CommandBufferHandle handle);

/**
 * @brief 注册命令缓冲区
 * @param cmd 命令缓冲区指针
 */
void RegisterCommandBuffer(RHICommandBuffer* cmd);

/**
 * @brief 注销命令缓冲区
 * @param handle 命令缓冲区句柄
 */
void UnregisterCommandBuffer(CommandBufferHandle handle);

/**
 * @brief 命令缓冲区状态枚举
 * @details 描述命令缓冲区的记录状态
 */
enum class CommandBufferState : u8 {
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
enum class CommandType : u16 {
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
    GenerateMipmaps = 706,
    
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
 * @brief 描述符集绑定信息
 * @details 描述如何绑定着色器资源
 */
struct DescriptorSetBinding {
    u32 setIndex;                          ///< 描述符集索引
    u32 dynamicOffsetCount;                ///< 动态偏移数量
    const u32* dynamicOffsets;             ///< 动态偏移数组
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
    u32 subresource;                       ///< 子资源索引
    u32 queueFamily;                       ///< 队列族索引
    
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
    u32 drawCallCount;                      ///< 绘制调用次数
    u32 computeDispatchCount;               ///< 计算分派次数
    u32 copyCommandCount;                   ///< 复制命令次数
    u32 barrierCount;                       ///< 屏障命令次数
    u32 renderPassCount;                    ///< 渲染通道次数
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
    template<typename T> friend class RHIAllocator;
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
        // if (state_ == CommandBufferState::Submitted) return false;
        
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

    // ========================================================================
    // P4c-F7: Secondary CommandBuffer / 并行录制(双方语义统一)
    //
    // 契约(两端一致):
    //   - BeginSecondaryCommandBuffer 在 primary 上调用,返回一个已完成
    //     Begin 的 secondary;secondary 内不得再 BeginRenderPass(拒绝);
    //     也不得 Submit/WaitForCompletion(secondary 不拥有 fence —— 同步
    //     由执行它的 primary 提交统一完成)。
    //   - ExecuteSecondaryCommandBuffers 在 primary 的 render pass 实例内
    //     调用(Vulkan 在 pass 内 vkCmdExecuteCommands;Metal 的 parallel
    //     子 encoder 命令自动汇入,Execute 为 no-op)。
    //   - 生命周期:secondary 挂到 primary 的提交帧,随 primary 一起回收;
    //     调用方在 primary WaitForCompletion 后 Destroy 即安全。
    // ========================================================================

    /**
     * @brief 创建并开始录制一个 secondary command buffer
     * @param desc 继承信息(Vulkan 的 VkCommandBufferInheritanceInfo;
     *             Metal 的 parallel 子 encoder 自动继承,字段忽略)
     * @return secondary 句柄(失败返回 INVALID_COMMAND_BUFFER)
     */
    virtual CommandBufferHandle BeginSecondaryCommandBuffer(const SecondaryCommandBufferDesc& desc) {
        (void)desc;
        return handles::INVALID_COMMAND_BUFFER;
    }

    /**
     * @brief 在当前 render pass 实例内执行一组 secondary(primary 调用)
     */
    virtual void ExecuteSecondaryCommandBuffers(u32 count, CommandBufferHandle* secondaries) {
        (void)count; (void)secondaries;
    }
    
    /**
     * @brief 提交命令到GPU
     * @param waitFlags 等待标志
     * @return 提交是否成功
     */
    virtual bool Submit(u32 waitFlags = 0) {
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
    
    /**
     * @brief 添加等待信号量
     * @param semaphore 信号量句柄
     * @param value 等待值
     */
    void AddWaitSemaphore(SyncHandle semaphore, u64 value) {
        waitSemaphores_.push_back({semaphore, value});
    }

    /**
     * @brief 添加发送信号量
     * @param semaphore 信号量句柄
     * @param value 发送值
     */
    void AddSignalSemaphore(SyncHandle semaphore, u64 value) {
        signalSemaphores_.push_back({semaphore, value});
    }

    // === 渲染命令 ===
    
    /**
     * @brief 开始渲染通道
     * @param desc 渲染通道描述符
     */
    virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;

    /**
     * @brief 开始渲染通道 (使用句柄)
     * @param renderPass 渲染通道句柄
     */
    virtual void BeginRenderPass(RenderPassHandle renderPass) = 0;
    
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
    virtual void BindVertexBuffers(u32 firstSlot, u32 slotCount,
                                   const ResourceHandle* buffers, const u64* offsets) = 0;
    
    /**
     * @brief 绑定索引缓冲区
     * @param buffer 索引缓冲区句柄
     * @param format 索引格式
     * @param offset 偏移量
     */
    virtual void BindIndexBuffer(ResourceHandle buffer, DataFormat format, u64 offset = 0) = 0;

    /**
     * @brief 绑定描述符集
     * @param bindPoint 绑定点（图形/计算）
     * @param pipelineLayout 管线布局句柄
     * @param firstSet 第一个描述符集索引
     * @param setCount 描述符集数量
     * @param descriptorSets 描述符集句柄数组
     * @param dynamicOffsetCount 动态偏移数量
     * @param dynamicOffsets 动态偏移数组
     */
    virtual void BindDescriptorSets(PipelineBindPoint bindPoint,
                                   PipelineLayoutHandle pipelineLayout,
                                   u32 firstSet,
                                   u32 setCount,
                                   const DescriptorSetHandle* descriptorSets,
                                   u32 dynamicOffsetCount,
                                   const u32* dynamicOffsets) = 0;

    /**
     * @brief 推送常量
     * @param layout 管线布局句柄
     * @param stageFlags 着色器阶段
     * @param offset 偏移量
     * @param size 大小
     * @param pValues 数据指针
     */
    virtual void PushConstants(PipelineLayoutHandle layout, ShaderStage stageFlags,
                              u32 offset, u32 size, const void* pValues) = 0;

    /**
     * @brief 设置计算着色器小常量数据 (Metal setBytes)
     * @param index 缓冲区绑定索引
     * @param data 数据指针
     * @param size 数据大小
     */
    virtual void SetComputeBytes(u32 index, const void* data, u32 size) = 0;

    /**
     * @brief 写入时间戳
     * @param queryPool 查询池句柄
     * @param queryIndex 查询索引
     */
    virtual void WriteTimestamp(QueryPoolHandle queryPool, u32 queryIndex) = 0;

    /**
     * @brief 重置查询池中的查询（GPU 端，vkCmdResetQueryPool）
     * @param queryPool 查询池句柄
     * @param firstQuery 起始 query 索引
     * @param queryCount 重置数量
     * @details T4.6.5 part 24.2 (B6 fix): Vulkan spec 要求 query 在 cmd buffer
     *          使用前 reset。GPU 端 reset 比 CPU 端 vkResetQueryPool 在某些
     *          验证层/MoltenVK 路径上更可靠。Metal/Dawn 实现为 no-op。
     */
    virtual void ResetQueryPool(QueryPoolHandle queryPool, u32 firstQuery, u32 queryCount) {}
    
    /**
     * @brief 绘制
     * @param vertexCount 顶点数量
     * @param startVertex 起始顶点
     * @param instanceCount 实例数量
     * @param startInstance 起始实例
     */
    virtual void Draw(u32 vertexCount, u32 startVertex = 0,
                     u32 instanceCount = 1, u32 startInstance = 0) = 0;
    
    /**
     * @brief 绘制索引
     * @param indexCount 索引数量
     * @param startIndex 起始索引
     * @param baseVertex 基础顶点
     * @param instanceCount 实例数量
     * @param startInstance 起始实例
     */
    virtual void DrawIndexed(u32 indexCount, u32 startIndex = 0,
                            u32 baseVertex = 0, u32 instanceCount = 1,
                            u32 startInstance = 0) = 0;
    
    /**
     * @brief 绘制间接
     * @param buffer 间接参数缓冲区
     * @param offset 偏移量
     * @param drawCount 绘制次数
     */
    virtual void DrawIndirect(ResourceHandle buffer, u64 offset = 0, u32 drawCount = 1) = 0;
    
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
    virtual void Dispatch(u32 groupCountX, u32 groupCountY, u32 groupCountZ) = 0;
    
    /**
     * @brief 间接计算分派
     * @param buffer 间接参数缓冲区
     * @param offset 偏移量
     */
    virtual void DispatchIndirect(ResourceHandle buffer, u64 offset = 0) = 0;

    /**
     * @brief 内存屏障
     * @details 确保之前的内存操作对后续操作可见
     * @param srcStageMask 源管线阶段掩码
     * @param dstStageMask 目标管线阶段掩码
     * @param srcAccessMask 源访问掩码
     * @param dstAccessMask 目标访问掩码
     */
    virtual void MemoryBarrier(PipelineStage srcStageMask, PipelineStage dstStageMask,
                              AccessFlag srcAccessMask, AccessFlag dstAccessMask) = 0;

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
                            u64 srcOffset = 0, u64 dstOffset = 0, u64 size = 0) = 0;
    
    /**
     * @brief 复制缓冲区到纹理
     * @param srcBuffer 源缓冲区
     * @param dstTexture 目标纹理
     * @param regions 复制区域数组
     * @param regionCount 区域数量
     */
    virtual void CopyBufferToTexture(ResourceHandle srcBuffer, ResourceHandle dstTexture,
                                     const BufferTextureCopyRegion* regions, u32 regionCount) = 0;

    /**
     * @brief 复制纹理到缓冲区
     * @param srcTexture 源纹理
     * @param dstBuffer 目标缓冲区
     * @param regions 复制区域数组
     * @param regionCount 区域数量
     */
    virtual void CopyTextureToBuffer(ResourceHandle srcTexture, ResourceHandle dstBuffer,
                                     const BufferTextureCopyRegion* regions, u32 regionCount) = 0;

    /**
     * @brief 纹理Blit
     * @param src 源纹理
     * @param dst 目标纹理
     * @param regions Blit区域数组
     * @param regionCount 区域数量
     * @param filter 过滤模式
     */
    virtual void BlitTexture(ResourceHandle src, ResourceHandle dst,
                             const TextureBlitRegion* regions, u32 regionCount,
                             FilterMode filter) = 0;

    /**
     * @brief 生成Mipmap
     * @param texture 纹理句柄
     */
    virtual void GenerateMipmaps(ResourceHandle texture) = 0;

    /**
     * @brief 插入资源屏障
     * @param barrier 屏障描述符
     * @param barrierCount 屏障数量
     */
    virtual void InsertBarrier(const ResourceBarrier* barriers, u32 barrierCount) = 0;
    
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

    /**
     * @brief 设置状态
     * @param state 新状态
     */
    void SetState(CommandBufferState state) {
        state_ = state;
    }
    
protected:
    // === 派生类必须实现的虚函数 ===
    
    virtual void destroyImpl() {}
    virtual bool resetImpl() = 0;
    virtual bool beginImpl() = 0;
    virtual bool endImpl() = 0;
    virtual bool submitImpl(u32 waitFlags) = 0;
    virtual bool waitForCompletionImpl() = 0;
    
    // === 受保护的成员变量 ===
    
    struct SemaphoreInfo {
        SyncHandle semaphore;
        u64 value;
    };
	utl::vector<SemaphoreInfo> waitSemaphores_;
	utl::vector<SemaphoreInfo> signalSemaphores_;

    RHIDeviceBase& device_;              ///< 设备引用
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
            case CommandType::GenerateMipmaps:
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