/**
 * @file RHITypes.h
 * @brief RHI系统基础类型定义
 * @details 定义RHI系统使用的句柄类型、枚举、常量等基础类型
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-29
 * @version 0.1.0
 */

#pragma once

#include "CommonHeaders.h"
#include "../../../Platform/PlatformTypes.h"

namespace primal::graphics::rhi {

// === 常量定义 ===

/**
 * @brief 最大同时在飞行的帧数（多缓冲）
 */
constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

/**
 * @brief 所有子资源掩码
 */
constexpr uint32_t RHI_ALL_SUBRESOURCES = ~0u;

// === 前向声明 ===

class RHIResource;
class RHICommandBuffer;
class RHISwapChain;
class RHIShader;
class RHIPipeline;

// === 句柄类型定义 ===

/**
 * @brief RHI设备句柄
 * @details 64位句柄，包含设备类型和唯一标识符
 */
using DeviceHandle = uint64_t;

/**
 * @brief RHI交换链句柄
 * @details 64位句柄，用于标识交换链对象
 */
using SwapChainHandle = uint64_t;

/**
 * @brief RHI资源句柄
 * @details 64位句柄，用于标识缓冲区、纹理等GPU资源
 */
using ResourceHandle = uint64_t;

/**
 * @brief RHI命令缓冲区句柄
 * @details 64位句柄，用于标识命令缓冲区
 */
using CommandBufferHandle = uint64_t;

/**
 * @brief RHI着色器句柄
 * @details 64位句柄，用于标识着色器对象
 */
using ShaderHandle = uint64_t;

/**
 * @brief RHI管线句柄
 * @details 64位句柄，用于标识渲染管线或计算管线
 */
using PipelineHandle = uint64_t;

/**
 * @brief RHI采样器句柄
 * @details 64位句柄，用于标识采样器状态
 */
using SamplerHandle = uint64_t;

/**
 * @brief RHI同步对象句柄
 * @details 64位句柄，用于标识围栏、信号量等同步对象
 */
using SyncHandle = uint64_t;

/**
 * @brief RHI查询池句柄
 * @details 64位句柄，用于标识查询池对象
 */
using QueryPoolHandle = uint64_t;

/**
 * @brief RHI描述符集布局句柄
 * @details 64位句柄，用于标识描述符集布局对象
 */
using DescriptorSetLayoutHandle = uint64_t;

/**
 * @brief RHI描述符集句柄
 * @details 64位句柄，用于标识描述符集对象
 */
using DescriptorSetHandle = uint64_t;

/**
 * @brief RHI ECS 实体 ID
 * @details 32位整数，用于标识 RHI ECS 中的实体
 */
using RHIEntityID = uint32_t;
constexpr RHIEntityID INVALID_RHI_ENTITY_ID = 0xFFFFFFFF;

/**
 * @brief RHI渲染通道句柄
 * @details 64位句柄，用于标识渲染通道对象
 */
using RenderPassHandle = uint64_t;

/**
 * @brief RHI管线布局句柄
 * @details 64位句柄，用于标识管线布局对象
 */
using PipelineLayoutHandle = uint64_t;

/**
 * @brief 无效句柄常量
 */
namespace handles {
    constexpr DeviceHandle INVALID_DEVICE = static_cast<DeviceHandle>(-1);
    constexpr ResourceHandle INVALID_RESOURCE = static_cast<ResourceHandle>(-1);
    constexpr CommandBufferHandle INVALID_COMMAND_BUFFER = static_cast<CommandBufferHandle>(-1);
    constexpr ShaderHandle INVALID_SHADER = static_cast<ShaderHandle>(-1);
    constexpr PipelineHandle INVALID_PIPELINE = static_cast<PipelineHandle>(-1);
    constexpr PipelineLayoutHandle INVALID_PIPELINE_LAYOUT = static_cast<PipelineLayoutHandle>(-1);
    constexpr SamplerHandle INVALID_SAMPLER = static_cast<SamplerHandle>(-1);
    constexpr SyncHandle INVALID_SYNC = static_cast<SyncHandle>(-1);
    constexpr QueryPoolHandle INVALID_QUERY_POOL = static_cast<QueryPoolHandle>(-1);
    constexpr DescriptorSetLayoutHandle INVALID_DESCRIPTOR_SET_LAYOUT = static_cast<DescriptorSetLayoutHandle>(-1);
    constexpr DescriptorSetHandle INVALID_DESCRIPTOR_SET = static_cast<DescriptorSetHandle>(-1);
    constexpr RenderPassHandle INVALID_RENDER_PASS = static_cast<RenderPassHandle>(-1);
}

/**
 * @brief 队列提交信息
 * @details 描述提交到命令队列的命令缓冲区和同步原语
 */
struct QueueSubmitInfo {
    CommandBufferHandle cmdBuffer{handles::INVALID_COMMAND_BUFFER};
    SyncHandle waitSemaphore{handles::INVALID_SYNC};   ///< 等待的信号量 (GPU wait)
    SyncHandle signalSemaphore{handles::INVALID_SYNC}; ///< 发出的信号量 (GPU signal)
    SyncHandle signalFence{handles::INVALID_SYNC};     ///< 发出的栅栏 (CPU wait)
};

// === 枚举定义 ===

/**
 * @brief RHI支持的图形API平台
 */
enum class RHIPlatform : uint8_t {
    Unknown = 0,    ///< 未知平台
    D3D12   = 1,    ///< DirectX 12
    Vulkan  = 2,    ///< Vulkan
    Metal   = 3,    ///< Apple Metal
    Dawn    = 4     ///< WebGPU (Dawn)
};

/**
 * @brief GPU资源类型
 */
enum class ResourceType : uint8_t {
    Unknown     = 0,    ///< 未知类型
    Buffer      = 1,    ///< 缓冲区
    Texture     = 2,    ///< 纹理
    RenderTarget = 3,   ///< 渲染目标
    DepthStencil = 4,   ///< 深度模板缓冲
    Pipeline    = 5,    ///< 管线
    Shader      = 6,    ///< 着色器
    Sampler     = 7,    ///< 采样器
    QueryPool   = 8,    ///< 查询池
    SwapChain   = 9,    ///< 交换链
    DescriptorSetLayout = 10, ///< 描述符集布局
    DescriptorSet = 11, ///< 描述符集
    RenderPass  = 12,   ///< 渲染通道
    PipelineLayout = 13 ///< 管线布局
};

/**
 * @brief 管线绑定点
 */
enum class PipelineBindPoint : uint8_t {
    Graphics = 0,   ///< 图形管线
    Compute = 1     ///< 计算管线
};

/**
 * @brief 查询类型
 */
enum class QueryType : uint8_t {
    Timestamp,  ///< 时间戳查询
    Occlusion,  ///< 遮挡查询
    PipelineStatistics ///< 管线统计查询
};

/**
 * @brief 查询结果标志
 */
enum class QueryResultFlags : uint8_t {
    None = 0,
    Wait = 1 << 0,      ///< 等待结果可用
    v64 = 1 << 1        ///< 结果为64位
};

inline QueryResultFlags operator|(QueryResultFlags a, QueryResultFlags b) {
    return static_cast<QueryResultFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline QueryResultFlags operator&(QueryResultFlags a, QueryResultFlags b) {
    return static_cast<QueryResultFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

/**
 * @brief 资源状态枚举
 * @details 描述资源的当前状态和生命周期
 */
enum class ResourceState : uint8_t {
    Unknown = 0,        ///< 未知状态
    Created = 1,        ///< 已创建，但未分配GPU内存
    Allocated = 2,      ///< 已分配GPU内存
    PendingUpload = 3,  ///< 等待数据上传
    Ready = 4,          ///< 资源就绪，可以使用
    InUse = 5,          ///< 正在被GPU使用
    PendingDestroy = 6,  ///< 等待销毁
    Destroyed = 7,       ///< 已销毁
    
    // 资源布局状态 (Pipeline States)
    General = 8,         ///< 通用状态 (Vulkan: GENERAL, D3D12: COMMON)
    ShaderResource = 9,  ///< 着色器资源 (Vulkan: SHADER_READ_ONLY_OPTIMAL, D3D12: PIXEL/NON_PIXEL_SHADER_RESOURCE)
    RenderTarget = 10,   ///< 渲染目标 (Vulkan: COLOR_ATTACHMENT_OPTIMAL, D3D12: RENDER_TARGET)
    DepthStencil = 11,   ///< 深度模板读写 (Vulkan: DEPTH_STENCIL_ATTACHMENT_OPTIMAL, D3D12: DEPTH_WRITE)
    DepthStencilReadOnly = 12, ///< 深度模板只读 (Vulkan: DEPTH_STENCIL_READ_ONLY_OPTIMAL, D3D12: DEPTH_READ)
    UnorderedAccess = 13, ///< 无序访问 (Vulkan: GENERAL, D3D12: UNORDERED_ACCESS)
    CopySource = 14,     ///< 复制源 (Vulkan: TRANSFER_SRC_OPTIMAL, D3D12: COPY_SOURCE)
    CopyDest = 15,       ///< 复制目标 (Vulkan: TRANSFER_DST_OPTIMAL, D3D12: COPY_DEST)
    Present = 16,        ///< 呈现 (Vulkan: PRESENT_SRC_KHR, D3D12: PRESENT)
    ResolveSource = 17,  ///< 解析源
    ResolveDest = 18,    ///< 解析目标
    IndirectArgument = 19 ///< 间接参数
};

/**
 * @brief 缓冲区类型
 */
enum class BufferType : uint8_t {
    Unknown             = 0,    ///< 未知类型
    Vertex              = 1,    ///< 顶点缓冲区
    Index               = 2,    ///< 索引缓冲区
    Constant            = 3,    ///< 常量缓冲区
    Structured          = 4,    ///< 结构化缓冲区
    Raw                 = 5,    ///< 原始缓冲区
    Indirect            = 6,    ///< 间接绘制缓冲区
    AccelerationStructure = 7   ///< 加速结构缓冲区
};

/**
 * @brief 缓冲区用途标志
 */
enum class BufferUsageFlags : uint32_t {
    None = 0,
    TransferSrc = 1 << 0,
    TransferDst = 1 << 1,
    UniformTexel = 1 << 2,
    StorageTexel = 1 << 3,
    Uniform = 1 << 4,
    Storage = 1 << 5,
    Index = 1 << 6,
    Vertex = 1 << 7,
    Indirect = 1 << 8
};

inline BufferUsageFlags operator|(BufferUsageFlags a, BufferUsageFlags b) {
    return static_cast<BufferUsageFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline BufferUsageFlags operator&(BufferUsageFlags a, BufferUsageFlags b) {
    return static_cast<BufferUsageFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/**
 * @brief 描述符绑定标志
 */
enum class DescriptorBindingFlags : uint32_t {
    None = 0,
    UpdateAfterBind = 1 << 0,           ///< 绑定后更新
    UpdateUnusedWhilePending = 1 << 1,  ///< 挂起时更新未使用
    PartiallyBound = 1 << 2,            ///< 部分绑定 (Bindless)
    VariableDescriptorCount = 1 << 3    ///< 可变描述符数量
};

inline DescriptorBindingFlags operator|(DescriptorBindingFlags a, DescriptorBindingFlags b) {
    return static_cast<DescriptorBindingFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline DescriptorBindingFlags operator&(DescriptorBindingFlags a, DescriptorBindingFlags b) {
    return static_cast<DescriptorBindingFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/**
 * @brief 纹理类型
 */
enum class TextureType : uint8_t {
    Unknown     = 0,    ///< 未知类型
    Texture1D   = 1,    ///< 1D纹理
    Texture2D   = 2,    ///< 2D纹理
    Texture3D   = 3,    ///< 3D纹理
    TextureCube = 4,    ///< 立方纹理
    Texture1DArray = 5, ///< 1D纹理数组
    Texture2DArray = 6, ///< 2D纹理数组
    TextureCubeArray = 7 ///< 立方纹理数组
};

/**
 * @brief 纹理方面掩码
 */
enum class TextureAspect : uint8_t {
    Unknown = 0,
    Color = 1,          ///< 颜色分量
    Depth = 2,          ///< 深度分量
    Stencil = 4,        ///< 模板分量
    Metadata = 8        ///< 元数据
};

inline TextureAspect operator|(TextureAspect a, TextureAspect b) {
    return static_cast<TextureAspect>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline TextureAspect operator&(TextureAspect a, TextureAspect b) {
    return static_cast<TextureAspect>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

/**
 * @brief 纹理用途
 */
enum class TextureUsage : uint32_t {
    Unknown = 0x00000000,
    ShaderResource = 0x00000001,    ///< 着色器资源
    RenderTarget = 0x00000002,       ///< 渲染目标
    DepthStencil = 0x00000004,       ///< 深度模板缓冲
    UnorderedAccess = 0x00000008,    ///< 无序访问视图
    CopySource = 0x00000010,         ///< 复制源
    CopyDest = 0x00000020,           ///< 复制目标
    ResolveSource = 0x00000040,      ///< 解析源
    ResolveDest = 0x00000080,        ///< 解析目标
    Present = 0x00000100             ///< 呈现目标
};

inline TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline TextureUsage operator&(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/**
 * @brief 数据格式枚举
 */
enum class DataFormat : uint16_t {
    Unknown = 0,
    
    // 8位格式
    R8_UNorm = 1,
    R8_SNorm = 2,
    R8_UInt = 3,
    R8_SInt = 4,
    
    // 16位格式
    R16_UNorm = 5,
    R16_SNorm = 6,
    R16_UInt = 7,
    R16_SInt = 8,
    R16_Float = 9,
    
    RG8_UNorm = 10,
    RG8_SNorm = 11,
    RG8_UInt = 12,
    RG8_SInt = 13,
    
    // 24位格式
    R8G8B8_UNorm = 14,
    R8G8B8_SNorm = 15,
    R8G8B8_UInt = 16,
    R8G8B8_SInt = 17,
    
    // 32位格式
    R32_UNorm = 18,
    R32_SNorm = 19,
    R32_UInt = 20,
    R32_SInt = 21,
    R32_Float = 22,
    
    RG16_UNorm = 23,
    RG16_SNorm = 24,
    RG16_UInt = 25,
    RG16_SInt = 26,
    RG16_Float = 27,
    
    RG8B8A8_UNorm = 28,
    RG8B8A8_SNorm = 29,
    RG8B8A8_UInt = 30,
    RG8B8A8_SInt = 31,
    
    BGRA8_UNorm = 32,
    BGRA8_SNorm = 33,
    BGRA8_UInt = 34,
    BGRA8_SInt = 35,
    
    RGBA8_UNorm = 36,
    RGBA8_SNorm = 37,
    RGBA8_UInt = 38,
    RGBA8_SInt = 39,
    RGBA8_sRGB = 40,
    
    RG32_UNorm = 41,
    RG32_SNorm = 42,
    RG32_UInt = 43,
    RG32_SInt = 44,
    RG32_Float = 45,
    
    RGB32_UNorm = 46,
    RGB32_SNorm = 47,
    RGB32_UInt = 48,
    RGB32_SInt = 49,
    RGB32_Float = 50,
    
    RGBA16_UNorm = 51,
    RGBA16_SNorm = 52,
    RGBA16_UInt = 53,
    RGBA16_SInt = 54,
    RGBA16_Float = 55,
    
    RGBA32_UNorm = 56,
    RGBA32_SNorm = 57,
    RGBA32_UInt = 58,
    RGBA32_SInt = 59,
    RGBA32_Float = 60,
    RGBA32_sRGB = 61,
    
    // 压缩格式
    BC1_UNorm = 62,
    BC1_sRGB = 63,
    BC2_UNorm = 64,
    BC2_sRGB = 65,
    BC3_UNorm = 66,
    BC3_sRGB = 67,
    BC4_UNorm = 68,
    BC4_SNorm = 69,
    BC5_UNorm = 70,
    BC5_SNorm = 71,
    BC6H_UF16 = 72,
    BC6H_SF16 = 73,
    BC7_UNorm = 74,
    BC7_sRGB = 75,
    
    // 深度模板格式
    D16_UNorm = 76,
    D24_UNorm_S8_UInt = 77,
    D32_Float = 78,
    D32_Float_S8X24_UInt = 79
};

/**
 * @brief 索引数据类型
 */
enum class DataIndexType : uint8_t {
    Unknown = 0,
    UInt16 = 1,    ///< 16位无符号整数索引
    UInt32 = 2     ///< 32位无符号整数索引
};

/**
 * @brief GPU内存使用模式
 */
enum class GPUMemoryUsage : uint8_t {
    Unknown = 0,
    Static = 1,    ///< 静态内存，CPU只写一次，GPU多次读取
    Dynamic = 2,   ///< 动态内存，CPU频繁更新，GPU多次读取
    Staging = 3,   ///< 暂存内存，用于CPU到GPU的数据传输
    Readback = 4,  ///< 回读内存，用于GPU到CPU的数据传输
    Immutable = 5, ///< 不可变内存，CPU写一次后不再修改
    SwapChain = 6  ///< 交换链内存，由交换链管理，不直接创建
};

/**
 * @brief 命令队列类型
 */
enum class CommandQueueType : uint8_t {
    Unknown = 0,
    Graphics = 1,  ///< 图形队列，支持图形和计算操作
    Compute = 2,   ///< 计算队列，仅支持计算操作
    Transfer = 3   ///< 传输队列，仅支持数据传输操作
};

/**
 * @brief 图元拓扑类型
 */
enum class PrimitiveTopology : uint8_t {
    Unknown = 0,
    PointList = 1,      ///< 点列表
    LineList = 2,       ///< 线列表
    LineStrip = 3,      ///< 线带
    TriangleList = 4,   ///< 三角形列表
    TriangleStrip = 5,  ///< 三角形带
    LineListAdj = 6,    ///< 邻接线列表
    LineStripAdj = 7,   ///< 邻接线带
    TriangleListAdj = 8, ///< 邻接三角形列表
    TriangleStripAdj = 9 ///< 邻接三角形带
};

/**
 * @brief 着色器阶段
 */
enum class ShaderStage : uint8_t {
    Unknown = 0,
    Vertex = 1 << 0,     ///< 顶点着色器
    Pixel = 1 << 1,      ///< 像素着色器
    Geometry = 1 << 2,   ///< 几何着色器
    Hull = 1 << 3,       ///< 外壳着色器
    Domain = 1 << 4,     ///< 域着色器
    Compute = 1 << 5     ///< 计算着色器
};

inline ShaderStage operator|(ShaderStage a, ShaderStage b) {
    return static_cast<ShaderStage>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline ShaderStage operator&(ShaderStage a, ShaderStage b) {
    return static_cast<ShaderStage>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

/**
 * @brief 描述符类型
 */
enum class DescriptorType : uint8_t {
    Unknown = 0,
    Sampler = 1,
    CombinedImageSampler = 2,
    SampledImage = 3,
    StorageImage = 4,
    UniformTexelBuffer = 5,
    StorageTexelBuffer = 6,
    UniformBuffer = 7,
    StorageBuffer = 8,
    UniformBufferDynamic = 9,
    StorageBufferDynamic = 10,
    InputAttachment = 11
};

/**
 * @brief 描述符集布局绑定
 */
struct DescriptorSetLayoutBinding {
    uint32_t binding{ 0 };
    DescriptorType descriptorType{ DescriptorType::Unknown };
    uint32_t descriptorCount{ 0 };
    ShaderStage stageFlags{ ShaderStage::Unknown };
    const SamplerHandle* immutableSamplers{ nullptr };
    DescriptorBindingFlags flags{ DescriptorBindingFlags::None };
};

/**
 * @brief 描述符集布局描述符
 */
struct DescriptorSetLayoutDesc {
    uint32_t bindingCount{ 0 };
    const DescriptorSetLayoutBinding* bindings{ nullptr };
};

/**
 * @brief 描述符集描述符
 */
struct DescriptorSetDesc {
    DescriptorSetLayoutHandle layout{ handles::INVALID_RESOURCE };
};

/**
 * @brief 描述符图像信息
 */
struct DescriptorImageInfo {
    SamplerHandle sampler{ handles::INVALID_SAMPLER };
    ResourceHandle imageView{ handles::INVALID_RESOURCE }; // 纹理句柄
    ResourceState imageLayout{ ResourceState::Unknown };
};

/**
 * @brief 描述符缓冲区信息
 */
struct DescriptorBufferInfo {
    ResourceHandle buffer{ handles::INVALID_RESOURCE };
    uint64_t offset{ 0 };
    uint64_t range{ 0 };
};

/**
 * @brief 写描述符集
 */
struct WriteDescriptorSet {
    DescriptorSetHandle dstSet{ handles::INVALID_RESOURCE };
    uint32_t dstBinding{ 0 };
    uint32_t dstArrayElement{ 0 };
    uint32_t descriptorCount{ 0 };
    DescriptorType descriptorType{ DescriptorType::Unknown };
    const DescriptorImageInfo* imageInfo{ nullptr };
    const DescriptorBufferInfo* bufferInfo{ nullptr };
};

/**
 * @brief 混合操作
 */
enum class BlendOp : uint8_t {
    Unknown = 0,
    Add = 1,        ///< 相加
    Subtract = 2,   ///< 相减
    RevSubtract = 3, ///< 反向相减
    Min = 4,        ///< 最小值
    Max = 5         ///< 最大值
};

/**
 * @brief 混合因子
 */
enum class BlendFactor : uint8_t {
    Unknown = 0,
    Zero = 1,           ///< 零
    One = 2,            ///< 一
    SrcColor = 3,       ///< 源颜色
    InvSrcColor = 4,    ///< 反源颜色
    SrcAlpha = 5,       ///< 源Alpha
    InvSrcAlpha = 6,    ///< 反源Alpha
    DestAlpha = 7,      ///< 目标Alpha
    InvDestAlpha = 8,   ///< 反目标Alpha
    DestColor = 9,      ///< 目标颜色
    InvDestColor = 10,  ///< 反目标颜色
    SrcAlphaSat = 11,   ///< 源Alpha饱和
    BlendFactor = 12,   ///< 混合因子
    InvBlendFactor = 13, ///< 反混合因子
    Src1Color = 14,     ///< 源1颜色
    InvSrc1Color = 15,  ///< 反源1颜色
    Src1Alpha = 16,     ///< 源1Alpha
    InvSrc1Alpha = 17   ///< 反源1Alpha
};

/**
 * @brief 比较函数
 */
enum class ComparisonFunc : uint8_t {
    Unknown = 0,
    Never = 1,          ///< 永不通过
    Less = 2,           ///< 小于通过
    Equal = 3,          ///< 等于通过
    LessEqual = 4,      ///< 小于等于通过
    Greater = 5,        ///< 大于通过
    NotEqual = 6,       ///< 不等于通过
    GreaterEqual = 7,   ///< 大于等于通过
    Always = 8          ///< 总是通过
};

/**
 * @brief 模板操作
 */
enum class StencilOp : uint8_t {
    Unknown = 0,
    Keep = 1,           ///< 保持
    Zero = 2,           ///< 置零
    Replace = 3,        ///< 替换
    IncSat = 4,         ///< 饱和递增
    DecSat = 5,         ///< 饱和递减
    Invert = 6,         ///< 反转
    Inc = 7,            ///< 递增
    Dec = 8             ///< 递减
};

/**
 * @brief 填充模式
 */
enum class FillMode : uint8_t {
    Unknown = 0,
    Solid = 1,          ///< 实心填充
    Wireframe = 2       ///< 线框填充
};

/**
 * @brief 裁剪模式
 */
enum class CullMode : uint8_t {
    Unknown = 0,
    None = 1,           ///< 不裁剪
    Front = 2,          ///< 裁剪前面
    Back = 3            ///< 裁剪背面
};

/**
 * @brief 纹理过滤模式
 */
enum class FilterMode : uint8_t {
    Unknown = 0,
    Point = 1,          ///< 点过滤
    Linear = 2,         ///< 线性过滤
    Anisotropic = 3,    ///< 各向异性过滤
    Nearest = 1         ///< 最近邻过滤 (Point)
};

/**
 * @brief 纹理寻址模式
 */
enum class TextureAddressMode : uint8_t {
    Unknown = 0,
    Wrap = 1,           ///< 重复
    Mirror = 2,         ///< 镜像
    Clamp = 3,          ///< 限制
    Border = 4,         ///< 边框
    MirrorOnce = 5      ///< 单次镜像
};

/**
 * @brief 呈现模式
 */
enum class PresentMode : uint8_t {
    Immediate = 0,      ///< 立即呈现（可能撕裂）
    Mailbox = 1,        ///< 邮箱模式（三缓冲，低延迟，无撕裂）
    FIFO = 2,           ///< 先进先出（垂直同步，标准）
    FIFO_Relaxed = 3    ///< 宽松FIFO（如果错过垂直同步，则立即呈现）
};

// === 常量定义 ===

/**
 * @brief RHI系统常量
 */
namespace constants {
    constexpr uint32_t MAX_RENDER_TARGETS = 8;           ///< 最大渲染目标数量
    constexpr uint32_t MAX_VERTEX_BUFFERS = 16;         ///< 最大顶点缓冲区数量
    constexpr uint32_t MAX_TEXTURE_UNITS = 32;          ///< 最大纹理单元数量
    constexpr uint32_t MAX_SAMPLERS = 16;               ///< 最大采样器数量
    constexpr uint32_t MAX_CONSTANT_BUFFERS = 14;        ///< 最大常量缓冲区数量
    constexpr uint32_t MAX_VIEWPORTS = 16;              ///< 最大视口数量
    constexpr uint32_t MAX_SCISSOR_RECTS = 16;          ///< 最大裁剪矩形数量
    constexpr uint32_t MAX_VERTEX_INPUT_ATTRIBUTES = 16; ///< 最大顶点输入属性数量
    constexpr uint32_t MAX_COLOR_ATTACHMENTS = 8;       ///< 最大颜色附件数量
    constexpr uint32_t MAX_SHADER_STAGES = 6;            ///< 最大着色器阶段数
    constexpr uint32_t MAX_PUSH_CONSTANTS_SIZE = 256;    ///< 最大推送常量大小（字节）
    constexpr uint32_t MAX_UBO_SIZE = 64 * 1024;          ///< 最大统一缓冲区大小（64KB）
    constexpr uint32_t MAX_SSBO_SIZE = 128 * 1024 * 1024; ///< 最大存储缓冲区大小（128MB）
    constexpr uint32_t MIN_UNIFORM_BUFFER_OFFSET_ALIGNMENT = 256; ///< 最小统一缓冲区对齐
    constexpr uint32_t FRAME_COUNT = 3;                 ///< 帧缓冲数量（三重缓冲）
    constexpr float MAX_ANISOTROPY = 16.0f;              ///< 最大各向异性
}



// === 状态描述符结构体 ===

/**
 * @brief 模板操作描述符
 */
struct StencilOpDesc {
    StencilOp failOp{ StencilOp::Keep };       ///< 模板测试失败操作
    StencilOp depthFailOp{ StencilOp::Keep };  ///< 深度测试失败操作
    StencilOp passOp{ StencilOp::Keep };       ///< 模板/深度测试通过操作
    ComparisonFunc func{ ComparisonFunc::Always };    ///< 比较函数
};

/**
 * @brief 混合状态描述符
 */
struct BlendState {
    bool enableBlend{ false };                   ///< 是否启用混合
    BlendFactor srcColorBlendFactor{ BlendFactor::One };    ///< 源颜色混合因子
    BlendFactor dstColorBlendFactor{ BlendFactor::Zero };    ///< 目标颜色混合因子
    BlendOp colorBlendOp{ BlendOp::Add };               ///< 颜色混合操作
    BlendFactor srcAlphaBlendFactor{ BlendFactor::One };    ///< 源Alpha混合因子
    BlendFactor dstAlphaBlendFactor{ BlendFactor::Zero };    ///< 目标Alpha混合因子
    BlendOp alphaBlendOp{ BlendOp::Add };               ///< Alpha混合操作
    math::v4 blendConstants{1.0f, 1.0f, 1.0f, 1.0f};            ///< 混合常量
};

/**
 * @brief 深度模板状态描述符
 */
struct DepthStencilState {
    bool enableDepthTest{ true };               ///< 是否启用深度测试
    bool enableDepthWrite{ true };              ///< 是否启用深度写入
    ComparisonFunc depthFunc{ ComparisonFunc::Less };           ///< 深度比较函数
    
    bool enableStencilTest{ false };             ///< 是否启用模板测试
    uint8_t stencilReadMask{ 0xFF };           ///< 模板读取掩码
    uint8_t stencilWriteMask{ 0xFF };           ///< 模板写入掩码
    
    StencilOpDesc frontStencil{};         ///< 正面模板操作
    StencilOpDesc backStencil{};          ///< 背面模板操作
};

/**
 * @brief 光栅化状态描述符
 */
struct RasterizerState {
    FillMode fillMode{ FillMode::Solid };                  ///< 填充模式
    CullMode cullMode{ CullMode::Back };                  ///< 裁剪模式
    PrimitiveTopology topology{ PrimitiveTopology::TriangleList };         ///< 图元拓扑 (注意：通常这属于 Input Assembly，但这里方便管理放在一起，或者 Material 单独管理 Topology)
    
    // Depth Bias
    float depthBias{ 0.0f };                    ///< 深度偏差常数因子
    float depthBiasClamp{ 0.0f };               ///< 深度偏差截断
    float slopeScaledDepthBias{ 0.0f };         ///< 深度偏差斜率因子
};

// === 基础结构体定义 ===

/**
 * @brief 视口描述符
 */
struct ViewportDesc {
    math::v2 topLeft{ 0.0f, 0.0f };        ///< 视口左上角坐标 (x, y)
    math::v2 size{ 1.0f, 1.0f };           ///< 视口大小 (width, height)
    float minDepth{ 0.0f };          ///< 最小深度值
    float maxDepth{ 1.0f };          ///< 最大深度值
};

/**
 * @brief 裁剪矩形描述符
 */
struct Rect {
    math::s32v2 offset{ 0, 0 };      ///< 裁剪矩形偏移量 (x, y)
    math::u32v2 extent{ 0, 0 };      ///< 裁剪矩形大小 (width, height)
};

/**
 * @brief 清除值联合体
 */
struct ClearValue {
    union {
        math::v4 color;      ///< 颜色清除值 (r, g, b, a)
        struct {
            float depth;     ///< 深度清除值
            uint32_t stencil; ///< 模板清除值
        };
        math::v4 depthStencil; ///< 深度模板清除值
    };
};

/**
 * @brief 纹理子资源层
 */
struct TextureSubresourceLayers {
    uint32_t mipLevel{ 0 };       ///< Mip层级
    uint32_t baseArrayLayer{ 0 }; ///< 起始数组层
    uint32_t layerCount{ 1 };     ///< 数组层数量
};

/**
 * @brief 3D 偏移量
 */
struct Offset3D {
    int32_t x{ 0 };
    int32_t y{ 0 };
    int32_t z{ 0 };
};

/**
 * @brief 3D 范围
 */
struct Extent3D {
    uint32_t width{ 0 };
    uint32_t height{ 0 };
    uint32_t depth{ 0 };
};

/**
 * @brief 缓冲区到纹理的复制区域
 */
struct BufferTextureCopyRegion {
    uint64_t bufferOffset{ 0 };       ///< 缓冲区偏移量
    uint32_t bufferRowLength{ 0 };    ///< 缓冲区行长（像素），0表示紧密排列
    uint32_t bufferImageHeight{ 0 };  ///< 缓冲区图像高度（像素），0表示紧密排列
    TextureSubresourceLayers imageSubresource{ 0, 0, 1 }; ///< 纹理子资源
    Offset3D imageOffset{ 0, 0, 0 };        ///< 纹理偏移
    Extent3D imageExtent{ 0, 0, 0 };        ///< 纹理范围
};

/**
 * @brief 纹理复制区域
 */
struct TextureCopyRegion {
    TextureSubresourceLayers srcSubresource{ 0, 0, 1 }; ///< 源纹理子资源
    Offset3D srcOffset{ 0, 0, 0 };        ///< 源纹理偏移
    TextureSubresourceLayers dstSubresource{ 0, 0, 1 }; ///< 目标纹理子资源
    Offset3D dstOffset{ 0, 0, 0 };        ///< 目标纹理偏移
    Extent3D extent{ 0, 0, 0 };        ///< 复制范围
};

/**
 * @brief 纹理Blit区域
 */
struct TextureBlitRegion {
    TextureSubresourceLayers srcSubresource{ 0, 0, 1 }; ///< 源纹理子资源
    Offset3D srcOffsets[2];  ///< 源区域 [min, max]
    TextureSubresourceLayers dstSubresource{ 0, 0, 1 }; ///< 目标纹理子资源
    Offset3D dstOffsets[2];  ///< 目标区域 [min, max]
};

/**
 * @brief 顶点输入属性描述符
 */
struct VertexInputAttribute {
    uint32_t location{ 0 };       ///< 着色器中的位置
    uint32_t binding{ 0 };        ///< 绑定点
    DataFormat format{ DataFormat::Unknown };       ///< 数据格式
    uint32_t offset{ 0 };         ///< 在缓冲区中的字节偏移量
};

/**
 * @brief 顶点输入绑定描述符
 */
struct VertexInputBinding {
    uint32_t binding{ 0 };        ///< 绑定点
    uint32_t stride{ 0 };         ///< 顶点步长（字节）
    bool perVertex{ true };          ///< true=每个顶点，false=每个实例
};

/**
 * @brief 缓冲区描述符
 */
struct BufferDesc {
    uint64_t size{ 0 };           ///< 缓冲区大小（字节）
    BufferType type{ BufferType::Unknown };         ///< 缓冲区类型
    GPUMemoryUsage usage{ GPUMemoryUsage::Unknown };    ///< 内存使用模式
    GPUMemoryUsage memoryUsage{ GPUMemoryUsage::Unknown }; ///< 内存使用方式（兼容字段）
    uint32_t bindFlags{ 0 };      ///< 绑定标志位
    
    // 扩展字段用于具体缓冲区类型
    union {
        struct {
            uint32_t vertexCount{ 0 };     ///< 顶点数量
            uint32_t vertexStride{ 0 };    ///< 顶点步长
        } vertex;
        
        struct {
            uint32_t indexCount{ 0 };      ///< 索引数量
            DataFormat format{ DataFormat::Unknown };       ///< 索引格式
        } index;
        
        struct {
            uint32_t elementCount{ 0 };    ///< 元素数量
            uint32_t elementStride{ 0 };  ///< 元素步长
        } structured;
    };
    
    std::string name{};         ///< 缓冲区名称（调试用）
};

/**
 * @brief 采样数量枚举
 * @details 定义多重采样支持的采样数量
 */
enum class SampleCount : uint8_t {
    Unknown = 0,    ///< 未知采样数量
    Samples1 = 1,   ///< 1个采样点
    Samples2 = 2,   ///< 2个采样点
    Samples4 = 4,   ///< 4个采样点
    Samples8 = 8,   ///< 8个采样点
    Samples16 = 16  ///< 16个采样点
};

/**
 * @brief 纹理描述符
 */
struct TextureDesc {
    math::u32v3 size{ 0, 0, 0 };        ///< 纹理尺寸 (width, height, depth)
    uint32_t mipLevels{ 1 };      ///< Mip层级数
    uint32_t arraySize{ 1 };      ///< 数组大小
    DataFormat format{ DataFormat::Unknown };       ///< 数据格式
    TextureType type{ TextureType::Unknown };        ///< 纹理类型
    TextureUsage usage{ TextureUsage::Unknown };      ///< 纹理用途
    GPUMemoryUsage memoryUsage{ GPUMemoryUsage::Unknown }; ///< 内存使用方式
    std::string name{};        ///< 纹理名称
};

/**
 * @brief 纹理视图描述符
 */
struct TextureViewDesc {
    ResourceHandle texture{ handles::INVALID_RESOURCE };           ///< 原始纹理句柄
    TextureType viewType{ TextureType::Unknown };             ///< 视图类型
    DataFormat format{ DataFormat::Unknown };                ///< 数据格式
    uint32_t mostDetailedMip{ 0 };         ///< 起始Mip层级
    uint32_t mipCount{ 1 };                ///< Mip层级数量
    uint32_t firstArraySlice{ 0 };         ///< 起始数组层
    uint32_t arraySize{ 1 };               ///< 数组层数量
};

/**
 * @brief 采样器描述符
 */
struct SwapChainDesc {
    platform::window_handle window{ nullptr };  ///< 窗口句柄
    uint32_t width{ 0 };                  ///< 宽度
    uint32_t height{ 0 };                 ///< 高度
    DataFormat format{ DataFormat::BGRA8_UNorm };               ///< 颜色格式
    uint32_t bufferCount{ 3 };            ///< 缓冲区数量
    PresentMode presentMode{ PresentMode::FIFO };         ///< 呈现模式
    bool enableVsync{ true };                ///< 是否开启垂直同步（辅助字段，优先使用presentMode）
};

/**
 * @brief 采样器描述符
 */
struct SamplerDesc {
    FilterMode minFilter{ FilterMode::Linear };        ///< 缩小过滤模式
    FilterMode magFilter{ FilterMode::Linear };        ///< 放大过滤模式
    FilterMode mipFilter{ FilterMode::Linear };        ///< Mipmap过滤模式
    TextureAddressMode addressU{ TextureAddressMode::Wrap }; ///< U轴寻址模式
    TextureAddressMode addressV{ TextureAddressMode::Wrap }; ///< V轴寻址模式
    TextureAddressMode addressW{ TextureAddressMode::Wrap }; ///< W轴寻址模式
    float mipLodBias{ 0.0f };            ///< Mipmap LOD偏差
    uint32_t maxAnisotropy{ 1 };      ///< 最大各向异性
    ComparisonFunc comparisonFunc{ ComparisonFunc::Always }; ///< 比较函数
    math::v4 borderColor{ 0.0f, 0.0f, 0.0f, 0.0f };        ///< 边框颜色
    float minLod{ 0.0f };                ///< 最小LOD
    float maxLod{ 1000.0f };                ///< 最大LOD
};

/**
 * @brief 计算管线描述符
 */
struct ComputePipelineDesc {
    ShaderHandle computeShader{ handles::INVALID_SHADER };         ///< 计算着色器
    PipelineLayoutHandle layout{ handles::INVALID_PIPELINE_LAYOUT };        ///< 管线布局
    math::u32v3 threadGroupSize{ 1, 1, 1 };        ///< 线程组大小 (x, y, z)
};

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

struct PushConstantRange {
    ShaderStage stageFlags{ ShaderStage::Unknown };
    uint32_t offset{ 0 };
    uint32_t size{ 0 };
};

struct PipelineLayoutDesc {
    uint32_t setLayoutCount{ 0 };
    const DescriptorSetLayoutHandle* setLayouts{ nullptr };
    uint32_t pushConstantRangeCount{ 0 };
    const PushConstantRange* pushConstantRanges{ nullptr };
};

/**
 * @brief 渲染通道描述符
 * @details 定义渲染通道的配置参数
 */
struct RenderPassDesc {
    struct Attachment {
        ResourceHandle texture{ handles::INVALID_RESOURCE };           ///< 渲染目标纹理
        DataFormat format{ DataFormat::Unknown };                ///< 数据格式
        LoadAction loadOp{ LoadAction::DontCare };                ///< 加载操作
        StoreAction storeOp{ StoreAction::Store };              ///< 存储操作
        ClearValue clearValue;            ///< 清除值
        u32 sampleCount{ 1 };                  ///< 采样数量
        uint8_t mipLevel{ 0 };                 ///< Mip层级
        uint16_t arrayLayer{ 0 };              ///< 数组层级
    };
    
    utl::vector<Attachment> colorAttachments;   ///< 颜色附件
    Attachment depthAttachment{ handles::INVALID_RESOURCE };                 ///< 深度附件
    Attachment stencilAttachment{ handles::INVALID_RESOURCE };              ///< 模板附件
    
    ViewportDesc viewport{ {0.0f, 0.0f}, {0.0f, 0.0f}, 0.0f, 1.0f };                       ///< 视口
    Rect scissor{ {0, 0}, {0, 0} };                               ///< 裁剪矩形
    
    // Timestamp Queries
    QueryPoolHandle timestampQueryPool{handles::INVALID_QUERY_POOL};
    uint32_t beginTimestampIndex{0};
    uint32_t endTimestampIndex{0};
    bool enableTimestamp{false};

    // Multi-View / Layered Rendering
    uint32_t renderTargetArrayLength{1};
};

} // namespace primal::graphics::rhi