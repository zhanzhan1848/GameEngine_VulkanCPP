/**
 * @file RHICommand.cpp
 * @brief RHI命令缓冲区基类实现
 * @details 命令缓冲区基类的非虚函数实现和工具函数
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-29
 * @version 0.1.0
 */

#include "RHICommand.h"
#include "RHIDevice.h"
#include <algorithm>
#include <unordered_map>
#include <mutex>

namespace primal::graphics::rhi {

// === 命令缓冲区管理器 ===

/**
 * @brief 命令缓冲区管理器
 * @details 管理所有命令缓冲区的生命周期和统计信息
 */
class CommandBufferManager {
public:
    /**
     * @brief 获取单例实例
     * @return 命令缓冲区管理器单例引用
     */
    static CommandBufferManager& Instance() {
        static CommandBufferManager instance;
        return instance;
    }
    
    /**
     * @brief 注册命令缓冲区
     * @param commandBuffer 命令缓冲区指针
     */
    void RegisterCommandBuffer(RHICommandBuffer* commandBuffer) {
        if (commandBuffer) {
            std::lock_guard<std::mutex> lock(mutex_);
            commandBuffers_[commandBuffer->GetHandle()] = commandBuffer;
        }
    }
    
    /**
     * @brief 注销命令缓冲区
     * @param handle 命令缓冲区句柄
     */
    void UnregisterCommandBuffer(CommandBufferHandle handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        commandBuffers_.erase(handle);
    }
    
    /**
     * @brief 获取命令缓冲区
     * @param handle 命令缓冲区句柄
     * @return 命令缓冲区指针，失败返回nullptr
     */
    RHICommandBuffer* GetCommandBuffer(CommandBufferHandle handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = commandBuffers_.find(handle);
        return (it != commandBuffers_.end()) ? it->second : nullptr;
    }
    
    /**
     * @brief 等待所有命令缓冲区执行完成
     */
    void WaitForAllCommandBuffers() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [handle, commandBuffer] : commandBuffers_) {
            if (commandBuffer && commandBuffer->GetState() == CommandBufferState::Submitted) {
                commandBuffer->WaitForCompletion();
            }
        }
    }
    
    /**
     * @brief 销毁所有命令缓冲区
     */
    void DestroyAllCommandBuffers() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [handle, commandBuffer] : commandBuffers_) {
            if (commandBuffer) {
                commandBuffer->Destroy();
                delete commandBuffer;
            }
        }
        commandBuffers_.clear();
    }
    
private:
    CommandBufferManager() = default;
    ~CommandBufferManager() = default;
    
    mutable std::mutex mutex_;
    std::unordered_map<CommandBufferHandle, RHICommandBuffer*> commandBuffers_;
};

// === 命令缓冲区工厂 ===

/**
 * @brief 命令缓冲区工厂类
 * @details 用于创建不同类型的命令缓冲区
 */
class CommandBufferFactory {
public:
    /**
     * @brief 创建命令缓冲区
     * @param device 设备引用
     * @param type 命令队列类型
     * @return 命令缓冲区指针，失败返回nullptr
     */
    template<typename CommandType>
    static std::unique_ptr<CommandType> CreateCommandBuffer(RHIDevice& device, CommandQueueType type) {
        std::unique_ptr<CommandType> commandBuffer = std::make_unique<CommandType>(device, type);
        if (commandBuffer && commandBuffer->Initialize()) {
            return commandBuffer;
        }
        return nullptr;
    }
    
    /**
     * @brief 销毁命令缓冲区
     * @param commandBuffer 命令缓冲区指针
     */
    template<typename CommandType>
    static void DestroyCommandBuffer(CommandType* commandBuffer) {
        if (commandBuffer) {
            commandBuffer->Destroy();
            delete commandBuffer;
        }
    }
};

// === 命令验证工具 ===

/**
 * @brief 验证渲染通道描述符
 * @param desc 渲染通道描述符
 * @return 描述符是否有效
 */
bool ValidateRenderPassDesc(const RenderPassDesc& desc) {
    // 检查颜色附件
    for (size_t i = 0; i < desc.colorAttachments.size(); ++i) {
        const auto& attachment = desc.colorAttachments[i];
        if (attachment.texture == handles::INVALID_RESOURCE) {
            // 空附件是允许的（某些渲染目标可能未使用）
            continue;
        }
        
        if (attachment.format == DataFormat::Unknown) {
            return false;
        }
        
        if (attachment.sampleCount == SampleCount::Unknown) {
            return false;
        }
    }
    
    // 检查深度附件
    if (desc.depthAttachment.texture != handles::INVALID_RESOURCE) {
        if (desc.depthAttachment.format == DataFormat::Unknown) {
            return false;
        }
        
        if (!IsDepthFormat(desc.depthAttachment.format)) {
            return false;
        }
    }
    
    // 检查模板附件
    if (desc.stencilAttachment.texture != handles::INVALID_RESOURCE) {
        if (desc.stencilAttachment.format == DataFormat::Unknown) {
            return false;
        }
        
        if (!IsStencilFormat(desc.stencilAttachment.format)) {
            return false;
        }
    }
    
    return true;
}

/**
 * @brief 验证资源屏障描述符
 * @param barrier 资源屏障描述符
 * @return 屏障是否有效
 */
bool ValidateResourceBarrier(const ResourceBarrier& barrier) {
    if (barrier.resource == handles::INVALID_RESOURCE) {
        return false;
    }
    
    if (barrier.beforeState == ResourceState::Unknown || 
        barrier.afterState == ResourceState::Unknown) {
        return false;
    }
    
    if (barrier.beforeState == barrier.afterState) {
        return false; // 状态相同，不需要屏障
    }
    
    return IsValidStateTransition(barrier.beforeState, barrier.afterState);
}

/**
 * @brief 验证描述符集绑定
 * @param binding 描述符集绑定信息
 * @return 绑定是否有效
 */
bool ValidateDescriptorSetBinding(const DescriptorSetBinding& binding) {
    if (binding.descriptorSet == handles::INVALID_RESOURCE) {
        return false;
    }
    
    if (binding.dynamicOffsetCount > 0 && !binding.dynamicOffsets) {
        return false;
    }
    
    return true;
}

// === 命令状态转换工具 ===

/**
 * @brief 获取命令缓冲区状态的文本描述
 * @param state 状态枚举
 * @return 状态描述字符串
 */
const char* GetCommandBufferStateName(CommandBufferState state) {
    switch (state) {
        case CommandBufferState::Reset: return "Reset";
        case CommandBufferState::Recording: return "Recording";
        case CommandBufferState::RecordingEnded: return "RecordingEnded";
        case CommandBufferState::Submitted: return "Submitted";
        case CommandBufferState::Executed: return "Executed";
        case CommandBufferState::Invalid: return "Invalid";
        default: return "Unknown";
    }
}

/**
 * @brief 检查状态转换是否合法
 * @param from 起始状态
 * @param to 目标状态
 * @return 转换是否合法
 */
bool IsValidCommandBufferStateTransition(CommandBufferState from, CommandBufferState to) {
    // 状态转换规则表
    static const bool transitionTable[6][6] = {
        // From: Reset, Recording, RecordingEnded, Submitted, Executed, Invalid
        /* To: Reset         */ {true,  false, false, false,   true,   false},
        /*      Recording    */ {true,  true,  false, false,   true,   false},
        /*      RecordingEnded*/{false, true,  true,  false,   false,  false},
        /*      Submitted    */{false, false, true,  true,    false,  false},
        /*      Executed     */{true,  false, false, false,   true,   false},
        /*      Invalid      */{false, false, false, false,   false,  true }
    };
    
    int fromIndex = static_cast<int>(from);
    int toIndex = static_cast<int>(to);
    
    if (fromIndex < 0 || fromIndex >= 6 || toIndex < 0 || toIndex >= 6) {
        return false;
    }
    
    return transitionTable[toIndex][fromIndex];
}

// === 命令类型工具 ===

/**
 * @brief 获取命令类型的文本描述
 * @param type 命令类型枚举
 * @return 命令类型描述字符串
 */
const char* GetCommandTypeName(CommandType type) {
    switch (type) {
        case CommandType::Unknown: return "Unknown";
        
        // 资源管理命令
        case CommandType::BeginFrame: return "BeginFrame";
        case CommandType::EndFrame: return "EndFrame";
        
        // 渲染通道命令
        case CommandType::BeginRenderPass: return "BeginRenderPass";
        case CommandType::EndRenderPass: return "EndRenderPass";
        case CommandType::SetViewport: return "SetViewport";
        case CommandType::SetScissor: return "SetScissor";
        case CommandType::SetBlendConstants: return "SetBlendConstants";
        case CommandType::SetStencilRef: return "SetStencilRef";
        
        // 管线绑定命令
        case CommandType::BindGraphicsPipeline: return "BindGraphicsPipeline";
        case CommandType::BindComputePipeline: return "BindComputePipeline";
        
        // 资源绑定命令
        case CommandType::BindVertexBuffers: return "BindVertexBuffers";
        case CommandType::BindIndexBuffer: return "BindIndexBuffer";
        case CommandType::BindDescriptorSets: return "BindDescriptorSets";
        
        // 绘制命令
        case CommandType::Draw: return "Draw";
        case CommandType::DrawInstanced: return "DrawInstanced";
        case CommandType::DrawIndexed: return "DrawIndexed";
        case CommandType::DrawIndexedInstanced: return "DrawIndexedInstanced";
        case CommandType::DrawIndirect: return "DrawIndirect";
        case CommandType::DrawIndexedIndirect: return "DrawIndexedIndirect";
        
        // 计算命令
        case CommandType::Dispatch: return "Dispatch";
        case CommandType::DispatchIndirect: return "DispatchIndirect";
        
        // 资源操作命令
        case CommandType::CopyBuffer: return "CopyBuffer";
        case CommandType::CopyTexture: return "CopyTexture";
        case CommandType::CopyBufferToTexture: return "CopyBufferToTexture";
        case CommandType::CopyTextureToBuffer: return "CopyTextureToBuffer";
        case CommandType::BlitTexture: return "BlitTexture";
        case CommandType::ResolveTexture: return "ResolveTexture";
        
        // 清除命令
        case CommandType::ClearRenderTarget: return "ClearRenderTarget";
        case CommandType::ClearDepthStencil: return "ClearDepthStencil";
        case CommandType::ClearBuffer: return "ClearBuffer";
        case CommandType::ClearTexture: return "ClearTexture";
        
        // 同步命令
        case CommandType::Barrier: return "Barrier";
        case CommandType::InsertDebugMarker: return "InsertDebugMarker";
        case CommandType::BeginDebugMarker: return "BeginDebugMarker";
        case CommandType::EndDebugMarker: return "EndDebugMarker";
        
        // 立即执行命令
        case CommandType::ImmediateExecute: return "ImmediateExecute";
        
        default: return "Unknown";
    }
}

/**
 * @brief 检查命令是否需要在渲染通道内执行
 * @param type 命令类型
 * @return 是否需要渲染通道
 */
bool IsCommandInsideRenderPass(CommandType type) {
    switch (type) {
        case CommandType::Draw:
        case CommandType::DrawInstanced:
        case CommandType::DrawIndexed:
        case CommandType::DrawIndexedInstanced:
        case CommandType::DrawIndirect:
        case CommandType::DrawIndexedIndirect:
        case CommandType::SetViewport:
        case CommandType::SetScissor:
        case CommandType::SetBlendConstants:
        case CommandType::SetStencilRef:
        case CommandType::BindGraphicsPipeline:
        case CommandType::BindVertexBuffers:
        case CommandType::BindIndexBuffer:
        case CommandType::BindDescriptorSets:
        case CommandType::ClearRenderTarget:
        case CommandType::ClearDepthStencil:
            return true;
            
        default:
            return false;
    }
}

/**
 * @brief 检查命令是否是渲染通道控制命令
 * @param type 命令类型
 * @return 是否是渲染通道控制命令
 */
bool IsRenderPassControlCommand(CommandType type) {
    return type == CommandType::BeginRenderPass || 
           type == CommandType::EndRenderPass;
}

// === 调试和日志工具 ===

/**
 * @brief 打印命令缓冲区信息
 * @param commandBuffer 命令缓冲区指针
 * @param verbose 是否输出详细信息
 */
void PrintCommandBufferInfo(const RHICommandBuffer* commandBuffer, bool verbose) {
    if (!commandBuffer) {
        printf("CommandBuffer: nullptr\n");
        return;
    }
    
    const auto& stats = commandBuffer->GetStats();
    
    printf("CommandBuffer Info:\n");
    printf("  Handle: 0x%016llx\n", static_cast<unsigned long long>(commandBuffer->GetHandle()));
    printf("  Type: %d\n", static_cast<int>(commandBuffer->GetType()));
    printf("  State: %s\n", GetCommandBufferStateName(commandBuffer->GetState()));
    printf("  IsValid: %s\n", commandBuffer->IsValid() ? "true" : "false");
    printf("  CanRecord: %s\n", commandBuffer->CanRecord() ? "true" : "false");
    printf("  CanSubmit: %s\n", commandBuffer->CanSubmit() ? "true" : "false");
    
    if (verbose) {
        printf("Statistics:\n");
        printf("  Draw Calls: %u\n", stats.drawCallCount);
        printf("  Compute Dispatches: %u\n", stats.computeDispatchCount);
        printf("  Copy Commands: %u\n", stats.copyCommandCount);
        printf("  Barriers: %u\n", stats.barrierCount);
        printf("  Render Passes: %u\n", stats.renderPassCount);
        printf("  Recording Time: %.3f ms\n", stats.commandRecordingTime);
        printf("  Execution Time: %.3f ms\n", stats.commandExecutionTime);
    }
}

/**
 * @brief 打印渲染通道描述符
 * @param desc 渲染通道描述符
 */
void PrintRenderPassDesc(const RenderPassDesc& desc) {
    printf("RenderPass Description:\n");
    printf("  Color Attachments (%zu):\n", desc.colorAttachments.size());
    for (size_t i = 0; i < desc.colorAttachments.size(); ++i) {
        const auto& attachment = desc.colorAttachments[i];
        printf("    [%zu]: Texture=0x%016llx, Format=%d, LoadOp=%d, StoreOp=%d\n",
               i, static_cast<unsigned long long>(attachment.texture),
               static_cast<int>(attachment.format),
               static_cast<int>(attachment.loadOp),
               static_cast<int>(attachment.storeOp));
    }
    
    if (desc.depthAttachment.texture != handles::INVALID_RESOURCE) {
        printf("  Depth Attachment: Texture=0x%016llx, Format=%d\n",
               static_cast<unsigned long long>(desc.depthAttachment.texture),
               static_cast<int>(desc.depthAttachment.format));
    }
    
    if (desc.stencilAttachment.texture != handles::INVALID_RESOURCE) {
        printf("  Stencil Attachment: Texture=0x%016llx, Format=%d\n",
               static_cast<unsigned long long>(desc.stencilAttachment.texture),
               static_cast<int>(desc.stencilAttachment.format));
    }
    
    printf("  Viewport: (%.1f,%.1f)-(%.1f,%.1f)\n",
           desc.viewport.topLeft.x, desc.viewport.topLeft.y,
           desc.viewport.size.x, desc.viewport.size.y);
    printf("  Scissor: (%d,%d)-(%d,%d)\n",
           desc.scissor.left, desc.scissor.top,
           desc.scissor.right, desc.scissor.bottom);
}

} // namespace primal::graphics::rhi