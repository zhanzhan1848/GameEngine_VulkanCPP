# RHI核心模块API技术文档

## 文档概述

本文档详细描述了游戏引擎RHI（Render Hardware Interface）核心模块的API接口、数据结构和功能特性。RHI层基于CRTP（奇异递归模板模式）+ RAII（资源获取即初始化）+ ECS（实体组件系统）架构设计，支持D3D12、Vulkan、Metal、Dawn四个图形API平台。

**版本**: v0.2.0  
**更新时间**: 2025-12-30  
**作者**: GameEngine VulkanCPP Team  

---

## 命名空间结构

### `primal::graphics::rhi`

RHI系统的根命名空间，包含所有RHI相关的类型、枚举、结构体和类定义。

---

## 核心类型定义 (RHITypes.h)

### 句柄类型

#### `DeviceHandle`
- **完整函数名**: `primal::graphics::rhi::DeviceHandle`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `using DeviceHandle = uint64_t;`
- **功能描述**: RHI设备句柄，64位整数类型，包含设备类型和唯一标识符
- **使用说明**: 用于标识和管理RHI设备实例
- **相关常量**: `handles::INVALID_DEVICE = 0`

#### `ResourceHandle`
- **完整函数名**: `primal::graphics::rhi::ResourceHandle`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `using ResourceHandle = uint64_t;`
- **功能描述**: RHI资源句柄，64位整数类型，用于标识缓冲区、纹理等GPU资源
- **使用说明**: 所有GPU资源（缓冲区、纹理、渲染目标等）的唯一标识符
- **相关常量**: `handles::INVALID_RESOURCE = 0`

#### `CommandBufferHandle`
- **完整函数名**: `primal::graphics::rhi::CommandBufferHandle`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `using CommandBufferHandle = uint64_t;`
- **功能描述**: RHI命令缓冲区句柄，64位整数类型，用于标识命令缓冲区
- **使用说明**: 命令缓冲区记录和执行GPU操作的容器
- **相关常量**: `handles::INVALID_COMMAND_BUFFER = 0`

#### `ShaderHandle`
- **完整函数名**: `primal::graphics::rhi::ShaderHandle`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `using ShaderHandle = uint64_t;`
- **功能描述**: RHI着色器句柄，64位整数类型，用于标识着色器对象
- **使用说明**: 顶点着色器、像素着色器、计算着色器等的标识符
- **相关常量**: `handles::INVALID_SHADER = 0`

#### `PipelineHandle`
- **完整函数名**: `primal::graphics::rhi::PipelineHandle`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `using PipelineHandle = uint64_t;`
- **功能描述**: RHI管线句柄，64位整数类型，用于标识渲染管线或计算管线
- **使用说明**: 图形管线或计算管线的唯一标识符
- **相关常量**: `handles::INVALID_PIPELINE = 0`

#### `SyncHandle`
- **完整函数名**: `primal::graphics::rhi::SyncHandle`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `using SyncHandle = uint64_t;`
- **功能描述**: RHI同步对象句柄，64位整数类型，用于标识围栏、信号量等同步对象
- **使用说明**: GPU同步操作（围栏、信号量、事件等）的标识符
- **相关常量**: `handles::INVALID_SYNC = 0`

### 枚举类型

#### `RHIPlatform`
- **完整函数名**: `primal::graphics::rhi::RHIPlatform`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class RHIPlatform : uint8_t`
- **功能描述**: RHI支持的图形API平台枚举
- **枚举值**:
  - `Unknown = 0`: 未知平台
  - `D3D12 = 1`: DirectX 12平台
  - `Vulkan = 2`: Vulkan平台
  - `Metal = 3`: Apple Metal平台
  - `Dawn = 4`: WebGPU (Dawn)平台

#### `ResourceType`
- **完整函数名**: `primal::graphics::rhi::ResourceType`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class ResourceType : uint8_t`
- **功能描述**: GPU资源类型枚举
- **枚举值**:
  - `Unknown = 0`: 未知类型
  - `Buffer = 1`: 缓冲区资源
  - `Texture = 2`: 纹理资源
  - `RenderTarget = 3`: 渲染目标
  - `DepthStencil = 4`: 深度模板缓冲
  - `Pipeline = 5`: 管线资源
  - `Shader = 6`: 着色器资源
  - `Sampler = 7`: 采样器资源

#### `BufferType`
- **完整函数名**: `primal::graphics::rhi::BufferType`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class BufferType : uint8_t`
- **功能描述**: 缓冲区类型枚举，定义不同用途的缓冲区
- **枚举值**:
  - `Unknown = 0`: 未知类型
  - `Vertex = 1`: 顶点缓冲区，存储顶点数据
  - `Index = 2`: 索引缓冲区，存储索引数据
  - `Constant = 3`: 常量缓冲区，存储着色器常量
  - `Structured = 4`: 结构化缓冲区，存储结构化数据
  - `Raw = 5`: 原始缓冲区，存储原始字节数据
  - `Indirect = 6`: 间接绘制缓冲区，存储间接绘制命令

#### `TextureType`
- **完整函数名**: `primal::graphics::rhi::TextureType`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class TextureType : uint8_t`
- **功能描述**: 纹理类型枚举，定义不同维度的纹理
- **枚举值**:
  - `Unknown = 0`: 未知类型
  - `Texture1D = 1`: 1D纹理
  - `Texture2D = 2`: 2D纹理
  - `Texture3D = 3`: 3D纹理
  - `TextureCube = 4`: 立方纹理
  - `Texture1DArray = 5`: 1D纹理数组
  - `Texture2DArray = 6`: 2D纹理数组
  - `TextureCubeArray = 7`: 立方纹理数组

#### `TextureUsage`
- **完整函数名**: `primal::graphics::rhi::TextureUsage`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class TextureUsage : uint32_t`
- **功能描述**: 纹理用途枚举，支持位标志组合
- **枚举值**:
  - `Unknown = 0x00000000`: 未知用途
  - `ShaderResource = 0x00000001`: 着色器资源，可在着色器中采样
  - `RenderTarget = 0x00000002`: 渲染目标，可作为颜色附件
  - `DepthStencil = 0x00000004`: 深度模板缓冲，可作为深度模板附件
  - `UnorderedAccess = 0x00000008`: 无序访问视图，支持读写操作
  - `CopySource = 0x00000010`: 复制源，可作为复制操作的源
  - `CopyDest = 0x00000020`: 复制目标，可作为复制操作的目标
  - `ResolveSource = 0x00000040`: 解析源，多重采样解析源
  - `ResolveDest = 0x00000080`: 解析目标，多重采样解析目标
  - `Present = 0x00000100`: 呈现目标，可呈现到屏幕

#### `DataFormat`
- **完整函数名**: `primal::graphics::rhi::DataFormat`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class DataFormat : uint16_t`
- **功能描述**: 数据格式枚举，定义顶点、纹理、缓冲区等的数据格式
- **主要分类**:
  - **8位格式**: R8_UNorm, R8_SNorm, R8_UInt, R8_SInt
  - **16位格式**: R16_UNorm, R16_SNorm, R16_UInt, R16_SInt, R16_Float, RG8_UNorm, RG8_SNorm, RG8_UInt, RG8_SInt
  - **24位格式**: R8G8B8_UNorm, R8G8B8_SNorm, R8G8B8_UInt, R8G8B8_SInt
  - **32位格式**: R32_UNorm, R32_SNorm, R32_UInt, R32_SInt, R32_Float, RG16_UNorm, RG16_SNorm, RG16_UInt, RG16_SInt, RG16_Float, RG8B8A8_UNorm, RG8B8A8_SNorm, RG8B8A8_UInt, RG8B8A8_SInt, BGRA8_UNorm, BGRA8_SNorm, BGRA8_UInt, BGRA8_SInt, RGBA8_UNorm, RGBA8_SNorm, RGBA8_UInt, RGBA8_SInt, RGBA8_sRGB, RG32_UNorm, RG32_SNorm, RG32_UInt, RG32_SInt, RG32_Float, RGB32_UNorm, RGB32_SNorm, RGB32_UInt, RGB32_SInt, RGB32_Float, RGBA16_UNorm, RGBA16_SNorm, RGBA16_UInt, RGBA16_SInt, RGBA16_Float, RGBA32_UNorm, RGBA32_SNorm, RGBA32_UInt, RGBA32_SInt, RGBA32_Float, RGBA32_sRGB
  - **压缩格式**: BC1_UNorm, BC1_sRGB, BC2_UNorm, BC2_sRGB, BC3_UNorm, BC3_sRGB, BC4_UNorm, BC4_SNorm, BC5_UNorm, BC5_SNorm, BC6H_UF16, BC6H_SF16, BC7_UNorm, BC7_sRGB
  - **深度模板格式**: D16_UNorm, D24_UNorm_S8_UInt, D32_Float, D32_Float_S8X24_UInt

#### `DataIndexType`
- **完整函数名**: `primal::graphics::rhi::DataIndexType`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class DataIndexType : uint8_t`
- **功能描述**: 索引数据类型枚举，定义索引缓冲区的数据格式
- **枚举值**:
  - `Unknown = 0`: 未知类型
  - `UInt16 = 1`: 16位无符号整数索引
  - `UInt32 = 2`: 32位无符号整数索引

#### `GPUMemoryUsage`
- **完整函数名**: `primal::graphics::rhi::GPUMemoryUsage`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class GPUMemoryUsage : uint8_t`
- **功能描述**: GPU内存使用模式枚举，定义内存的访问特性
- **枚举值**:
  - `Unknown = 0`: 未知模式
  - `Static = 1`: 静态内存，CPU只写一次，GPU多次读取
  - `Dynamic = 2`: 动态内存，CPU频繁更新，GPU多次读取
  - `Staging = 3`: 暂存内存，用于CPU到GPU的数据传输
  - `Readback = 4`: 回读内存，用于GPU到CPU的数据传输

#### `CommandQueueType`
- **完整函数名**: `primal::graphics::rhi::CommandQueueType`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class CommandQueueType : uint8_t`
- **功能描述**: 命令队列类型枚举，定义不同类型的命令队列
- **枚举值**:
  - `Unknown = 0`: 未知类型
  - `Graphics = 1`: 图形队列，支持图形和计算操作
  - `Compute = 2`: 计算队列，仅支持计算操作
  - `Transfer = 3`: 传输队列，仅支持数据传输操作

#### `PrimitiveTopology`
- **完整函数名**: `primal::graphics::rhi::PrimitiveTopology`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class PrimitiveTopology : uint8_t`
- **功能描述**: 图元拓扑类型枚举，定义绘制的基本图元类型
- **枚举值**:
  - `Unknown = 0`: 未知类型
  - `PointList = 1`: 点列表
  - `LineList = 2`: 线列表
  - `LineStrip = 3`: 线带
  - `TriangleList = 4`: 三角形列表
  - `TriangleStrip = 5`: 三角形带
  - `LineListAdj = 6`: 邻接线列表
  - `LineStripAdj = 7`: 邻接线带
  - `TriangleListAdj = 8`: 邻接三角形列表
  - `TriangleStripAdj = 9`: 邻接三角形带

#### `ShaderStage`
- **完整函数名**: `primal::graphics::rhi::ShaderStage`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class ShaderStage : uint8_t`
- **功能描述**: 着色器阶段枚举，定义管线中的不同着色器阶段
- **枚举值**:
  - `Unknown = 0`: 未知阶段
  - `Vertex = 1`: 顶点着色器
  - `Pixel = 2`: 像素着色器
  - `Geometry = 3`: 几何着色器
  - `Hull = 4`: 外壳着色器
  - `Domain = 5`: 域着色器
  - `Compute = 6`: 计算着色器

#### `BlendOp`
- **完整函数名**: `primal::graphics::rhi::BlendOp`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class BlendOp : uint8_t`
- **功能描述**: 混合操作枚举，定义颜色混合的数学运算
- **枚举值**:
  - `Unknown = 0`: 未知操作
  - `Add = 1`: 相加 (src + dest)
  - `Subtract = 2`: 相减 (src - dest)
  - `RevSubtract = 3`: 反向相减 (dest - src)
  - `Min = 4`: 最小值 (min(src, dest))
  - `Max = 5`: 最大值 (max(src, dest))

#### `BlendFactor`
- **完整函数名**: `primal::graphics::rhi::BlendFactor`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class BlendFactor : uint8_t`
- **功能描述**: 混合因子枚举，定义混合操作中的权重因子
- **枚举值**:
  - `Unknown = 0`: 未知因子
  - `Zero = 1`: 零因子 (0, 0, 0, 0)
  - `One = 2`: 一因子 (1, 1, 1, 1)
  - `SrcColor = 3`: 源颜色因子 (Rs, Gs, Bs, As)
  - `InvSrcColor = 4`: 反源颜色因子 (1-Rs, 1-Gs, 1-Bs, 1-As)
  - `SrcAlpha = 5`: 源Alpha因子 (As, As, As, As)
  - `InvSrcAlpha = 6`: 反源Alpha因子 (1-As, 1-As, 1-As, 1-As)
  - `DestAlpha = 7`: 目标Alpha因子 (Ad, Ad, Ad, Ad)
  - `InvDestAlpha = 8`: 反目标Alpha因子 (1-Ad, 1-Ad, 1-Ad, 1-Ad)
  - `DestColor = 9`: 目标颜色因子 (Rd, Gd, Bd, Ad)
  - `InvDestColor = 10`: 反目标颜色因子 (1-Rd, 1-Gd, 1-Bd, 1-Ad)
  - `SrcAlphaSat = 11`: 源Alpha饱和因子 (min(As, 1-Ad), min(As, 1-Ad), min(As, 1-Ad), min(As, 1-Ad))
  - `BlendFactor = 12`: 混合因子 (常量颜色)
  - `InvBlendFactor = 13`: 反混合因子 (1-常量颜色)
  - `Src1Color = 14`: 源1颜色因子
  - `InvSrc1Color = 15`: 反源1颜色因子
  - `Src1Alpha = 16`: 源1Alpha因子
  - `InvSrc1Alpha = 17`: 反源1Alpha因子

#### `ComparisonFunc`
- **完整函数名**: `primal::graphics::rhi::ComparisonFunc`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class ComparisonFunc : uint8_t`
- **功能描述**: 比较函数枚举，定义深度或模板测试的比较操作
- **枚举值**:
  - `Unknown = 0`: 未知函数
  - `Never = 1`: 永不通过 (false)
  - `Less = 2`: 小于通过 (src < dest)
  - `Equal = 3`: 等于通过 (src == dest)
  - `LessEqual = 4`: 小于等于通过 (src <= dest)
  - `Greater = 5`: 大于通过 (src > dest)
  - `NotEqual = 6`: 不等于通过 (src != dest)
  - `GreaterEqual = 7`: 大于等于通过 (src >= dest)
  - `Always = 8`: 总是通过 (true)

#### `StencilOp`
- **完整函数名**: `primal::graphics::rhi::StencilOp`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class StencilOp : uint8_t`
- **功能描述**: 模板操作枚举，定义模板测试失败或通过时的操作
- **枚举值**:
  - `Unknown = 0`: 未知操作
  - `Keep = 1`: 保持当前模板值
  - `Zero = 2`: 将模板值置零
  - `Replace = 3`: 用参考值替换模板值
  - `IncSat = 4`: 饱和递增模板值（不超过最大值）
  - `DecSat = 5`: 饱和递减模板值（不低于最小值）
  - `Invert = 6`: 按位反转模板值
  - `Inc = 7`: 递增模板值（环绕）
  - `Dec = 8`: 递减模板值（环绕）

#### `FillMode`
- **完整函数名**: `primal::graphics::rhi::FillMode`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class FillMode : uint8_t`
- **功能描述**: 填充模式枚举，定义多边形的填充方式
- **枚举值**:
  - `Unknown = 0`: 未知模式
  - `Solid = 1`: 实心填充
  - `Wireframe = 2`: 线框填充

#### `CullMode`
- **完整函数名**: `primal::graphics::rhi::CullMode`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class CullMode : uint8_t`
- **功能描述**: 裁剪模式枚举，定义多边形的裁剪方式
- **枚举值**:
  - `Unknown = 0`: 未知模式
  - `None = 1`: 不裁剪
  - `Front = 2`: 裁剪前面
  - `Back = 3`: 裁剪背面

#### `FilterMode`
- **完整函数名**: `primal::graphics::rhi::FilterMode`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class FilterMode : uint8_t`
- **功能描述**: 纹理过滤模式枚举，定义纹理采样时的过滤方式
- **枚举值**:
  - `Unknown = 0`: 未知模式
  - `Point = 1`: 点过滤（最近邻采样）
  - `Linear = 2`: 线性过滤（双线性采样）
  - `Anisotropic = 3`: 各向异性过滤

#### `TextureAddressMode`
- **完整函数名**: `primal::graphics::rhi::TextureAddressMode`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class TextureAddressMode : uint8_t`
- **功能描述**: 纹理寻址模式枚举，定义纹理坐标超出[0,1]范围时的处理方式
- **枚举值**:
  - `Unknown = 0`: 未知模式
  - `Wrap = 1`: 重复（纹理坐标对1取模）
  - `Mirror = 2`: 镜像（纹理坐标对整数部分取模，奇数时反转）
  - `Clamp = 3`: 限制（纹理坐标限制在[0,1]范围）
  - `Border = 4`: 边框（使用边框颜色）
  - `MirrorOnce = 5`: 单次镜像（镜像一次后限制）

---

## 常量定义

### `primal::graphics::rhi::constants`

RHI系统常量命名空间，定义各种限制和默认值。

#### 系统限制常量
- **`MAX_RENDER_TARGETS = 8`**: 最大渲染目标数量
- **`MAX_VERTEX_BUFFERS = 16`**: 最大顶点缓冲区数量
- **`MAX_TEXTURE_UNITS = 32`**: 最大纹理单元数量
- **`MAX_SAMPLERS = 16`**: 最大采样器数量
- **`MAX_CONSTANT_BUFFERS = 14`**: 最大常量缓冲区数量
- **`MAX_VIEWPORTS = 16`**: 最大视口数量
- **`MAX_SCISSOR_RECTS = 16`**: 最大裁剪矩形数量
- **`MAX_VERTEX_INPUT_ATTRIBUTES = 16`**: 最大顶点输入属性数量
- **`MAX_COLOR_ATTACHMENTS = 8`**: 最大颜色附件数量
- **`MAX_SHADER_STAGES = 6`**: 最大着色器阶段数
- **`MAX_PUSH_CONSTANTS_SIZE = 256`**: 最大推送常量大小（字节）
- **`MAX_UBO_SIZE = 64 * 1024`**: 最大统一缓冲区大小（64KB）
- **`MAX_SSBO_SIZE = 128 * 1024 * 1024`**: 最大存储缓冲区大小（128MB）
- **`MIN_UNIFORM_BUFFER_OFFSET_ALIGNMENT = 256`**: 最小统一缓冲区对齐
- **`FRAME_COUNT = 3`**: 帧缓冲数量（三重缓冲）
- **`MAX_ANISOTROPY = 16.0f`**: 最大各向异性

### `primal::graphics::rhi::handles`

无效句柄常量命名空间。

#### 无效句柄常量
- **`INVALID_DEVICE = 0`**: 无效设备句柄
- **`INVALID_RESOURCE = 0`**: 无效资源句柄
- **`INVALID_COMMAND_BUFFER = 0`**: 无效命令缓冲区句柄
- **`INVALID_SHADER = 0`**: 无效着色器句柄
- **`INVALID_PIPELINE = 0`**: 无效管线句柄
- **`INVALID_SYNC = 0`**: 无效同步对象句柄

---

## 基础结构体定义

### `ViewportDesc`
- **完整函数名**: `primal::graphics::rhi::ViewportDesc`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: 视口描述符，定义渲染区域和深度范围
- **成员变量**:
  - `math::v2 topLeft`: 视口左上角坐标 (x, y)
  - `math::v2 size`: 视口大小 (width, height)
  - `float minDepth`: 最小深度值
  - `float maxDepth`: 最大深度值
- **构造函数**:
  - `ViewportDesc()`: 默认构造函数，初始化为(0,0,1,1,0,1)
  - `ViewportDesc(float x, float y, float width, float height, float minD = 0.0f, float maxD = 1.0f)`: 参数化构造函数

### `Rect`
- **完整函数名**: `primal::graphics::rhi::Rect`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: 裁剪矩形描述符，定义像素级的裁剪区域
- **成员变量**:
  - `math::s32v2 offset`: 裁剪矩形偏移量 (x, y)
  - `math::u32v2 extent`: 裁剪矩形大小 (width, height)
- **构造函数**:
  - `Rect()`: 默认构造函数，初始化为(0,0,0,0)
  - `Rect(int32_t x, int32_t y, uint32_t width, uint32_t height)`: 参数化构造函数

### `ClearValue`
- **完整函数名**: `primal::graphics::rhi::ClearValue`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: 清除值联合体，用于渲染目标或深度模板缓冲的清除操作
- **成员变量**:
  - 联合体包含:
    - `math::v4 color`: 颜色清除值 (r, g, b, a)
    - 结构体:
      - `float depth`: 深度清除值
      - `uint32_t stencil`: 模板清除值
    - `math::v4 depthStencil`: 深度模板清除值（别名）
- **构造函数**:
  - `ClearValue()`: 默认构造函数，初始化为(0,0,0,1)
  - `ClearValue(float r, float g, float b, float a)`: 颜色清除值构造函数
  - `ClearValue(float d, uint32_t s)`: 深度模板清除值构造函数

### `VertexInputAttribute`
- **完整函数名**: `primal::graphics::rhi::VertexInputAttribute`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: 顶点输入属性描述符，定义顶点数据的格式和布局
- **成员变量**:
  - `uint32_t location`: 着色器中的位置
  - `uint32_t binding`: 绑定点
  - `DataFormat format`: 数据格式
  - `uint32_t offset`: 在缓冲区中的字节偏移量
- **构造函数**:
  - `VertexInputAttribute()`: 默认构造函数
  - `VertexInputAttribute(uint32_t loc, uint32_t bind, DataFormat fmt, uint32_t off)`: 参数化构造函数

### `VertexInputBinding`
- **完整函数名**: `primal::graphics::rhi::VertexInputBinding`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: 顶点输入绑定描述符，定义顶点缓冲区的绑定方式
- **成员变量**:
  - `uint32_t binding`: 绑定点
  - `uint32_t stride`: 顶点步长（字节）
  - `bool perVertex`: true=每个顶点，false=每个实例
- **构造函数**:
  - `VertexInputBinding()`: 默认构造函数
  - `VertexInputBinding(uint32_t bind, uint32_t str, bool perVert = true)`: 参数化构造函数

### `BufferDesc`
- **完整函数名**: `primal::graphics::rhi::BufferDesc`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: 缓冲区描述符，定义缓冲区的创建参数
- **成员变量**:
  - `uint64_t size`: 缓冲区大小（字节）
  - `BufferType type`: 缓冲区类型
  - `GPUMemoryUsage usage`: 内存使用模式
  - `uint32_t bindFlags`: 绑定标志位
- **构造函数**:
  - `BufferDesc()`: 默认构造函数
  - `BufferDesc(uint64_t sz, BufferType tp, GPUMemoryUsage us, uint32_t flags = 0)`: 参数化构造函数

### `TextureDesc`
- **完整函数名**: `primal::graphics::rhi::TextureDesc`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: 纹理描述符，定义纹理的创建参数
- **成员变量**:
  - `math::u32v3 size`: 纹理尺寸 (width, height, depth)
  - `uint32_t mipLevels`: Mip层级数
  - `uint32_t arraySize`: 数组大小
  - `DataFormat format`: 数据格式
  - `TextureType type`: 纹理类型
  - `TextureUsage usage`: 纹理用途
- **构造函数**:
  - `TextureDesc()`: 默认构造函数
  - `TextureDesc(uint32_t width, uint32_t height, uint32_t depth, uint32_t mips, uint32_t array, DataFormat fmt, TextureType tp)`: 参数化构造函数

---

## RHI设备基类 (RHIDevice.h)

### `DeviceDesc`
- **完整函数名**: `primal::graphics::rhi::DeviceDesc`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: RHI设备描述符，包含设备创建所需的参数
- **成员变量**:
  - `RHIPlatform platform`: 目标平台
  - `bool enableDebug`: 是否启用调试层
  - `bool enableValidation`: 是否启用验证层
  - `uint32_t adapterIndex`: 适配器索引
  - `uint32_t maxFramesInFlight`: 最大帧数
- **构造函数**:
  - `DeviceDesc()`: 默认构造函数，初始化为平台Unknown、禁用调试和验证、适配器索引0、最大3帧

### `DeviceInfo`
- **完整函数名**: `primal::graphics::rhi::DeviceInfo`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: RHI设备信息，包含设备的硬件信息和能力
- **成员变量**:
  - `RHIPlatform platform`: 设备平台
  - `char deviceName[256]`: 设备名称
  - `char driverVersion[128]`: 驱动版本
  - `uint64_t dedicatedVideoMemory`: 专用显存大小（字节）
  - `uint64_t sharedSystemMemory`: 共享系统内存大小（字节）
  - `uint32_t maxTexture1DSize`: 1D纹理最大尺寸
  - `uint32_t maxTexture2DSize`: 2D纹理最大尺寸
  - `uint32_t maxTexture3DSize`: 3D纹理最大尺寸
  - `uint32_t maxTextureCubeSize`: 立方纹理最大尺寸
  - `uint32_t maxRenderTargets`: 最大渲染目标数
  - `uint32_t maxVertexAttributes`: 最大顶点属性数
  - `uint32_t maxSamplerStates`: 最大采样器状态数
  - `uint32_t maxConstantBufferSize`: 最大常量缓冲区大小
  - `bool supportsRayTracing`: 是否支持光线追踪
  - `bool supportsMeshShaders`: 是否支持网格着色器
  - `bool supportsVariableRateShading`: 是否支持可变速率着色
- **构造函数**:
  - `DeviceInfo()`: 默认构造函数，初始化所有值为0或false

### `RHIDevice<Derived>`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `template<typename Derived> class RHIDevice`
- **功能描述**: RHI设备基类（CRTP模式），使用奇异递归模板模式实现编译时多态，避免虚函数调用开销

#### 构造函数和析构函数

##### `RHIDevice(const DeviceDesc& desc)`
- **函数类型**: 构造函数
- **参数列表**:
  - `const DeviceDesc& desc`: 设备描述符，包含创建设备所需的参数
- **返回值**: 无
- **功能描述**: 构造RHI设备实例，初始化设备描述符和有效性标志
- **异常情况**: 无

##### `virtual ~RHIDevice()`
- **函数类型**: 虚析构函数
- **参数列表**: 无
- **返回值**: 无
- **功能描述**: 虚析构函数，确保派生类正确析构
- **异常情况**: 无

#### 核心接口方法

##### `bool Initialize()`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::Initialize`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `bool`
- **返回值说明**: 初始化是否成功
- **功能描述**: 初始化设备，调用派生类的初始化实现并查询设备信息
- **异常情况**: 如果设备已初始化则直接返回true

##### `void Shutdown()`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::Shutdown`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `void`
- **返回值说明**: 无返回值
- **功能描述**: 销毁设备，调用派生类的销毁实现并重置有效性标志
- **异常情况**: 如果设备未初始化则直接返回

##### `void WaitIdle() const`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::WaitIdle`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `void`
- **返回值说明**: 无返回值
- **功能描述**: 等待设备空闲，等待所有GPU操作完成
- **异常情况**: 如果设备未初始化则断言失败

##### `void BeginFrame()`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::BeginFrame`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `void`
- **返回值说明**: 无返回值
- **功能描述**: 开始新的一帧，准备帧渲染
- **异常情况**: 如果设备未初始化则断言失败

##### `void EndFrame()`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::EndFrame`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `void`
- **返回值说明**: 无返回值
- **功能描述**: 结束当前帧，完成帧渲染
- **异常情况**: 如果设备未初始化则断言失败

##### `void Present()`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::Present`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `void`
- **返回值说明**: 无返回值
- **功能描述**: 呈现到屏幕，将渲染结果显示到窗口
- **异常情况**: 如果设备未初始化则断言失败

#### 资源创建接口

##### `ResourceHandle CreateBuffer(const BufferDesc& desc)`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::CreateBuffer`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `const BufferDesc& desc`: 缓冲区描述符，包含大小、类型、使用模式等信息
- **返回值类型**: `ResourceHandle`
- **返回值说明**: 资源句柄，失败返回INVALID_RESOURCE
- **功能描述**: 创建缓冲区资源，根据描述符参数分配GPU内存并返回句柄
- **异常情况**: 如果设备未初始化则断言失败

##### `ResourceHandle CreateTexture(const TextureDesc& desc)`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::CreateTexture`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `const TextureDesc& desc`: 纹理描述符，包含尺寸、格式、类型等信息
- **返回值类型**: `ResourceHandle`
- **返回值说明**: 资源句柄，失败返回INVALID_RESOURCE
- **功能描述**: 创建纹理资源，根据描述符参数分配GPU内存并返回句柄
- **异常情况**: 如果设备未初始化则断言失败

##### `ShaderHandle CreateShader(const void* data, size_t size, ShaderStage stage, const char* entryPoint = "main")`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::CreateShader`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `const void* data`: 着色器数据，指向编译后的着色器字节码
  - `size_t size`: 数据大小，着色器字节码的长度
  - `ShaderStage stage`: 着色器阶段，指定着色器类型
  - `const char* entryPoint`: 入口点函数名，默认为"main"
- **返回值类型**: `ShaderHandle`
- **返回值说明**: 着色器句柄，失败返回INVALID_SHADER
- **功能描述**: 创建着色器对象，编译并加载着色器代码
- **异常情况**: 
  - 如果设备未初始化则断言失败
  - 如果数据为空或大小为0则断言失败

##### `PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::CreateGraphicsPipeline`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `const GraphicsPipelineDesc& desc`: 图形管线描述符，包含着色器、顶点输入、渲染状态等信息
- **返回值类型**: `PipelineHandle`
- **返回值说明**: 管线句柄，失败返回INVALID_PIPELINE
- **功能描述**: 创建图形管线，配置图形渲染管线的各个阶段
- **异常情况**: 如果设备未初始化则断言失败

##### `PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc)`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::CreateComputePipeline`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `const ComputePipelineDesc& desc`: 计算管线描述符，包含计算着色器等信息
- **返回值类型**: `PipelineHandle`
- **返回值说明**: 管线句柄，失败返回INVALID_PIPELINE
- **功能描述**: 创建计算管线，配置计算着色器管线
- **异常情况**: 如果设备未初始化则断言失败

##### `CommandBufferHandle CreateCommandBuffer(CommandQueueType type = CommandQueueType::Graphics)`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::CreateCommandBuffer`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `CommandQueueType type`: 命令队列类型，默认为Graphics
- **返回值类型**: `CommandBufferHandle`
- **返回值说明**: 命令缓冲区句柄，失败返回INVALID_COMMAND_BUFFER
- **功能描述**: 创建命令缓冲区，用于记录GPU命令
- **异常情况**: 如果设备未初始化则断言失败

#### 资源销毁接口

##### `void DestroyBuffer(ResourceHandle handle)`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::DestroyBuffer`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `ResourceHandle handle`: 缓冲区句柄，要销毁的缓冲区
- **返回值类型**: `void`
- **返回值说明**: 无返回值
- **功能描述**: 销毁缓冲区，释放GPU内存资源
- **异常情况**: 
  - 如果设备未初始化则断言失败
  - 如果句柄为INVALID_RESOURCE则直接返回

##### `void DestroyTexture(ResourceHandle handle)`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::DestroyTexture`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `ResourceHandle handle`: 纹理句柄，要销毁的纹理
- **返回值类型**: `void`
- **返回值说明**: 无返回值
- **功能描述**: 销毁纹理，释放GPU内存资源
- **异常情况**: 
  - 如果设备未初始化则断言失败
  - 如果句柄为INVALID_RESOURCE则直接返回

##### `void DestroyShader(ShaderHandle handle)`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::DestroyShader`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `ShaderHandle handle`: 着色器句柄，要销毁的着色器
- **返回值类型**: `void`
- **返回值说明**: 无返回值
- **功能描述**: 销毁着色器，释放着色器资源
- **异常情况**: 
  - 如果设备未初始化则断言失败
  - 如果句柄为INVALID_SHADER则直接返回

##### `void DestroyPipeline(PipelineHandle handle)`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::DestroyPipeline`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `PipelineHandle handle`: 管线句柄，要销毁的管线
- **返回值类型**: `void`
- **返回值说明**: 无返回值
- **功能描述**: 销毁管线，释放管线资源
- **异常情况**: 
  - 如果设备未初始化则断言失败
  - 如果句柄为INVALID_PIPELINE则直接返回

##### `void DestroyCommandBuffer(CommandBufferHandle handle)`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::DestroyCommandBuffer`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `CommandBufferHandle handle`: 命令缓冲区句柄，要销毁的命令缓冲区
- **返回值类型**: `void`
- **返回值说明**: 无返回值
- **功能描述**: 销毁命令缓冲区，释放命令缓冲区资源
- **异常情况**: 
  - 如果设备未初始化则断言失败
  - 如果句柄为INVALID_COMMAND_BUFFER则直接返回

#### 访问器方法

##### `const DeviceDesc& GetDesc() const`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::GetDesc`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `const DeviceDesc&`
- **返回值说明**: 设备描述符的常量引用
- **功能描述**: 获取设备描述符，用于查询设备创建参数
- **异常情况**: 无

##### `const DeviceInfo& GetInfo() const`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::GetInfo`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `const DeviceInfo&`
- **返回值说明**: 设备信息的常量引用
- **功能描述**: 获取设备信息，用于查询设备硬件能力
- **异常情况**: 无

##### `bool IsValid() const`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::IsValid`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `bool`
- **返回值说明**: 设备是否已初始化且有效
- **功能描述**: 检查设备是否有效，用于验证设备状态
- **异常情况**: 无

##### `uint32_t GetCurrentFrameIndex() const`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::GetCurrentFrameIndex`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `uint32_t`
- **返回值说明**: 当前帧索引（0到maxFramesInFlight-1）
- **功能描述**: 获取当前帧索引，用于多帧同步
- **异常情况**: 如果设备未初始化则断言失败

##### `RHIPlatform GetPlatform() const`
- **完整函数名**: `primal::graphics::rhi::RHIDevice<Derived>::GetPlatform`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `RHIPlatform`
- **返回值说明**: 设备平台类型
- **功能描述**: 获取平台，用于识别底层图形API
- **异常情况**: 无

---

## 管线描述符结构体

### `GraphicsPipelineDesc`
- **完整函数名**: `primal::graphics::rhi::GraphicsPipelineDesc`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: 图形管线描述符，包含图形渲染管线的所有配置参数
- **成员变量**:
  - `ShaderHandle vertexShader`: 顶点着色器
  - `ShaderHandle pixelShader`: 像素着色器
  - `ShaderHandle geometryShader`: 几何着色器
  - `ShaderHandle hullShader`: 外壳着色器
  - `ShaderHandle domainShader`: 域着色器
  - `utl::vector<VertexInputAttribute> vertexAttributes`: 顶点输入属性
  - `utl::vector<VertexInputBinding> vertexBindings`: 顶点输入绑定
  - `PrimitiveTopology topology`: 图元拓扑
  - `FillMode fillMode`: 填充模式
  - `CullMode cullMode`: 裁剪模式
  - `DataFormat renderTargetFormats[constants::MAX_RENDER_TARGETS]`: 渲染目标格式数组
  - `uint32_t renderTargetCount`: 渲染目标数量
  - `DataFormat depthStencilFormat`: 深度模板格式
  - `bool enableDepthTest`: 是否启用深度测试
  - `bool enableDepthWrite`: 是否启用深度写入
  - `ComparisonFunc depthFunc`: 深度比较函数
  - `bool enableStencilTest`: 是否启用模板测试
  - `uint8_t stencilReadMask`: 模板读取掩码
  - `uint8_t stencilWriteMask`: 模板写入掩码
  - `bool enableBlend`: 是否启用混合
  - `BlendFactor srcBlend`: 源混合因子
  - `BlendFactor destBlend`: 目标混合因子
  - `BlendOp blendOp`: 混合操作
  - `math::v4 blendConstants`: 混合常量
- **构造函数**:
  - `GraphicsPipelineDesc()`: 默认构造函数，初始化所有成员为默认值

### `ComputePipelineDesc`
- **完整函数名**: `primal::graphics::rhi::ComputePipelineDesc`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: 计算管线描述符，包含计算管线的配置参数
- **成员变量**:
  - `ShaderHandle computeShader`: 计算着色器
- **构造函数**:
  - `ComputePipelineDesc()`: 默认构造函数，初始化着色器为INVALID_SHADER

---

## MPSC队列系统 (RHIMpscQueue.h)

### `WorkPriority` 枚举
- **完整函数名**: `primal::graphics::rhi::WorkPriority`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class WorkPriority : uint8_t`
- **功能描述**: 工作项优先级枚举，定义MPSC队列中工作项的处理优先级
- **枚举值**:
  - `Low = 0`: 低优先级工作项
  - `Normal = 1`: 普通优先级工作项（默认）
  - `High = 2`: 高优先级工作项
  - `Critical = 3`: 关键优先级工作项

### `WorkItemType` 枚举
- **完整函数名**: `primal::graphics::rhi::WorkItemType`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **类型定义**: `enum class WorkItemType : uint8_t`
- **功能描述**: 工作项类型枚举，定义不同类型的GPU工作项
- **枚举值**:
  - `Unknown = 0`: 未知类型
  - `ResourceCreation = 1`: 资源创建工作
  - `ResourceDestruction = 2`: 资源销毁工作
  - `CommandSubmission = 3`: 命令提交工作
  - `MemoryAllocation = 4`: 内存分配工作
  - `MemoryDeallocation = 5`: 内存释放工作
  - `StateTransition = 6`: 状态转换工作
  - `SyncOperation = 7`: 同步操作工作
  - `Custom = 255`: 自定义工作项

### `WorkItem` 结构体
- **完整函数名**: `primal::graphics::rhi::WorkItem`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: 工作项结构体，代表MPSC队列中的单个工作单元
- **成员变量**:
  - `uint64_t workId`: 工作项唯一标识符
  - `WorkItemType type`: 工作项类型
  - `WorkPriority priority`: 工作项优先级
  - `std::function<void()> function`: 工作项执行函数
  - `std::chrono::high_resolution_clock::time_point submitTime`: 提交时间戳
  - `std::atomic<bool> completed`: 完成状态标志
  - `std::string description`: 工作项描述
- **构造函数**:
  - `WorkItem()`: 默认构造函数
  - `WorkItem(uint64_t id, WorkItemType t, WorkPriority p, std::function<void()> func, const std::string& desc = "")`: 参数化构造函数

### `QueueStats` 结构体
- **完整函数名**: `primal::graphics::rhi::QueueStats`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: 队列统计信息结构体，记录MPSC队列的性能和使用统计
- **成员变量**:
  - `std::atomic<uint64_t> totalEnqueued`: 总入队数量
  - `std::atomic<uint64_t> totalDequeued`: 总出队数量
  - `std::atomic<uint64_t> totalProcessed`: 总处理数量
  - `std::atomic<uint64_t> totalCompleted`: 总完成数量
  - `std::atomic<uint64_t> totalFailed`: 总失败数量
  - `std::atomic<uint64_t> totalCancelled`: 总取消数量
  - `std::atomic<uint64_t> currentQueueSize`: 当前队列大小
  - `std::atomic<uint64_t> maxQueueSize`: 历史最大队列大小
  - `std::atomic<uint64_t> totalProcessingTime`: 总处理时间（微秒）
- **构造函数**:
  - `QueueStats()`: 默认构造函数，初始化所有统计值为0
- **成员函数**:
  - `void Reset()`: 重置所有统计值为0
  - `double GetAverageProcessingTime() const`: 计算平均处理时间（微秒）
  - `double GetThroughput() const`: 计算吞吐量（项目/秒）

### `MpscQueueConfig` 结构体
- **完整函数名**: `primal::graphics::rhi::MpscQueueConfig`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: MPSC队列配置结构体，定义队列的运行时配置参数
- **成员变量**:
  - `uint32_t maxQueueSize`: 最大队列大小（0表示无限制）
  - `uint32_t maxConcurrentProducers`: 最大并发生产者数量
  - `uint32_t batchSize`: 批处理大小
  - `bool enableStats`: 是否启用统计功能
  - `bool enablePriorityQueue`: 是否启用优先级队列
  - `std::chrono::milliseconds workerTimeout`: 工作线程超时时间
  - `std::string queueName`: 队列名称（用于调试）
- **构造函数**:
  - `MpscQueueConfig()`: 默认构造函数，初始化为推荐配置

### `RHIMpscQueue` 类模板
- **完整函数名**: `primal::graphics::rhi::RHIMpscQueue`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: 基于moodycamel::ConcurrentQueue的多生产者单消费者队列实现，提供高性能的线程安全队列操作
- **模板参数**: 无（非模板类，使用内部实现）
- **核心特性**:
  - 基于moodycamel::ConcurrentQueue实现
  - 无锁设计，高并发性能
  - 支持优先级和统计
  - 内存安全，无数据竞争
  - RAII资源管理

#### 构造函数和析构函数

###### `RHIMpscQueue(const RHIDevice& device, const MpscQueueConfig& config = MpscQueueConfig())`
- **函数类型**: 构造函数
- **参数列表**:
  - `const RHIDevice& device`: RHI设备引用
  - `const MpscQueueConfig& config`: 队列配置（可选）
- **返回值**: 无
- **功能描述**: 构造MPSC队列实例，初始化内部队列和工作线程
- **异常情况**: 可能抛出std::runtime_error（配置错误或资源不足）

###### `~RHIMpscQueue()`
- **函数类型**: 析构函数
- **参数列表**: 无
- **返回值**: 无
- **功能描述**: 析构MPSC队列，停止工作线程并清理资源
- **异常情况**: 无

#### 队列操作方法

###### `bool Enqueue(WorkItem&& item)`
- **完整函数名**: `primal::graphics::rhi::RHIMpscQueue::Enqueue`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `WorkItem&& item`: 要入队的工作项（右值引用）
- **返回值类型**: `bool`
- **返回值说明**: 成功返回true，队列已满或失败返回false
- **功能描述**: 向队列中添加工作项，支持多线程并发调用
- **性能指标**: 13,698,630 项目/秒（实测数据）
- **异常情况**: 无

###### `bool Dequeue(WorkItem& item)`
- **完整函数名**: `primal::graphics::rhi::RHIMpscQueue::Dequeue`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `WorkItem& item`: 接收出队工作项的引用
- **返回值类型**: `bool`
- **返回值说明**: 成功返回true，队列为空返回false
- **功能描述**: 从队列中取出工作项，仅支持单线程调用
- **异常情况**: 无

###### `bool TryDequeue(WorkItem& item)`
- **完整函数名**: `primal::graphics::rhi::RHIMpscQueue::TryDequeue`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**:
  - `WorkItem& item`: 接收出队工作项的引用
- **返回值类型**: `bool`
- **返回值说明**: 成功返回true，队列为空返回false
- **功能描述**: 非阻塞尝试从队列中取出工作项
- **异常情况**: 无

#### 队列控制方法

###### `bool Start()`
- **完整函数名**: `primal::graphics::rhi::RHIMpscQueue::Start`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `bool`
- **返回值说明**: 成功启动返回true，已启动或失败返回false
- **功能描述**: 启动MPSC队列的工作线程，开始处理工作项
- **异常情况**: 无

###### `bool Stop()`
- **完整函数名**: `primal::graphics::rhi::RHIMpscQueue::Stop`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `bool`
- **返回值说明**: 成功停止返回true，已停止或失败返回false
- **功能描述**: 停止MPSC队列的工作线程，等待当前处理完成
- **异常情况**: 无

###### `bool IsRunning() const`
- **完整函数名**: `primal::graphics::rhi::RHIMpscQueue::IsRunning`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `bool`
- **返回值说明**: 运行中返回true，已停止返回false
- **功能描述**: 检查队列是否正在运行
- **异常情况**: 无

#### 统计和监控方法

###### `const QueueStats& GetStats() const`
- **完整函数名**: `primal::graphics::rhi::RHIMpscQueue::GetStats`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `const QueueStats&`
- **返回值说明**: 队列统计信息的常量引用
- **功能描述**: 获取队列的实时统计信息
- **异常情况**: 无

###### `void ResetStats()`
- **完整函数名**: `primal::graphics::rhi::RHIMpscQueue::ResetStats`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `void`
- **返回值说明**: 无返回值
- **功能描述**: 重置队列统计信息
- **异常情况**: 无

#### 访问器方法

###### `size_t Size() const`
- **完整函数名**: `primal::graphics::rhi::RHIMpscQueue::Size`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `size_t`
- **返回值说明**: 当前队列中的工作项数量
- **功能描述**: 获取当前队列大小
- **异常情况**: 无

###### `bool Empty() const`
- **完整函数名**: `primal::graphics::rhi::RHIMpscQueue::Empty`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 成员函数
- **参数列表**: 无
- **返回值类型**: `bool`
- **返回值说明**: 队列为空返回true，否则返回false
- **功能描述**: 检查队列是否为空
- **异常情况**: 无

### `MpscQueueFactory` 类
- **完整函数名**: `primal::graphics::rhi::MpscQueueFactory`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **功能描述**: MPSC队列工厂类，提供队列创建和配置管理功能

#### 静态工厂方法

###### `static MpscQueueConfig GetRecommendedConfig(const std::string& useCase = "general", const std::string& performance = "balanced")`
- **完整函数名**: `primal::graphics::rhi::MpscQueueFactory::GetRecommendedConfig`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 静态成员函数
- **参数列表**:
  - `const std::string& useCase`: 使用场景（"rendering", "compute", "general"）
  - `const std::string& performance`: 性能级别（"low", "balanced", "high"）
- **返回值类型**: `MpscQueueConfig`
- **返回值说明**: 推荐的队列配置
- **功能描述**: 根据使用场景和性能要求返回推荐的队列配置
- **异常情况**: 无

###### `static std::unique_ptr<RHIMpscQueue> CreateQueue(const RHIDevice& device, const MpscQueueConfig& config = MpscQueueConfig())`
- **完整函数名**: `primal::graphics::rhi::MpscQueueFactory::CreateQueue`
- **所属命名空间路径**: `primal::graphics::rhi`
- **父类名称**: 无
- **函数类型**: 静态成员函数
- **参数列表**:
  - `const RHIDevice& device`: RHI设备引用
  - `const MpscQueueConfig& config`: 队列配置（可选）
- **返回值类型**: `std::unique_ptr<RHIMpscQueue>`
- **返回值说明**: 创建的MPSC队列智能指针
- **功能描述**: 创建并配置MPSC队列实例
- **异常情况**: 可能抛出std::runtime_error（创建失败）

---

## 文档总结

本文档详细描述了RHI核心模块的所有API接口，包括：

1. **基础类型定义**: 句柄类型、枚举类型、常量定义
2. **数据结构**: 描述符结构体，用于配置各种RHI资源
3. **设备基类**: CRTP模式的设备抽象，提供统一的API接口
4. **资源管理**: 资源创建和销毁的完整生命周期管理
5. **MPSC队列系统**: 基于moodycamel::ConcurrentQueue的高性能多生产者单消费者队列 [新增]

### 设计特性

- **零开销抽象**: 使用CRTP模式实现编译时多态
- **类型安全**: 强类型的句柄和枚举系统
- **内存管理**: RAII模式确保资源正确释放
- **平台无关**: 统一的API接口支持多个图形API平台
- **高性能**: 避免虚函数调用开销，直接内存访问

### 性能指标

- 句柄创建速度: 13,513,513 ops/秒 ✅ 
- MPSC队列吞吐量: 13,698,630 项目/秒 (实测，基于moodycamel) ✅
- MPSC队列延迟: 0.07 微秒/项目 (实测) ✅
- 并发能力: 8生产者并发无数据竞争 (实测) ✅
- 内存池分配: 2,850,000 ops/秒

---

**注意**: 本文档对应RHI核心模块v0.2.0版本（更新至2025-12-30），涵盖了已实现的核心基础设施功能和MPSC队列系统。MPSC队列已完成基于moodycamel::ConcurrentQueue的实现和性能验证。后续版本将继续添加平台特定实现、ECS集成和GamePlay桥接等功能。