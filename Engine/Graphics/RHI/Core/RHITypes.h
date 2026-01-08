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
}

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
    DescriptorSet = 11  ///< 描述符集
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
    Destroyed = 7       ///< 已销毁
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
    Vertex = 1,     ///< 顶点着色器
    Pixel = 2,      ///< 像素着色器
    Geometry = 3,   ///< 几何着色器
    Hull = 4,       ///< 外壳着色器
    Domain = 5,     ///< 域着色器
    Compute = 6     ///< 计算着色器
};

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
    uint32_t binding;
    DescriptorType descriptorType;
    uint32_t descriptorCount;
    ShaderStage stageFlags;
    const SamplerHandle* immutableSamplers;
    
    DescriptorSetLayoutBinding() : binding(0), descriptorType(DescriptorType::Unknown), 
                                  descriptorCount(0), stageFlags(ShaderStage::Unknown), 
                                  immutableSamplers(nullptr) {}
};

/**
 * @brief 描述符集布局描述符
 */
struct DescriptorSetLayoutDesc {
    uint32_t bindingCount;
    const DescriptorSetLayoutBinding* bindings;
    
    DescriptorSetLayoutDesc() : bindingCount(0), bindings(nullptr) {}
};

/**
 * @brief 描述符集描述符
 */
struct DescriptorSetDesc {
    DescriptorSetLayoutHandle layout;
    
    DescriptorSetDesc() : layout(0) {}
};

/**
 * @brief 描述符图像信息
 */
struct DescriptorImageInfo {
    SamplerHandle sampler;
    ResourceHandle imageView; // 纹理句柄
    ResourceState imageLayout;
    
    DescriptorImageInfo() : sampler(handles::INVALID_SAMPLER), 
                           imageView(handles::INVALID_RESOURCE), 
                           imageLayout(ResourceState::Unknown) {}
};

/**
 * @brief 描述符缓冲区信息
 */
struct DescriptorBufferInfo {
    ResourceHandle buffer;
    uint64_t offset;
    uint64_t range;
    
    DescriptorBufferInfo() : buffer(handles::INVALID_RESOURCE), offset(0), range(0) {}
};

/**
 * @brief 写描述符集
 */
struct WriteDescriptorSet {
    DescriptorSetHandle dstSet;
    uint32_t dstBinding;
    uint32_t dstArrayElement;
    uint32_t descriptorCount;
    DescriptorType descriptorType;
    const DescriptorImageInfo* imageInfo;
    const DescriptorBufferInfo* bufferInfo;
    
    WriteDescriptorSet() : dstSet(handles::INVALID_RESOURCE), dstBinding(0), 
                          dstArrayElement(0), descriptorCount(0), 
                          descriptorType(DescriptorType::Unknown), 
                          imageInfo(nullptr), bufferInfo(nullptr) {}
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
    Anisotropic = 3     ///< 各向异性过滤
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



// === 基础结构体定义 ===

/**
 * @brief 视口描述符
 */
struct ViewportDesc {
    math::v2 topLeft;        ///< 视口左上角坐标 (x, y)
    math::v2 size;           ///< 视口大小 (width, height)
    float minDepth;          ///< 最小深度值
    float maxDepth;          ///< 最大深度值
    
    ViewportDesc() : topLeft{0.0f, 0.0f}, size{1.0f, 1.0f}, minDepth(0.0f), maxDepth(1.0f) {}
    ViewportDesc(float x, float y, float width, float height, float minD = 0.0f, float maxD = 1.0f)
        : topLeft{x, y}, size{width, height}, minDepth(minD), maxDepth(maxD) {}
};

/**
 * @brief 裁剪矩形描述符
 */
struct Rect {
    math::s32v2 offset;      ///< 裁剪矩形偏移量 (x, y)
    math::u32v2 extent;      ///< 裁剪矩形大小 (width, height)
    
    Rect() : offset{0, 0}, extent{0, 0} {}
    Rect(int32_t x, int32_t y, uint32_t width, uint32_t height)
        : offset{x, y}, extent{width, height} {}
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
    
    ClearValue() : color{0.0f, 0.0f, 0.0f, 1.0f} {}
    ClearValue(float r, float g, float b, float a) : color{r, g, b, a} {}
    ClearValue(float d, uint32_t s) : depth(d), stencil(s) {}
};

/**
 * @brief 顶点输入属性描述符
 */
struct VertexInputAttribute {
    uint32_t location;       ///< 着色器中的位置
    uint32_t binding;        ///< 绑定点
    DataFormat format;       ///< 数据格式
    uint32_t offset;         ///< 在缓冲区中的字节偏移量
    
    VertexInputAttribute() : location(0), binding(0), format(DataFormat::Unknown), offset(0) {}
    VertexInputAttribute(uint32_t loc, uint32_t bind, DataFormat fmt, uint32_t off)
        : location(loc), binding(bind), format(fmt), offset(off) {}
};

/**
 * @brief 顶点输入绑定描述符
 */
struct VertexInputBinding {
    uint32_t binding;        ///< 绑定点
    uint32_t stride;         ///< 顶点步长（字节）
    bool perVertex;          ///< true=每个顶点，false=每个实例
    
    VertexInputBinding() : binding(0), stride(0), perVertex(true) {}
    VertexInputBinding(uint32_t bind, uint32_t str, bool perVert = true)
        : binding(bind), stride(str), perVertex(perVert) {}
};

/**
 * @brief 缓冲区描述符
 */
struct BufferDesc {
    uint64_t size;           ///< 缓冲区大小（字节）
    BufferType type;         ///< 缓冲区类型
    GPUMemoryUsage usage;    ///< 内存使用模式
    GPUMemoryUsage memoryUsage; ///< 内存使用方式（兼容字段）
    uint32_t bindFlags;      ///< 绑定标志位
    
    // 扩展字段用于具体缓冲区类型
    union {
        struct {
            uint32_t vertexCount;     ///< 顶点数量
            uint32_t vertexStride;    ///< 顶点步长
        } vertex;
        
        struct {
            uint32_t indexCount;      ///< 索引数量
            DataFormat format;       ///< 索引格式
        } index;
        
        struct {
            uint32_t elementCount;    ///< 元素数量
            uint32_t elementStride;  ///< 元素步长
        } structured;
    };
    
    std::string name;         ///< 缓冲区名称（调试用）
    
    BufferDesc() : size(0), type(BufferType::Unknown), usage(GPUMemoryUsage::Unknown), 
                   memoryUsage(GPUMemoryUsage::Unknown), bindFlags(0) {
        vertex.vertexCount = 0;
        vertex.vertexStride = 0;
        index.indexCount = 0;
        index.format = DataFormat::Unknown;
        structured.elementCount = 0;
        structured.elementStride = 0;
    }
    
    BufferDesc(uint64_t sz, BufferType tp, GPUMemoryUsage us, uint32_t flags = 0)
        : size(sz), type(tp), usage(us), memoryUsage(us), bindFlags(flags) {
        vertex.vertexCount = 0;
        vertex.vertexStride = 0;
        index.indexCount = 0;
        index.format = DataFormat::Unknown;
        structured.elementCount = 0;
        structured.elementStride = 0;
    }
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
    math::u32v3 size;        ///< 纹理尺寸 (width, height, depth)
    uint32_t mipLevels;      ///< Mip层级数
    uint32_t arraySize;      ///< 数组大小
    DataFormat format;       ///< 数据格式
    TextureType type;        ///< 纹理类型
    TextureUsage usage;      ///< 纹理用途
    GPUMemoryUsage memoryUsage; ///< 内存使用方式
    std::string name;        ///< 纹理名称
    
    TextureDesc() : size{0, 0, 0}, mipLevels(1), arraySize(1), 
                   format(DataFormat::Unknown), type(TextureType::Unknown), 
                   memoryUsage(GPUMemoryUsage::Unknown) {}
    TextureDesc(uint32_t width, uint32_t height, uint32_t depth, 
                uint32_t mips, uint32_t array, DataFormat fmt, TextureType tp)
        : size{width, height, depth}, mipLevels(mips), arraySize(array), 
          format(fmt), type(tp), memoryUsage(GPUMemoryUsage::Unknown) {}
};

/**
 * @brief 交换链描述符
 */
struct SwapChainDesc {
    platform::window_handle window;  ///< 窗口句柄
    uint32_t width;                  ///< 宽度
    uint32_t height;                 ///< 高度
    DataFormat format;               ///< 颜色格式
    uint32_t bufferCount;            ///< 缓冲区数量
    PresentMode presentMode;         ///< 呈现模式
    bool enableVsync;                ///< 是否开启垂直同步（辅助字段，优先使用presentMode）
    
    SwapChainDesc() : window(nullptr), width(0), height(0), 
                     format(DataFormat::BGRA8_UNorm), bufferCount(3), 
                     presentMode(PresentMode::FIFO), enableVsync(true) {}
};

/**
 * @brief 采样器描述符
 */
struct SamplerDesc {
    FilterMode minFilter;        ///< 缩小过滤模式
    FilterMode magFilter;        ///< 放大过滤模式
    FilterMode mipFilter;        ///< Mipmap过滤模式
    TextureAddressMode addressU; ///< U轴寻址模式
    TextureAddressMode addressV; ///< V轴寻址模式
    TextureAddressMode addressW; ///< W轴寻址模式
    float mipLodBias;            ///< Mipmap LOD偏差
    uint32_t maxAnisotropy;      ///< 最大各向异性
    ComparisonFunc comparisonFunc; ///< 比较函数
    math::v4 borderColor;        ///< 边框颜色
    float minLod;                ///< 最小LOD
    float maxLod;                ///< 最大LOD

    SamplerDesc() : minFilter(FilterMode::Linear), magFilter(FilterMode::Linear), 
                   mipFilter(FilterMode::Linear), addressU(TextureAddressMode::Wrap), 
                   addressV(TextureAddressMode::Wrap), addressW(TextureAddressMode::Wrap), 
                   mipLodBias(0.0f), maxAnisotropy(1), comparisonFunc(ComparisonFunc::Always), 
                   borderColor{0.0f, 0.0f, 0.0f, 0.0f}, minLod(0.0f), maxLod(1000.0f) {}
};

/**
 * @brief 计算管线描述符
 */
struct ComputePipelineDesc {
    ShaderHandle computeShader;         ///< 计算着色器
    math::u32v3 threadGroupSize;        ///< 线程组大小 (x, y, z)
    
    ComputePipelineDesc() : computeShader(handles::INVALID_SHADER), threadGroupSize{1, 1, 1} {}
};

} // namespace primal::graphics::rhi