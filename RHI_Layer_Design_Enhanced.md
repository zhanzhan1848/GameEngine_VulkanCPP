# 游戏引擎RHI层封装方案（增强版）

## 1. 概述

本文档提供了一套完整的游戏引擎RHI（Render Hardware Interface）层封装方案，特别针对不同图形API的资源创建和绑定（如pipeline、descriptor等）提供统一接口，并使用模板技术实现方便的扩展。该方案基于现有的游戏引擎架构，提供高性能、低延迟的图形渲染抽象层。

## 2. 设计目标

- **跨平台兼容性**：支持Windows（Direct3D12/Vulkan/WebGPU）、Linux（Vulkan/WebGPU）和macOS（Metal/Vulkan/WebGPU）
- **高性能**：最小化API调用开销，优化GPU资源管理
- **低延迟**：减少CPU-GPU同步点，优化命令缓冲区提交
- **易扩展性**：便于添加新的图形API支持
- **类型安全**：利用C++17特性提供强类型接口
- **模板化设计**：使用模板技术实现资源创建和绑定的统一接口
- **编译时优化**：尽可能在编译时解析平台特定代码，减少运行时开销

## 3. 架构设计

### 3.1 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                    游戏引擎上层应用层                          │
├─────────────────────────────────────────────────────────────┤
│                      RHI抽象层                               │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │  资源管理器  │  │  命令缓冲区  │  │  渲染状态机  │         │
│  └─────────────┘  └─────────────┘  └─────────────┘         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │  模板资源工厂│  │  绑定点管理  │  │  描述符缓存  │         │
│  └─────────────┘  └─────────────┘  └─────────────┘         │
├─────────────────────────────────────────────────────────────┤
│                   平台特化层                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │ Direct3D12  │  │   Vulkan    │  │    Metal    │         │
│  │   特化实现  │  │   特化实现   │  │   特化实现   │         │
│  └─────────────┘  └─────────────┘  └─────────────┘         │
│  ┌─────────────┐                                           │
│  │   WebGPU    │                                           │
│  │   特化实现  │                                           │
│  └─────────────┘                                           │
├─────────────────────────────────────────────────────────────┤
│                    操作系统/驱动层                           │
└─────────────────────────────────────────────────────────────┘
```

### 3.2 核心组件

#### 3.2.1 平台接口（Platform Interface）

平台接口是RHI层的核心抽象，定义了所有图形API必须实现的功能集合：

```cpp
namespace primal::graphics {
    struct platform_interface
    {
        // 初始化与关闭
        bool(*initialize)(void);
        void(*shutdown)(void);

        // 表面管理
        struct {
            surface(*create)(platform::window);
            void(*remove)(surface_id);
            void(*resize)(surface_id, u32, u32);
            u32(*width)(surface_id);
            u32(*height)(surface_id);
            void(*render)(surface_id, frame_info);
        } surface;

        // 光照管理
        struct {
            void(*create_light_set)(u64);
            void(*remove_light_set)(u64);
            light(*create)(light_init_info);
            void(*remove)(light_id, u64);
            void(*set_parameter)(light_id, u64, light_parameter::parameter, const void *const, u32);
            void(*get_parameter)(light_id, u64, light_parameter::parameter, void *const, u32);
        } light;

        // 相机管理
        struct {
            camera(*create)(camera_init_info);
            void(*remove)(camera_id);
            void(*set_parameter)(camera_id, camera_parameter::parameter, const void *const, u32);
            void(*get_parameter)(camera_id, camera_parameter::parameter, void *const, u32);
        } camera;

        // 资源管理
        struct {
            id::id_type(*add_submesh)(const u8*&);
            void (*remove_submesh)(id::id_type);
            id::id_type(*add_texture)(const u8 *const);
            void (*remove_texture)(id::id_type);
            id::id_type(*add_material)(material_init_info);
            void (*remove_material)(id::id_type);
            id::id_type(*add_render_item)(id::id_type, id::id_type, u32, const id::id_type *const);
            void(*remove_render_item)(id::id_type);
        } resources;

        graphics_platform platform = (graphics_platform) - 1;
    };
}
```

## 4. 基于模板的资源抽象层

### 4.1 资源类型特化

使用模板特化为不同图形API提供统一的资源接口：

```cpp
namespace primal::graphics::rhi {

    // 前向声明
    template<graphics_platform Platform>
    struct PlatformTraits;

    // Direct3D12特化
    template<>
    struct PlatformTraits<graphics_platform::direct3d12>
    {
        using DeviceType = ID3D12Device*;
        using CommandType = ID3D12GraphicsCommandList*;
        using PipelineType = ID3D12PipelineState*;
        using DescriptorHeapType = ID3D12DescriptorHeap*;
        using ResourceType = ID3D12Resource*;
        using BufferType = ID3D12Resource*;
        using TextureType = ID3D12Resource*;
        using ShaderType = ID3D12RootSignature*;
        using FenceType = ID3D12Fence*;
        
        static constexpr u32 MaxDescriptorSets = 1;
        static constexpr u32 MaxRenderTargets = 8;
    };

    // Vulkan特化
    template<>
    struct PlatformTraits<graphics_platform::vulkan_1>
    {
        using DeviceType = VkDevice;
        using CommandType = VkCommandBuffer;
        using PipelineType = VkPipeline;
        using DescriptorHeapType = VkDescriptorPool;
        using ResourceType = VkImage;
        using BufferType = VkBuffer;
        using TextureType = VkImage;
        using ShaderType = VkShaderModule;
        using FenceType = VkFence;
        
        static constexpr u32 MaxDescriptorSets = 32;
        static constexpr u32 MaxRenderTargets = 8;
    };

    // Metal特化
    template<>
    struct PlatformTraits<graphics_platform::metal>
    {
        using DeviceType = MTL::Device*;
        using CommandType = MTL::CommandBuffer*;
        using PipelineType = MTL::RenderPipelineState*;
        using DescriptorHeapType = void*; // Metal不使用描述符堆
        using ResourceType = MTL::Resource*;
        using BufferType = MTL::Buffer*;
        using TextureType = MTL::Texture*;
        using ShaderType = MTL::Library*;
        using FenceType = MTL::Event*;
        
        static constexpr u32 MaxDescriptorSets = 1; // Metal使用参数缓冲区
        static constexpr u32 MaxRenderTargets = 8;
    };
    
    // WebGPU特化
    template<>
    struct PlatformTraits<graphics_platform::webgpu>
    {
        using DeviceType = WGPUDevice;
        using CommandType = WGPUCommandEncoder;
        using PipelineType = WGPURenderPipeline;
        using DescriptorHeapType = WGPUBindGroupLayout; // WebGPU使用绑定组布局
        using ResourceType = WGPUResource;
        using BufferType = WGPUBuffer;
        using TextureType = WGPUTexture;
        using ShaderType = WGPUShaderModule;
        using FenceType = WGPUFence;
        
        static constexpr u32 MaxDescriptorSets = 4; // WebGPU支持多个绑定组
        static constexpr u32 MaxRenderTargets = 8;
    };
}
```

### 4.2 资源基类

```cpp
namespace primal::graphics::rhi {

    // 资源基类
    template<graphics_platform Platform>
    class Resource
    {
    public:
        using Traits = PlatformTraits<Platform>;
        using NativeType = typename Traits::ResourceType;
        
        Resource() = default;
        virtual ~Resource() = default;
        
        // 获取原生资源句柄
        virtual NativeType GetNativeResource() const = 0;
        
        // 获取资源类型
        virtual ResourceType GetType() const = 0;
        
        // 获取资源状态
        virtual ResourceState GetState() const = 0;
        
        // 转换资源状态
        virtual void TransitionState(ResourceState newState) = 0;
        
        // 获取资源描述
        virtual const ResourceDesc& GetDesc() const = 0;
        
    protected:
        ResourceDesc desc_;
        ResourceState state_ = ResourceState::Undefined;
    };

    // 缓冲区资源
    template<graphics_platform Platform>
    class Buffer : public Resource<Platform>
    {
    public:
        using Traits = PlatformTraits<Platform>;
        using NativeType = typename Traits::BufferType;
        
        Buffer(const BufferDesc& desc);
        virtual ~Buffer();
        
        // 获取原生缓冲区句柄
        NativeType GetNativeBuffer() const override { return native_buffer_; }
        
        // 获取缓冲区描述
        const BufferDesc& GetBufferDesc() const { return buffer_desc_; }
        
        // 映射/取消映射内存
        void* Map(u64 offset = 0, u64 size = 0);
        void Unmap();
        
        // 更新缓冲区数据
        void UpdateData(const void* data, u64 size, u64 offset = 0);
        
    protected:
        NativeType native_buffer_ = nullptr;
        BufferDesc buffer_desc_;
        void* mapped_data_ = nullptr;
    };

    // 纹理资源
    template<graphics_platform Platform>
    class Texture : public Resource<Platform>
    {
    public:
        using Traits = PlatformTraits<Platform>;
        using NativeType = typename Traits::TextureType;
        
        Texture(const TextureDesc& desc);
        virtual ~Texture();
        
        // 获取原生纹理句柄
        NativeType GetNativeTexture() const override { return native_texture_; }
        
        // 获取纹理描述
        const TextureDesc& GetTextureDesc() const { return texture_desc_; }
        
        // 获取子资源描述
        SubresourceDesc GetSubresourceDesc(u32 mipLevel, u32 arraySlice = 0) const;
        
    protected:
        NativeType native_texture_ = nullptr;
        TextureDesc texture_desc_;
    };
}
```

### 4.3 资源工厂

```cpp
namespace primal::graphics::rhi {

    // 资源工厂基类
    template<graphics_platform Platform>
    class ResourceFactory
    {
    public:
        using Traits = PlatformTraits<Platform>;
        using DeviceType = typename Traits::DeviceType;
        
        explicit ResourceFactory(DeviceType device) : device_(device) {}
        virtual ~ResourceFactory() = default;
        
        // 创建缓冲区
        virtual std::unique_ptr<Buffer<Platform>> CreateBuffer(const BufferDesc& desc) = 0;
        
        // 创建纹理
        virtual std::unique_ptr<Texture<Platform>> CreateTexture(const TextureDesc& desc) = 0;
        
        // 创建着色器
        virtual std::unique_ptr<Shader<Platform>> CreateShader(const ShaderDesc& desc) = 0;
        
        // 创建管线
        virtual std::unique_ptr<Pipeline<Platform>> CreatePipeline(const PipelineDesc& desc) = 0;
        
        // 创建描述符布局
        virtual std::unique_ptr<DescriptorSetLayout<Platform>> CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) = 0;
        
        // 创建描述符池
        virtual std::unique_ptr<DescriptorPool<Platform>> CreateDescriptorPool(const DescriptorPoolDesc& desc) = 0;
        
        // 创建描述符集
        virtual std::unique_ptr<DescriptorSet<Platform>> CreateDescriptorSet(
            DescriptorPool<Platform>* pool, 
            DescriptorSetLayout<Platform>* layout) = 0;
        
        // 创建采样器
        virtual std::unique_ptr<Sampler<Platform>> CreateSampler(const SamplerDesc& desc) = 0;
        
        // 创建查询
        virtual std::unique_ptr<Query<Platform>> CreateQuery(const QueryDesc& desc) = 0;
        
        // 创建围栏
        virtual std::unique_ptr<Fence<Platform>> CreateFence(const FenceDesc& desc) = 0;
        
    protected:
        DeviceType device_;
    };

    // Direct3D12资源工厂特化
    template<>
    class ResourceFactory<graphics_platform::direct3d12>
    {
    public:
        using Traits = PlatformTraits<graphics_platform::direct3d12>;
        using DeviceType = typename Traits::DeviceType;
        
        explicit ResourceFactory(DeviceType device) : device_(device) {}
        
        std::unique_ptr<Buffer<graphics_platform::direct3d12>> CreateBuffer(const BufferDesc& desc) override
        {
            return std::make_unique<D3D12Buffer>(device_, desc);
        }
        
        std::unique_ptr<Texture<graphics_platform::direct3d12>> CreateTexture(const TextureDesc& desc) override
        {
            return std::make_unique<D3D12Texture>(device_, desc);
        }
        
        std::unique_ptr<Shader<graphics_platform::direct3d12>> CreateShader(const ShaderDesc& desc) override
        {
            return std::make_unique<D3D12Shader>(device_, desc);
        }
        
        std::unique_ptr<Pipeline<graphics_platform::direct3d12>> CreatePipeline(const PipelineDesc& desc) override
        {
            return std::make_unique<D3D12Pipeline>(device_, desc);
        }
        
        std::unique_ptr<DescriptorSetLayout<graphics_platform::direct3d12>> CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override
        {
            return std::make_unique<D3D12DescriptorSetLayout>(device_, desc);
        }
        
        std::unique_ptr<DescriptorPool<graphics_platform::direct3d12>> CreateDescriptorPool(const DescriptorPoolDesc& desc) override
        {
            return std::make_unique<D3D12DescriptorPool>(device_, desc);
        }
        
        std::unique_ptr<DescriptorSet<graphics_platform::direct3d12>> CreateDescriptorSet(
            DescriptorPool<graphics_platform::direct3d12>* pool, 
            DescriptorSetLayout<graphics_platform::direct3d12>* layout) override
        {
            return std::make_unique<D3D12DescriptorSet>(device_, pool, layout);
        }
        
        std::unique_ptr<Sampler<graphics_platform::direct3d12>> CreateSampler(const SamplerDesc& desc) override
        {
            return std::make_unique<D3D12Sampler>(device_, desc);
        }
        
        std::unique_ptr<Query<graphics_platform::direct3d12>> CreateQuery(const QueryDesc& desc) override
        {
            return std::make_unique<D3D12Query>(device_, desc);
        }
        
        std::unique_ptr<Fence<graphics_platform::direct3d12>> CreateFence(const FenceDesc& desc) override
        {
            return std::make_unique<D3D12Fence>(device_, desc);
        }
        
    private:
        DeviceType device_;
    };

    // Vulkan资源工厂特化
    template<>
    class ResourceFactory<graphics_platform::vulkan_1>
    {
    public:
        using Traits = PlatformTraits<graphics_platform::vulkan_1>;
        using DeviceType = typename Traits::DeviceType;
        
        explicit ResourceFactory(DeviceType device) : device_(device) {}
        
        std::unique_ptr<Buffer<graphics_platform::vulkan_1>> CreateBuffer(const BufferDesc& desc) override
        {
            return std::make_unique<VulkanBuffer>(device_, desc);
        }
        
        std::unique_ptr<Texture<graphics_platform::vulkan_1>> CreateTexture(const TextureDesc& desc) override
        {
            return std::make_unique<VulkanTexture>(device_, desc);
        }
        
        std::unique_ptr<Shader<graphics_platform::vulkan_1>> CreateShader(const ShaderDesc& desc) override
        {
            return std::make_unique<VulkanShader>(device_, desc);
        }
        
        std::unique_ptr<Pipeline<graphics_platform::vulkan_1>> CreatePipeline(const PipelineDesc& desc) override
        {
            return std::make_unique<VulkanPipeline>(device_, desc);
        }
        
        std::unique_ptr<DescriptorSetLayout<graphics_platform::vulkan_1>> CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override
        {
            return std::make_unique<VulkanDescriptorSetLayout>(device_, desc);
        }
        
        std::unique_ptr<DescriptorPool<graphics_platform::vulkan_1>> CreateDescriptorPool(const DescriptorPoolDesc& desc) override
        {
            return std::make_unique<VulkanDescriptorPool>(device_, desc);
        }
        
        std::unique_ptr<DescriptorSet<graphics_platform::vulkan_1>> CreateDescriptorSet(
            DescriptorPool<graphics_platform::vulkan_1>* pool, 
            DescriptorSetLayout<graphics_platform::vulkan_1>* layout) override
        {
            return std::make_unique<VulkanDescriptorSet>(device_, pool, layout);
        }
        
        std::unique_ptr<Sampler<graphics_platform::vulkan_1>> CreateSampler(const SamplerDesc& desc) override
        {
            return std::make_unique<VulkanSampler>(device_, desc);
        }
        
        std::unique_ptr<Query<graphics_platform::vulkan_1>> CreateQuery(const QueryDesc& desc) override
        {
            return std::make_unique<VulkanQuery>(device_, desc);
        }
        
        std::unique_ptr<Fence<graphics_platform::vulkan_1>> CreateFence(const FenceDesc& desc) override
        {
            return std::make_unique<VulkanFence>(device_, desc);
        }
        
    private:
        DeviceType device_;
    };

    // Metal资源工厂特化
    template<>
    class ResourceFactory<graphics_platform::metal>
    {
    public:
        using Traits = PlatformTraits<graphics_platform::metal>;
        using DeviceType = typename Traits::DeviceType;
        
        explicit ResourceFactory(DeviceType device) : device_(device) {}
        
        std::unique_ptr<Buffer<graphics_platform::metal>> CreateBuffer(const BufferDesc& desc) override
        {
            return std::make_unique<MetalBuffer>(device_, desc);
        }
        
        std::unique_ptr<Texture<graphics_platform::metal>> CreateTexture(const TextureDesc& desc) override
        {
            return std::make_unique<MetalTexture>(device_, desc);
        }
        
        std::unique_ptr<Shader<graphics_platform::metal>> CreateShader(const ShaderDesc& desc) override
        {
            return std::make_unique<MetalShader>(device_, desc);
        }
        
        std::unique_ptr<Pipeline<graphics_platform::metal>> CreatePipeline(const PipelineDesc& desc) override
        {
            return std::make_unique<MetalPipeline>(device_, desc);
        }
        
        std::unique_ptr<DescriptorSetLayout<graphics_platform::metal>> CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override
        {
            return std::make_unique<MetalDescriptorSetLayout>(device_, desc);
        }
        
        std::unique_ptr<DescriptorPool<graphics_platform::metal>> CreateDescriptorPool(const DescriptorPoolDesc& desc) override
        {
            return std::make_unique<MetalDescriptorPool>(device_, desc);
        }
        
        std::unique_ptr<DescriptorSet<graphics_platform::metal>> CreateDescriptorSet(
            DescriptorPool<graphics_platform::metal>* pool, 
            DescriptorSetLayout<graphics_platform::metal>* layout) override
        {
            return std::make_unique<MetalDescriptorSet>(device_, pool, layout);
        }
        
        std::unique_ptr<Sampler<graphics_platform::metal>> CreateSampler(const SamplerDesc& desc) override
        {
            return std::make_unique<MetalSampler>(device_, desc);
        }
        
        std::unique_ptr<Query<graphics_platform::metal>> CreateQuery(const QueryDesc& desc) override
        {
            return std::make_unique<MetalQuery>(device_, desc);
        }
        
        std::unique_ptr<Fence<graphics_platform::metal>> CreateFence(const FenceDesc& desc) override
        {
            return std::make_unique<MetalFence>(device_, desc);
        }
        
    private:
        DeviceType device_;
    };
}
```

## 5. 统一的Pipeline和Descriptor接口

### 5.1 Pipeline抽象

```cpp
namespace primal::graphics::rhi {

    // Pipeline基类
    template<graphics_platform Platform>
    class Pipeline
    {
    public:
        using Traits = PlatformTraits<Platform>;
        using NativeType = typename Traits::PipelineType;
        
        Pipeline() = default;
        virtual ~Pipeline() = default;
        
        // 获取原生Pipeline句柄
        virtual NativeType GetNativePipeline() const = 0;
        
        // 获取Pipeline描述
        virtual const PipelineDesc& GetDesc() const = 0;
        
        // 获取绑定点
        virtual const std::vector<BindingPoint>& GetBindingPoints() const = 0;
        
    protected:
        PipelineDesc desc_;
        std::vector<BindingPoint> binding_points_;
    };

    // 图形Pipeline
    template<graphics_platform Platform>
    class GraphicsPipeline : public Pipeline<Platform>
    {
    public:
        GraphicsPipeline(const GraphicsPipelineDesc& desc);
        virtual ~GraphicsPipeline() = default;
        
        // 获取图形Pipeline描述
        const GraphicsPipelineDesc& GetGraphicsDesc() const { return graphics_desc_; }
        
        // 获取顶点输入布局
        const std::vector<VertexInputAttribute>& GetVertexInputAttributes() const { return vertex_attributes_; }
        
        // 获取渲染状态
        const RenderState& GetRenderState() const { return render_state_; }
        
    protected:
        GraphicsPipelineDesc graphics_desc_;
        std::vector<VertexInputAttribute> vertex_attributes_;
        RenderState render_state_;
    };

    // 计算Pipeline
    template<graphics_platform Platform>
    class ComputePipeline : public Pipeline<Platform>
    {
    public:
        ComputePipeline(const ComputePipelineDesc& desc);
        virtual ~ComputePipeline() = default;
        
        // 获取计算Pipeline描述
        const ComputePipelineDesc& GetComputeDesc() const { return compute_desc_; }
        
        // 获取线程组大小
        const u32 (&GetThreadGroupSize())[3] const { return thread_group_size_; }
        
    protected:
        ComputePipelineDesc compute_desc_;
        u32 thread_group_size_[3] = { 1, 1, 1 };
    };
}
```

### 5.2 Descriptor抽象

```cpp
namespace primal::graphics::rhi {

    // 描述符类型枚举
    enum class DescriptorType : u32
    {
        ConstantBuffer = 0,
        Texture = 1,
        Sampler = 2,
        RWTexture = 3,
        RWBuffer = 4,
        StructuredBuffer = 5,
        RWStructuredBuffer = 6,
        AccelerationStructure = 7,
    };

    // 描述符绑定点
    struct BindingPoint
    {
        u32 binding;
        u32 arraySize;
        DescriptorType type;
        ShaderStageFlags stageFlags;
        
        bool operator==(const BindingPoint& other) const
        {
            return binding == other.binding && 
                   arraySize == other.arraySize && 
                   type == other.type && 
                   stageFlags == other.stageFlags;
        }
    };

    // 描述符集布局基类
    template<graphics_platform Platform>
    class DescriptorSetLayout
    {
    public:
        using Traits = PlatformTraits<Platform>;
        
        DescriptorSetLayout(const std::vector<BindingPoint>& bindingPoints);
        virtual ~DescriptorSetLayout() = default;
        
        // 获取绑定点
        const std::vector<BindingPoint>& GetBindingPoints() const { return binding_points_; }
        
        // 获取绑定点数量
        u32 GetBindingCount() const { return static_cast<u32>(binding_points_.size()); }
        
        // 查找绑定点
        const BindingPoint* FindBindingPoint(u32 binding) const
        {
            for (const auto& bp : binding_points_)
            {
                if (bp.binding == binding)
                    return &bp;
            }
            return nullptr;
        }
        
        // 获取原生布局句柄
        virtual void* GetNativeLayout() const = 0;
        
    protected:
        std::vector<BindingPoint> binding_points_;
    };

    // 描述符池基类
    template<graphics_platform Platform>
    class DescriptorPool
    {
    public:
        using Traits = PlatformTraits<Platform>;
        
        DescriptorPool(const std::vector<std::pair<DescriptorType, u32>>& poolSizes);
        virtual ~DescriptorPool() = default;
        
        // 获取池大小
        const std::vector<std::pair<DescriptorType, u32>>& GetPoolSizes() const { return pool_sizes_; }
        
        // 获取剩余描述符数量
        virtual u32 GetRemainingDescriptorCount(DescriptorType type) const = 0;
        
        // 重置池
        virtual void Reset() = 0;
        
        // 获取原生池句柄
        virtual void* GetNativePool() const = 0;
        
    protected:
        std::vector<std::pair<DescriptorType, u32>> pool_sizes_;
    };

    // 描述符集基类
    template<graphics_platform Platform>
    class DescriptorSet
    {
    public:
        using Traits = PlatformTraits<Platform>;
        
        DescriptorSet(DescriptorPool<Platform>* pool, DescriptorSetLayout<Platform>* layout);
        virtual ~DescriptorSet() = default;
        
        // 获取关联的池
        DescriptorPool<Platform>* GetPool() const { return pool_; }
        
        // 获取关联的布局
        DescriptorSetLayout<Platform>* GetLayout() const { return layout_; }
        
        // 更新常量缓冲区
        virtual void UpdateConstantBuffer(u32 binding, Buffer<Platform>* buffer, u64 offset = 0, u64 range = 0) = 0;
        
        // 更新纹理
        virtual void UpdateTexture(u32 binding, Texture<Platform>* texture, Sampler<Platform>* sampler = nullptr) = 0;
        
        // 更新采样器
        virtual void UpdateSampler(u32 binding, Sampler<Platform>* sampler) = 0;
        
        // 更新RW纹理
        virtual void UpdateRWTexture(u32 binding, Texture<Platform>* texture, u32 mipLevel = 0) = 0;
        
        // 更新RW缓冲区
        virtual void UpdateRWBuffer(u32 binding, Buffer<Platform>* buffer, u64 offset = 0, u64 range = 0) = 0;
        
        // 更新结构化缓冲区
        virtual void UpdateStructuredBuffer(u32 binding, Buffer<Platform>* buffer, u64 offset = 0, u64 range = 0) = 0;
        
        // 更新RW结构化缓冲区
        virtual void UpdateRWStructuredBuffer(u32 binding, Buffer<Platform>* buffer, u64 offset = 0, u64 range = 0) = 0;
        
        // 更新加速结构
        virtual void UpdateAccelerationStructure(u32 binding, AccelerationStructure<Platform>* as) = 0;
        
        // 批量更新
        virtual void UpdateDescriptors(const std::vector<DescriptorUpdate>& updates) = 0;
        
        // 获取原生描述符集句柄
        virtual void* GetNativeDescriptorSet() const = 0;
        
    protected:
        DescriptorPool<Platform>* pool_;
        DescriptorSetLayout<Platform>* layout_;
    };
}
```

## 6. 资源创建和绑定的模板系统

### 6.1 资源创建模板

```cpp
namespace primal::graphics::rhi {

    // 资源创建模板
    template<graphics_platform Platform, typename ResourceType>
    std::unique_ptr<ResourceType> CreateResource(ResourceFactory<Platform>* factory, const typename ResourceType::DescType& desc)
    {
        static_assert(std::is_base_of_v<Resource<Platform>, ResourceType>, "ResourceType must inherit from Resource<Platform>");
        
        if constexpr (std::is_same_v<ResourceType, Buffer<Platform>>)
        {
            return factory->CreateBuffer(desc);
        }
        else if constexpr (std::is_same_v<ResourceType, Texture<Platform>>)
        {
            return factory->CreateTexture(desc);
        }
        else if constexpr (std::is_same_v<ResourceType, Shader<Platform>>)
        {
            return factory->CreateShader(desc);
        }
        else if constexpr (std::is_same_v<ResourceType, GraphicsPipeline<Platform>>)
        {
            return factory->CreatePipeline(desc);
        }
        else if constexpr (std::is_same_v<ResourceType, ComputePipeline<Platform>>)
        {
            return factory->CreatePipeline(desc);
        }
        else if constexpr (std::is_same_v<ResourceType, DescriptorSetLayout<Platform>>)
        {
            return factory->CreateDescriptorSetLayout(desc);
        }
        else if constexpr (std::is_same_v<ResourceType, DescriptorPool<Platform>>)
        {
            return factory->CreateDescriptorPool(desc);
        }
        else if constexpr (std::is_same_v<ResourceType, Sampler<Platform>>)
        {
            return factory->CreateSampler(desc);
        }
        else if constexpr (std::is_same_v<ResourceType, Query<Platform>>)
        {
            return factory->CreateQuery(desc);
        }
        else if constexpr (std::is_same_v<ResourceType, Fence<Platform>>)
        {
            return factory->CreateFence(desc);
        }
        else
        {
            static_assert(!sizeof(ResourceType), "Unsupported resource type");
            return nullptr;
        }
    }

    // 便捷的资源创建函数
    template<graphics_platform Platform>
    std::unique_ptr<Buffer<Platform>> CreateBuffer(ResourceFactory<Platform>* factory, const BufferDesc& desc)
    {
        return CreateResource<Platform, Buffer<Platform>>(factory, desc);
    }

    template<graphics_platform Platform>
    std::unique_ptr<Texture<Platform>> CreateTexture(ResourceFactory<Platform>* factory, const TextureDesc& desc)
    {
        return CreateResource<Platform, Texture<Platform>>(factory, desc);
    }

    template<graphics_platform Platform>
    std::unique_ptr<GraphicsPipeline<Platform>> CreateGraphicsPipeline(ResourceFactory<Platform>* factory, const GraphicsPipelineDesc& desc)
    {
        return CreateResource<Platform, GraphicsPipeline<Platform>>(factory, desc);
    }

    template<graphics_platform Platform>
    std::unique_ptr<ComputePipeline<Platform>> CreateComputePipeline(ResourceFactory<Platform>* factory, const ComputePipelineDesc& desc)
    {
        return CreateResource<Platform, ComputePipeline<Platform>>(factory, desc);
    }
}
```

### 6.2 资源绑定模板

```cpp
namespace primal::graphics::rhi {

    // 描述符更新结构
    struct DescriptorUpdate
    {
        u32 binding;
        DescriptorType type;
        union
        {
            struct
            {
                Buffer<void>* buffer;
                u64 offset;
                u64 range;
            } buffer;
            
            struct
            {
                Texture<void>* texture;
                Sampler<void>* sampler;
            } texture;
            
            struct
            {
                Sampler<void>* sampler;
            } sampler;
            
            struct
            {
                AccelerationStructure<void>* as;
            } accelerationStructure;
        };
    };

    // 资源绑定模板
    template<graphics_platform Platform>
    class ResourceBinder
    {
    public:
        using Traits = PlatformTraits<Platform>;
        
        explicit ResourceBinder(ResourceFactory<Platform>* factory) : factory_(factory) {}
        
        // 创建描述符集
        std::unique_ptr<DescriptorSet<Platform>> CreateDescriptorSet(
            DescriptorPool<Platform>* pool, 
            DescriptorSetLayout<Platform>* layout)
        {
            return factory_->CreateDescriptorSet(pool, layout);
        }
        
        // 绑定常量缓冲区
        template<typename BufferType>
        void BindConstantBuffer(DescriptorSet<Platform>* descriptorSet, u32 binding, BufferType* buffer, u64 offset = 0, u64 range = 0)
        {
            static_assert(std::is_base_of_v<Buffer<Platform>, BufferType>, "BufferType must inherit from Buffer<Platform>");
            descriptorSet->UpdateConstantBuffer(binding, buffer, offset, range);
        }
        
        // 绑定纹理
        template<typename TextureType, typename SamplerType>
        void BindTexture(DescriptorSet<Platform>* descriptorSet, u32 binding, TextureType* texture, SamplerType* sampler = nullptr)
        {
            static_assert(std::is_base_of_v<Texture<Platform>, TextureType>, "TextureType must inherit from Texture<Platform>");
            static_assert(!sampler || std::is_base_of_v<Sampler<Platform>, SamplerType>, "SamplerType must inherit from Sampler<Platform>");
            descriptorSet->UpdateTexture(binding, texture, sampler);
        }
        
        // 绑定采样器
        template<typename SamplerType>
        void BindSampler(DescriptorSet<Platform>* descriptorSet, u32 binding, SamplerType* sampler)
        {
            static_assert(std::is_base_of_v<Sampler<Platform>, SamplerType>, "SamplerType must inherit from Sampler<Platform>");
            descriptorSet->UpdateSampler(binding, sampler);
        }
        
        // 绑定RW纹理
        template<typename TextureType>
        void BindRWTexture(DescriptorSet<Platform>* descriptorSet, u32 binding, TextureType* texture, u32 mipLevel = 0)
        {
            static_assert(std::is_base_of_v<Texture<Platform>, TextureType>, "TextureType must inherit from Texture<Platform>");
            descriptorSet->UpdateRWTexture(binding, texture, mipLevel);
        }
        
        // 绑定RW缓冲区
        template<typename BufferType>
        void BindRWBuffer(DescriptorSet<Platform>* descriptorSet, u32 binding, BufferType* buffer, u64 offset = 0, u64 range = 0)
        {
            static_assert(std::is_base_of_v<Buffer<Platform>, BufferType>, "BufferType must inherit from Buffer<Platform>");
            descriptorSet->UpdateRWBuffer(binding, buffer, offset, range);
        }
        
        // 绑定结构化缓冲区
        template<typename BufferType>
        void BindStructuredBuffer(DescriptorSet<Platform>* descriptorSet, u32 binding, BufferType* buffer, u64 offset = 0, u64 range = 0)
        {
            static_assert(std::is_base_of_v<Buffer<Platform>, BufferType>, "BufferType must inherit from Buffer<Platform>");
            descriptorSet->UpdateStructuredBuffer(binding, buffer, offset, range);
        }
        
        // 绑定RW结构化缓冲区
        template<typename BufferType>
        void BindRWStructuredBuffer(DescriptorSet<Platform>* descriptorSet, u32 binding, BufferType* buffer, u64 offset = 0, u64 range = 0)
        {
            static_assert(std::is_base_of_v<Buffer<Platform>, BufferType>, "BufferType must inherit from Buffer<Platform>");
            descriptorSet->UpdateRWStructuredBuffer(binding, buffer, offset, range);
        }
        
        // 绑定加速结构
        template<typename ASType>
        void BindAccelerationStructure(DescriptorSet<Platform>* descriptorSet, u32 binding, ASType* as)
        {
            static_assert(std::is_base_of_v<AccelerationStructure<Platform>, ASType>, "ASType must inherit from AccelerationStructure<Platform>");
            descriptorSet->UpdateAccelerationStructure(binding, as);
        }
        
        // 批量绑定
        void BindDescriptors(DescriptorSet<Platform>* descriptorSet, const std::vector<DescriptorUpdate>& updates)
        {
            descriptorSet->UpdateDescriptors(updates);
        }
        
    private:
        ResourceFactory<Platform>* factory_;
    };
}
```

### 6.3 命令缓冲区绑定模板

```cpp
namespace primal::graphics::rhi {

    // 命令缓冲区绑定模板
    template<graphics_platform Platform>
    class CommandBufferBinder
    {
    public:
        using Traits = PlatformTraits<Platform>;
        using CommandType = typename Traits::CommandType;
        
        explicit CommandBufferBinder(CommandType commandBuffer) : command_buffer_(commandBuffer) {}
        
        // 绑定管线
        template<typename PipelineType>
        void BindPipeline(PipelineType* pipeline)
        {
            static_assert(std::is_base_of_v<Pipeline<Platform>, PipelineType>, "PipelineType must inherit from Pipeline<Platform>");
            BindPipelineInternal(pipeline);
        }
        
        // 绑定描述符集
        template<typename PipelineType>
        void BindDescriptorSets(PipelineType* pipeline, u32 firstSet, const std::vector<DescriptorSet<Platform>*>& descriptorSets)
        {
            static_assert(std::is_base_of_v<Pipeline<Platform>, PipelineType>, "PipelineType must inherit from Pipeline<Platform>");
            BindDescriptorSetsInternal(pipeline, firstSet, descriptorSets);
        }
        
        // 绑定顶点缓冲区
        template<typename BufferType>
        void BindVertexBuffers(u32 startSlot, u32 slotCount, BufferType** buffers, const u64* offsets = nullptr)
        {
            static_assert(std::is_base_of_v<Buffer<Platform>, BufferType>, "BufferType must inherit from Buffer<Platform>");
            BindVertexBuffersInternal(startSlot, slotCount, buffers, offsets);
        }
        
        // 绑定索引缓冲区
        template<typename BufferType>
        void BindIndexBuffer(BufferType* buffer, u64 offset = 0, IndexFormat format = IndexFormat::UInt16)
        {
            static_assert(std::is_base_of_v<Buffer<Platform>, BufferType>, "BufferType must inherit from Buffer<Platform>");
            BindIndexBufferInternal(buffer, offset, format);
        }
        
        // 设置推送常量
        template<typename PipelineType>
        void PushConstants(PipelineType* pipeline, ShaderStageFlags stageFlags, u32 offset, u32 size, const void* data)
        {
            static_assert(std::is_base_of_v<Pipeline<Platform>, PipelineType>, "PipelineType must inherit from Pipeline<Platform>");
            PushConstantsInternal(pipeline, stageFlags, offset, size, data);
        }
        
        // 设置视口
        void SetViewport(u32 x, u32 y, u32 width, u32 height, f32 minDepth = 0.0f, f32 maxDepth = 1.0f)
        {
            SetViewportInternal(x, y, width, height, minDepth, maxDepth);
        }
        
        // 设置裁剪矩形
        void SetScissorRect(u32 x, u32 y, u32 width, u32 height)
        {
            SetScissorRectInternal(x, y, width, height);
        }
        
        // 绘制命令
        void Draw(u32 vertexCount, u32 startVertex = 0)
        {
            DrawInternal(vertexCount, startVertex);
        }
        
        void DrawIndexed(u32 indexCount, u32 startIndex = 0, u32 baseVertex = 0)
        {
            DrawIndexedInternal(indexCount, startIndex, baseVertex);
        }
        
        void DrawInstanced(u32 vertexCount, u32 instanceCount, u32 startVertex = 0, u32 startInstance = 0)
        {
            DrawInstancedInternal(vertexCount, instanceCount, startVertex, startInstance);
        }
        
        void DrawIndexedInstanced(u32 indexCount, u32 instanceCount, u32 startIndex = 0, u32 baseVertex = 0, u32 startInstance = 0)
        {
            DrawIndexedInstancedInternal(indexCount, instanceCount, startIndex, baseVertex, startInstance);
        }
        
        // 计算命令
        void Dispatch(u32 x, u32 y, u32 z)
        {
            DispatchInternal(x, y, z);
        }
        
        // 资源屏障
        void Barrier(const std::vector<ResourceBarrier>& barriers)
        {
            BarrierInternal(barriers);
        }
        
    protected:
        // 平台特定的内部实现
        virtual void BindPipelineInternal(Pipeline<Platform>* pipeline) = 0;
        virtual void BindDescriptorSetsInternal(Pipeline<Platform>* pipeline, u32 firstSet, const std::vector<DescriptorSet<Platform>*>& descriptorSets) = 0;
        virtual void BindVertexBuffersInternal(u32 startSlot, u32 slotCount, Buffer<Platform>** buffers, const u64* offsets) = 0;
        virtual void BindIndexBufferInternal(Buffer<Platform>* buffer, u64 offset, IndexFormat format) = 0;
        virtual void PushConstantsInternal(Pipeline<Platform>* pipeline, ShaderStageFlags stageFlags, u32 offset, u32 size, const void* data) = 0;
        virtual void SetViewportInternal(u32 x, u32 y, u32 width, u32 height, f32 minDepth, f32 maxDepth) = 0;
        virtual void SetScissorRectInternal(u32 x, u32 y, u32 width, u32 height) = 0;
        virtual void DrawInternal(u32 vertexCount, u32 startVertex) = 0;
        virtual void DrawIndexedInternal(u32 indexCount, u32 startIndex, u32 baseVertex) = 0;
        virtual void DrawInstancedInternal(u32 vertexCount, u32 instanceCount, u32 startVertex, u32 startInstance) = 0;
        virtual void DrawIndexedInstancedInternal(u32 indexCount, u32 instanceCount, u32 startIndex, u32 baseVertex, u32 startInstance) = 0;
        virtual void DispatchInternal(u32 x, u32 y, u32 z) = 0;
        virtual void BarrierInternal(const std::vector<ResourceBarrier>& barriers) = 0;
        
        CommandType command_buffer_;
    };
}
```

## 7. 平台特化实现示例

### 7.1 Direct3D12特化实现

```cpp
namespace primal::graphics::rhi {

    // Direct3D12缓冲区实现
    class D3D12Buffer : public Buffer<graphics_platform::direct3d12>
    {
    public:
        D3D12Buffer(ID3D12Device* device, const BufferDesc& desc);
        ~D3D12Buffer() override;
        
        ID3D12Resource* GetNativeResource() const override { return native_buffer_; }
        ResourceType GetType() const override { return ResourceType::Buffer; }
        ResourceState GetState() const override { return state_; }
        void TransitionState(ResourceState newState) override;
        const ResourceDesc& GetDesc() const override { return desc_; }
        
        // Direct3D12特定方法
        D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const;
        u32 GetSize() const { return buffer_desc_.size; }
        
    private:
        void CreateBuffer(ID3D12Device* device, const BufferDesc& desc);
        
        ID3D12Resource* native_buffer_ = nullptr;
        D3D12_RESOURCE_DESC resource_desc_{};
    };

    // Direct3D12图形管线实现
    class D3D12GraphicsPipeline : public GraphicsPipeline<graphics_platform::direct3d12>
    {
    public:
        D3D12GraphicsPipeline(ID3D12Device* device, const GraphicsPipelineDesc& desc);
        ~D3D12GraphicsPipeline() override;
        
        ID3D12PipelineState* GetNativePipeline() const override { return pipeline_state_; }
        const PipelineDesc& GetDesc() const override { return desc_; }
        const std::vector<BindingPoint>& GetBindingPoints() const override { return binding_points_; }
        
        // Direct3D12特定方法
        ID3D12RootSignature* GetRootSignature() const { return root_signature_; }
        
    private:
        void CreateRootSignature(ID3D12Device* device, const GraphicsPipelineDesc& desc);
        void CreatePipelineState(ID3D12Device* device, const GraphicsPipelineDesc& desc);
        
        ID3D12PipelineState* pipeline_state_ = nullptr;
        ID3D12RootSignature* root_signature_ = nullptr;
    };

    // Direct3D12描述符集实现
    class D3D12DescriptorSet : public DescriptorSet<graphics_platform::direct3d12>
    {
    public:
        D3D12DescriptorSet(ID3D12Device* device, DescriptorPool<graphics_platform::direct3d12>* pool, 
                          DescriptorSetLayout<graphics_platform::direct3d12>* layout);
        ~D3D12DescriptorSet() override = default;
        
        void UpdateConstantBuffer(u32 binding, Buffer<graphics_platform::direct3d12>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateTexture(u32 binding, Texture<graphics_platform::direct3d12>* texture, Sampler<graphics_platform::direct3d12>* sampler = nullptr) override;
        void UpdateSampler(u32 binding, Sampler<graphics_platform::direct3d12>* sampler) override;
        void UpdateRWTexture(u32 binding, Texture<graphics_platform::direct3d12>* texture, u32 mipLevel = 0) override;
        void UpdateRWBuffer(u32 binding, Buffer<graphics_platform::direct3d12>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateStructuredBuffer(u32 binding, Buffer<graphics_platform::direct3d12>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateRWStructuredBuffer(u32 binding, Buffer<graphics_platform::direct3d12>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateAccelerationStructure(u32 binding, AccelerationStructure<graphics_platform::direct3d12>* as) override;
        void UpdateDescriptors(const std::vector<DescriptorUpdate>& updates) override;
        
        void* GetNativeDescriptorSet() const override { return nullptr; } // D3D12不使用描述符集对象
        
        // Direct3D12特定方法
        D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandle(u32 binding) const;
        D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandle(u32 binding) const;
        
    private:
        ID3D12Device* device_;
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> cpu_handles_;
        std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> gpu_handles_;
    };
}
```

### 7.2 Vulkan特化实现

```cpp
namespace primal::graphics::rhi {

    // Vulkan缓冲区实现
    class VulkanBuffer : public Buffer<graphics_platform::vulkan_1>
    {
    public:
        VulkanBuffer(VkDevice device, const BufferDesc& desc);
        ~VulkanBuffer() override;
        
        VkImage GetNativeResource() const override { return VK_NULL_HANDLE; } // 缓冲区不是图像
        ResourceType GetType() const override { return ResourceType::Buffer; }
        ResourceState GetState() const override { return state_; }
        void TransitionState(ResourceState newState) override;
        const ResourceDesc& GetDesc() const override { return desc_; }
        
        // Vulkan特定方法
        VkBuffer GetNativeBuffer() const { return buffer_; }
        VkDeviceMemory GetDeviceMemory() const { return memory_; }
        VkDeviceSize GetSize() const { return buffer_desc_.size; }
        
    private:
        void CreateBuffer(VkDevice device, const BufferDesc& desc);
        void AllocateMemory(VkDevice device);
        
        VkBuffer buffer_ = VK_NULL_HANDLE;
        VkDeviceMemory memory_ = VK_NULL_HANDLE;
    };

    // Vulkan图形管线实现
    class VulkanGraphicsPipeline : public GraphicsPipeline<graphics_platform::vulkan_1>
    {
    public:
        VulkanGraphicsPipeline(VkDevice device, const GraphicsPipelineDesc& desc);
        ~VulkanGraphicsPipeline() override;
        
        VkPipeline GetNativePipeline() const override { return pipeline_; }
        const PipelineDesc& GetDesc() const override { return desc_; }
        const std::vector<BindingPoint>& GetBindingPoints() const override { return binding_points_; }
        
        // Vulkan特定方法
        VkPipelineLayout GetPipelineLayout() const { return pipeline_layout_; }
        VkRenderPass GetRenderPass() const { return render_pass_; }
        
    private:
        void CreatePipelineLayout(VkDevice device, const GraphicsPipelineDesc& desc);
        void CreateRenderPass(VkDevice device, const GraphicsPipelineDesc& desc);
        void CreatePipeline(VkDevice device, const GraphicsPipelineDesc& desc);
        
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
        VkRenderPass render_pass_ = VK_NULL_HANDLE;
    };

    // Vulkan描述符集实现
    class VulkanDescriptorSet : public DescriptorSet<graphics_platform::vulkan_1>
    {
    public:
        VulkanDescriptorSet(VkDevice device, DescriptorPool<graphics_platform::vulkan_1>* pool, 
                           DescriptorSetLayout<graphics_platform::vulkan_1>* layout);
        ~VulkanDescriptorSet() override;
        
        void UpdateConstantBuffer(u32 binding, Buffer<graphics_platform::vulkan_1>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateTexture(u32 binding, Texture<graphics_platform::vulkan_1>* texture, Sampler<graphics_platform::vulkan_1>* sampler = nullptr) override;
        void UpdateSampler(u32 binding, Sampler<graphics_platform::vulkan_1>* sampler) override;
        void UpdateRWTexture(u32 binding, Texture<graphics_platform::vulkan_1>* texture, u32 mipLevel = 0) override;
        void UpdateRWBuffer(u32 binding, Buffer<graphics_platform::vulkan_1>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateStructuredBuffer(u32 binding, Buffer<graphics_platform::vulkan_1>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateRWStructuredBuffer(u32 binding, Buffer<graphics_platform::vulkan_1>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateAccelerationStructure(u32 binding, AccelerationStructure<graphics_platform::vulkan_1>* as) override;
        void UpdateDescriptors(const std::vector<DescriptorUpdate>& updates) override;
        
        void* GetNativeDescriptorSet() const override { return reinterpret_cast<void*>(descriptor_set_); }
        
        // Vulkan特定方法
        VkDescriptorSet GetVkDescriptorSet() const { return descriptor_set_; }
        
    private:
        VkDevice device_;
        VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
        std::vector<VkWriteDescriptorSet> pending_writes_;
        std::vector<VkDescriptorBufferInfo> buffer_infos_;
        std::vector<VkDescriptorImageInfo> image_infos_;
    };
}
```

### 7.3 Metal特化实现

```cpp
namespace primal::graphics::rhi {

    // Metal缓冲区实现
    class MetalBuffer : public Buffer<graphics_platform::metal>
    {
    public:
        MetalBuffer(MTL::Device* device, const BufferDesc& desc);
        ~MetalBuffer() override;
        
        MTL::Resource* GetNativeResource() const override { return buffer_; }
        ResourceType GetType() const override { return ResourceType::Buffer; }
        ResourceState GetState() const override { return state_; }
        void TransitionState(ResourceState newState) override;
        const ResourceDesc& GetDesc() const override { return desc_; }
        
        // Metal特定方法
        MTL::Buffer* GetMTLBuffer() const { return buffer_; }
        NSUInteger GetLength() const { return buffer_desc_.size; }
        
    private:
        MTL::Buffer* buffer_ = nullptr;
    };

    // Metal图形管线实现
    class MetalGraphicsPipeline : public GraphicsPipeline<graphics_platform::metal>
    {
    public:
        MetalGraphicsPipeline(MTL::Device* device, const GraphicsPipelineDesc& desc);
        ~MetalGraphicsPipeline() override;
        
        MTL::RenderPipelineState* GetNativePipeline() const override { return pipeline_state_; }
        const PipelineDesc& GetDesc() const override { return desc_; }
        const std::vector<BindingPoint>& GetBindingPoints() const override { return binding_points_; }
        
        // Metal特定方法
        MTL::RenderPipelineReflection* GetReflection() const { return reflection_; }
        
    private:
        void CreatePipelineState(MTL::Device* device, const GraphicsPipelineDesc& desc);
        
        MTL::RenderPipelineState* pipeline_state_ = nullptr;
        MTL::RenderPipelineReflection* reflection_ = nullptr;
    };

    // Metal描述符集实现（Metal使用参数缓冲区）
    class MetalDescriptorSet : public DescriptorSet<graphics_platform::metal>
    {
    public:
        MetalDescriptorSet(MTL::Device* device, DescriptorPool<graphics_platform::metal>* pool, 
                          DescriptorSetLayout<graphics_platform::metal>* layout);
        ~MetalDescriptorSet() override;
        
        void UpdateConstantBuffer(u32 binding, Buffer<graphics_platform::metal>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateTexture(u32 binding, Texture<graphics_platform::metal>* texture, Sampler<graphics_platform::metal>* sampler = nullptr) override;
        void UpdateSampler(u32 binding, Sampler<graphics_platform::metal>* sampler) override;
        void UpdateRWTexture(u32 binding, Texture<graphics_platform::metal>* texture, u32 mipLevel = 0) override;
        void UpdateRWBuffer(u32 binding, Buffer<graphics_platform::metal>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateStructuredBuffer(u32 binding, Buffer<graphics_platform::metal>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateRWStructuredBuffer(u32 binding, Buffer<graphics_platform::metal>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateAccelerationStructure(u32 binding, AccelerationStructure<graphics_platform::metal>* as) override;
        void UpdateDescriptors(const std::vector<DescriptorUpdate>& updates) override;
        
        void* GetNativeDescriptorSet() const override { return nullptr; } // Metal不使用描述符集对象
        
        // Metal特定方法
        MTL::Buffer* GetArgumentBuffer() const { return argument_buffer_; }
        void EncodeArgumentBuffer(MTL::CommandBuffer* commandBuffer);
        
    private:
        MTL::Device* device_;
        MTL::Buffer* argument_buffer_ = nullptr;
        std::vector<MTL::Resource*> bound_resources_;
    };
}
```

### 7.4 WebGPU特化实现

```cpp
namespace primal::graphics::rhi {

    // WebGPU缓冲区实现
    class WebGPUBuffer : public Buffer<graphics_platform::webgpu>
    {
    public:
        WebGPUBuffer(WGPUDevice device, const BufferDesc& desc);
        ~WebGPUBuffer() override;
        
        WGPUResource GetNativeResource() const override { return reinterpret_cast<WGPUResource>(buffer_); }
        ResourceType GetType() const override { return ResourceType::Buffer; }
        ResourceState GetState() const override { return state_; }
        void TransitionState(ResourceState newState) override;
        const ResourceDesc& GetDesc() const override { return desc_; }
        
        // WebGPU特定方法
        WGPUBuffer GetNativeBuffer() const { return buffer_; }
        u64 GetSize() const { return buffer_desc_.size; }
        
    private:
        void CreateBuffer(WGPUDevice device, const BufferDesc& desc);
        
        WGPUBuffer buffer_ = nullptr;
        WGPUBufferUsageFlags usage_ = 0;
    };

    // WebGPU图形管线实现
    class WebGPUGraphicsPipeline : public GraphicsPipeline<graphics_platform::webgpu>
    {
    public:
        WebGPUGraphicsPipeline(WGPUDevice device, const GraphicsPipelineDesc& desc);
        ~WebGPUGraphicsPipeline() override;
        
        WGPURenderPipeline GetNativePipeline() const override { return pipeline_; }
        const PipelineDesc& GetDesc() const override { return desc_; }
        const std::vector<BindingPoint>& GetBindingPoints() const override { return binding_points_; }
        
        // WebGPU特定方法
        WGPUPipelineLayout GetPipelineLayout() const { return pipeline_layout_; }
        
    private:
        void CreatePipelineLayout(WGPUDevice device, const GraphicsPipelineDesc& desc);
        void CreateRenderPipeline(WGPUDevice device, const GraphicsPipelineDesc& desc);
        
        WGPURenderPipeline pipeline_ = nullptr;
        WGPUPipelineLayout pipeline_layout_ = nullptr;
    };

    // WebGPU描述符集实现（WebGPU使用绑定组）
    class WebGPUDescriptorSet : public DescriptorSet<graphics_platform::webgpu>
    {
    public:
        WebGPUDescriptorSet(WGPUDevice device, DescriptorPool<graphics_platform::webgpu>* pool, 
                           DescriptorSetLayout<graphics_platform::webgpu>* layout);
        ~WebGPUDescriptorSet() override;
        
        void UpdateConstantBuffer(u32 binding, Buffer<graphics_platform::webgpu>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateTexture(u32 binding, Texture<graphics_platform::webgpu>* texture, Sampler<graphics_platform::webgpu>* sampler = nullptr) override;
        void UpdateSampler(u32 binding, Sampler<graphics_platform::webgpu>* sampler) override;
        void UpdateRWTexture(u32 binding, Texture<graphics_platform::webgpu>* texture, u32 mipLevel = 0) override;
        void UpdateRWBuffer(u32 binding, Buffer<graphics_platform::webgpu>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateStructuredBuffer(u32 binding, Buffer<graphics_platform::webgpu>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateRWStructuredBuffer(u32 binding, Buffer<graphics_platform::webgpu>* buffer, u64 offset = 0, u64 range = 0) override;
        void UpdateAccelerationStructure(u32 binding, AccelerationStructure<graphics_platform::webgpu>* as) override;
        void UpdateDescriptors(const std::vector<DescriptorUpdate>& updates) override;
        
        void* GetNativeDescriptorSet() const override { return reinterpret_cast<void*>(bind_group_); }
        
        // WebGPU特定方法
        WGPUBindGroup GetBindGroup() const { return bind_group_; }
        
    private:
        WGPUDevice device_;
        WGPUBindGroup bind_group_ = nullptr;
        std::vector<WGPUBindGroupEntry> bind_entries_;
    };
}
```

## 8. 使用示例

### 8.1 资源创建示例

```cpp
// 获取当前平台的资源工厂
auto factory = GetCurrentResourceFactory();

// 创建缓冲区
BufferDesc vertexBufferDesc{};
vertexBufferDesc.size = 1024 * 1024; // 1MB
vertexBufferDesc.usage = BufferUsage::Vertex;
vertexBufferDesc.memoryType = MemoryType::DeviceLocal;

auto vertexBuffer = CreateBuffer<CurrentPlatform>(factory, vertexBufferDesc);

// 创建纹理
TextureDesc textureDesc{};
textureDesc.width = 1024;
textureDesc.height = 1024;
textureDesc.depth = 1;
textureDesc.format = TextureFormat::RGBA8_UNorm;
textureDesc.usage = TextureUsage::ShaderRead | TextureUsage::RenderTarget;
textureDesc.type = TextureType::Texture2D;

auto texture = CreateTexture<CurrentPlatform>(factory, textureDesc);

// 创建图形管线
GraphicsPipelineDesc pipelineDesc{};
pipelineDesc.vertexShader = vertexShader;
pipelineDesc.pixelShader = pixelShader;
pipelineDesc.vertexInputLayout = vertexInputLayout;
pipelineDesc.renderState = renderState;
pipelineDesc.renderTargetFormats = { TextureFormat::RGBA8_UNorm };
pipelineDesc.depthStencilFormat = TextureFormat::D32_Float;

auto graphicsPipeline = CreateGraphicsPipeline<CurrentPlatform>(factory, pipelineDesc);
```

### 8.2 资源绑定示例

```cpp
// 创建资源绑定器
ResourceBinder<CurrentPlatform> binder(factory);

// 创建描述符池
std::vector<std::pair<DescriptorType, u32>> poolSizes = {
    { DescriptorType::ConstantBuffer, 10 },
    { DescriptorType::Texture, 20 },
    { DescriptorType::Sampler, 5 }
};

auto descriptorPool = factory->CreateDescriptorPool(poolSizes);

// 创建描述符集布局
std::vector<BindingPoint> bindingPoints = {
    { 0, 1, DescriptorType::ConstantBuffer, ShaderStageFlags::Vertex },
    { 1, 1, DescriptorType::Texture, ShaderStageFlags::Pixel },
    { 2, 1, DescriptorType::Sampler, ShaderStageFlags::Pixel }
};

auto descriptorSetLayout = factory->CreateDescriptorSetLayout(bindingPoints);

// 创建描述符集
auto descriptorSet = binder.CreateDescriptorSet(descriptorPool, descriptorSetLayout);

// 绑定资源
binder.BindConstantBuffer(descriptorSet, 0, constantBuffer);
binder.BindTexture(descriptorSet, 1, texture, sampler);
binder.BindSampler(descriptorSet, 2, sampler);
```

### 8.3 命令缓冲区记录示例

```cpp
// 获取命令缓冲区
auto commandBuffer = GetCurrentCommandBuffer();

// 创建命令缓冲区绑定器
CommandBufferBinder<CurrentPlatform> cmdBinder(commandBuffer);

// 绑定管线
cmdBinder.BindPipeline(graphicsPipeline);

// 绑定描述符集
std::vector<DescriptorSet<CurrentPlatform>*> descriptorSets = { descriptorSet };
cmdBinder.BindDescriptorSets(graphicsPipeline, 0, descriptorSets);

// 绑定顶点和索引缓冲区
Buffer<CurrentPlatform>* vertexBuffers[] = { vertexBuffer };
u64 offsets[] = { 0 };
cmdBinder.BindVertexBuffers(0, 1, vertexBuffers, offsets);
cmdBinder.BindIndexBuffer(indexBuffer);

// 设置视口和裁剪矩形
cmdBinder.SetViewport(0, 0, 1920, 1080);
cmdBinder.SetScissorRect(0, 0, 1920, 1080);

// 绘制
cmdBinder.DrawIndexed(indexCount);
```

## 9. 性能优化

### 9.1 编译时平台选择

```cpp
// 使用模板和constexpr实现编译时平台选择
template<graphics_platform Platform>
class RHISystem
{
public:
    using FactoryType = ResourceFactory<Platform>;
    using BufferType = Buffer<Platform>;
    using TextureType = Texture<Platform>;
    using PipelineType = Pipeline<Platform>;
    using DescriptorSetType = DescriptorSet<Platform>;
    
    // ...
};

// 编译时平台特化
using CurrentRHISystem = RHISystem<CurrentPlatform>;
```

### 9.2 资源缓存

```cpp
// 资源缓存模板
template<graphics_platform Platform, typename ResourceType>
class ResourceCache
{
public:
    using KeyType = typename ResourceType::KeyType;
    using ValueType = std::unique_ptr<ResourceType>;
    
    ValueType GetOrCreate(const KeyType& key, std::function<ValueType()> factory)
    {
        auto it = cache_.find(key);
        if (it != cache_.end())
        {
            return ValueType(it->second->Clone());
        }
        
        auto resource = factory();
        cache_[key] = resource.get();
        return resource;
    }
    
    void Clear()
    {
        cache_.clear();
    }
    
private:
    std::unordered_map<KeyType, ResourceType*> cache_;
};
```

### 9.3 批量操作

```cpp
// 批量资源更新
template<graphics_platform Platform>
class BatchResourceUpdater
{
public:
    void AddBufferUpdate(Buffer<Platform>* buffer, const void* data, u64 size, u64 offset = 0)
    {
        BufferUpdate update;
        update.buffer = buffer;
        update.data = data;
        update.size = size;
        update.offset = offset;
        buffer_updates_.push_back(update);
    }
    
    void AddTextureUpdate(Texture<Platform>* texture, const void* data, u32 width, u32 height, u32 mipLevel = 0)
    {
        TextureUpdate update;
        update.texture = texture;
        update.data = data;
        update.width = width;
        update.height = height;
        update.mipLevel = mipLevel;
        texture_updates_.push_back(update);
    }
    
    void Execute()
    {
        // 执行批量更新
        ExecuteBufferUpdates();
        ExecuteTextureUpdates();
        
        // 清空更新列表
        buffer_updates_.clear();
        texture_updates_.clear();
    }
    
private:
    struct BufferUpdate
    {
        Buffer<Platform>* buffer;
        const void* data;
        u64 size;
        u64 offset;
    };
    
    struct TextureUpdate
    {
        Texture<Platform>* texture;
        const void* data;
        u32 width;
        u32 height;
        u32 mipLevel;
    };
    
    std::vector<BufferUpdate> buffer_updates_;
    std::vector<TextureUpdate> texture_updates_;
    
    void ExecuteBufferUpdates();
    void ExecuteTextureUpdates();
};
```

## 10. 总结

本增强版RHI层封装方案提供了以下优势：

1. **模板化设计**：使用C++模板技术实现跨平台的统一接口，支持编译时平台选择
2. **类型安全**：通过模板特化和类型约束确保类型安全
3. **高性能**：最小化运行时开销，尽可能在编译时解析平台特定代码
4. **易扩展**：通过模板特化轻松添加新的图形API支持
5. **统一接口**：为不同图形API的资源创建和绑定提供统一接口
6. **便捷使用**：提供简洁的API，减少样板代码

该方案特别适合需要高性能、低延迟的游戏引擎，同时保持良好的跨平台兼容性和易用性。通过模板技术，开发者可以编写一次代码，然后在多个平台上编译运行，而无需关心底层图形API的差异。

## 11. 现有结构集成与复用

### 11.1 现有图形API结构分析

通过对现有引擎代码的分析，我们发现了以下可复用的核心组件：

#### 11.1.1 平台接口抽象

现有的`platform_interface`结构（GraphicsPlatformInterface.h）已经提供了良好的跨平台抽象基础：

```cpp
namespace primal::graphics {
    struct platform_interface
    {
        bool(*initialize)(void);
        void(*shutdown)(void);
        
        struct {
            surface(*create)(platform::window);
            void(*remove)(surface_id);
            void(*resize)(surface_id, u32, u32);
            u32(*width)(surface_id);
            u32(*height)(surface_id);
            void(*render)(surface_id, frame_info);
        } surface;
        
        struct {
            void(*create_light_set)(u64);
            void(*remove_light_set)(u64);
            light(*create)(light_init_info);
            void(*remove)(light_id, u64);
            void(*set_parameter)(light_id, u64, light_parameter::parameter, const void *const, u32);
            void(*get_parameter)(light_id, u64, light_parameter::parameter, void *const, u32);
        } light;
        
        struct {
            camera(*create)(camera_init_info);
            void(*remove)(camera_id);
            void(*set_parameter)(camera_id, camera_parameter::parameter, const void *const, u32);
            void(*get_parameter)(camera_id, camera_parameter::parameter, void *const, u32);
        } camera;
        
        struct {
            id::id_type(*add_submesh)(const u8*&);
            void (*remove_submesh)(id::id_type);
            id::id_type(*add_texture)(const u8 *const);
            void (*remove_texture)(id::id_type);
            id::id_type(*add_material)(material_init_info);
            void (*remove_material)(id::id_type);
            id::id_type(*add_render_item)(id::id_type, id::id_type, u32, const id::id_type *const);
            void(*remove_render_item)(id::id_type);
        } resources;
        
        graphics_platform platform = (graphics_platform) - 1;
    };
}
```

这个结构已经提供了跨平台的统一接口，可以作为RHI层设计的基础。

#### 11.1.2 平台枚举

现有的`graphics_platform`枚举（GraphicsPlatform.h）已经定义了支持的图形平台：

```cpp
enum class graphics_platform : u32
{
    direct3d12 = 0,
    vulkan_1,
    metal,
    webgpu,
};
```

这与我们的RHI层设计中的平台枚举完全一致，可以直接复用。

#### 11.1.3 平台选择机制

现有的`set_platform_interface`函数（GraphicsPlatform.cpp）已经实现了平台选择机制：

```cpp
bool set_platform_interface(graphics_platform platform, platform_interface& pi)
{
    switch (platform)
    {
#ifndef __APPLE__
    case graphics_platform::direct3d12:
        d3d12::get_platform_interface(pi);
        break;
    case graphics_platform::vulkan_1:
        vulkan::get_platform_interface(pi);
        break;
#endif
    case graphics_platform::metal:
        metal::get_platform_interface(pi);
        break;
    default:
        return false;
    }

    assert(pi.platform == platform);
    return true;
}
```

这个机制可以扩展为我们的RHI层模板特化选择机制。

#### 11.1.4 资源管理结构

各平台已经实现了自己的资源管理结构：

**Direct3D12资源管理**（D3D12Resources.h）：
- `d3d12_buffer`：缓冲区管理
- `d3d12_texture`：纹理管理
- `d3d12_render_texture`：渲染目标纹理管理
- `d3d12_depth_buffer`：深度缓冲区管理
- `descriptor_heap`：描述符堆管理
- `constant_buffer`：常量缓冲区管理

**Metal资源管理**（MetalResource.h）：
- `metal_buffer`：缓冲区管理
- `metal_texture`：纹理管理
- `metal_render_texture`：渲染目标纹理管理
- `constant_buffer`：常量缓冲区管理

**Vulkan资源管理**（VulkanResources.h）：
- `vulkan_image`：图像/纹理管理
- `vulkan_framebuffer`：帧缓冲区管理

这些现有的资源管理结构可以作为我们RHI层资源基类的平台特化实现基础。

#### 11.1.5 核心初始化和管理

各平台的核心初始化和管理函数：

**Direct3D12核心**（D3D12Core.h）：
- `initialize()`/`shutdown()`：初始化和关闭
- `device()`：获取设备
- `rtv_heap()`/`dsv_heap()`/`srv_heap()`/`uav_heap()`：获取各种描述符堆
- `create_surface()`/`remove_surface()`/`resize_surface()`：表面管理

**Metal核心**（MetalCore.h）：
- `initialize()`/`shutdown()`：初始化和关闭
- `get_device()`：获取设备
- `create_surface()`/`remove_surface()`/`resize_surface()`：表面管理

**Vulkan核心**（VulkanCore.h）：
- `initialize()`/`shutdown()`：初始化和关闭
- `create_device()`：设备创建
- `create_surface()`/`remove_surface()`/`resize_surface()`：表面管理

### 11.2 RHI层与现有结构的集成方案

#### 11.2.1 平台特化集成

我们可以将现有的平台接口扩展为RHI层的模板特化：

```cpp
namespace primal::graphics::rhi {
    // 扩展现有的platform_interface为模板特化
    template<graphics_platform Platform>
    struct PlatformInterface;
    
    // Direct3D12特化
    template<>
    struct PlatformInterface<graphics_platform::direct3d12>
    {
        static bool Initialize()
        {
            return d3d12::core::initialize();
        }
        
        static void Shutdown()
        {
            d3d12::core::shutdown();
        }
        
        // 获取设备
        static ID3D12Device* GetDevice()
        {
            return d3d12::core::device();
        }
        
        // 其他Direct3D12特定方法...
    };
    
    // Metal特化
    template<>
    struct PlatformInterface<graphics_platform::metal>
    {
        static bool Initialize()
        {
            return metal::core::initialize();
        }
        
        static void Shutdown()
        {
            metal::core::shutdown();
        }
        
        // 获取设备
        static MTL::Device* GetDevice()
        {
            return metal::core::get_device();
        }
        
        // 其他Metal特定方法...
    };
    
    // Vulkan特化
    template<>
    struct PlatformInterface<graphics_platform::vulkan_1>
    {
        static bool Initialize()
        {
            return vulkan::core::initialize();
        }
        
        static void Shutdown()
        {
            vulkan::core::shutdown();
        }
        
        // 获取设备
        static VkDevice GetDevice()
        {
            return vulkan::core::logical_device();
        }
        
        // 其他Vulkan特定方法...
    };
}
```

#### 11.2.2 资源管理集成

将现有的资源管理类包装为RHI层的资源基类：

```cpp
namespace primal::graphics::rhi {
    // Direct3D12缓冲区包装
    template<>
    class Buffer<graphics_platform::direct3d12> : public BufferBase
    {
    public:
        explicit Buffer(ID3D12Device* device, const BufferDesc& desc)
        {
            // 使用现有的d3d12_buffer创建逻辑
            d3d12_buffer_init_info info{};
            info.size = desc.size;
            // 设置其他参数...
            
            buffer_ = std::make_unique<d3d12::d3d12_buffer>(info, desc.cpuAccessible);
        }
        
        ID3D12Resource* GetNativeResource() const override 
        { 
            return buffer_->buffer(); 
        }
        
        ResourceType GetType() const override 
        { 
            return ResourceType::Buffer; 
        }
        
        // 其他方法实现...
        
    private:
        std::unique_ptr<d3d12::d3d12_buffer> buffer_;
    };
    
    // Metal缓冲区包装
    template<>
    class Buffer<graphics_platform::metal> : public BufferBase
    {
    public:
        explicit Buffer(MTL::Device* device, const BufferDesc& desc)
        {
            // 使用现有的metal_buffer创建逻辑
            metal_buffer_init_info info{};
            info.size = desc.size;
            // 设置其他参数...
            
            buffer_ = std::make_unique<metal::metal_buffer>(info, desc.cpuAccessible);
        }
        
        MTL::Resource* GetNativeResource() const override 
        { 
            return buffer_->buffer(); 
        }
        
        ResourceType GetType() const override 
        { 
            return ResourceType::Buffer; 
        }
        
        // 其他方法实现...
        
    private:
        std::unique_ptr<metal::metal_buffer> buffer_;
    };
    
    // Vulkan缓冲区包装
    template<>
    class Buffer<graphics_platform::vulkan_1> : public BufferBase
    {
    public:
        explicit Buffer(VkDevice device, const BufferDesc& desc)
        {
            // 使用现有的Vulkan缓冲区创建逻辑
            // 创建Vulkan缓冲区和内存...
        }
        
        VkImage GetNativeResource() const override 
        { 
            return VK_NULL_HANDLE; // 缓冲区不是图像
        }
        
        ResourceType GetType() const override 
        { 
            return ResourceType::Buffer; 
        }
        
        // 其他方法实现...
        
    private:
        VkBuffer buffer_ = VK_NULL_HANDLE;
        VkDeviceMemory memory_ = VK_NULL_HANDLE;
    };
}
```

#### 11.2.3 描述符管理集成

将现有的描述符管理集成到RHI层的描述符系统：

```cpp
namespace primal::graphics::rhi {
    // Direct3D12描述符集包装
    template<>
    class DescriptorSet<graphics_platform::direct3d12> : public DescriptorSetBase
    {
    public:
        DescriptorSet(ID3D12Device* device, DescriptorPool<graphics_platform::direct3d12>* pool, 
                      DescriptorSetLayout<graphics_platform::direct3d12>* layout)
        {
            // 使用现有的descriptor_heap创建描述符集
            auto& srv_heap = d3d12::core::srv_heap();
            auto& uav_heap = d3d12::core::uav_heap();
            
            // 分配描述符...
        }
        
        void UpdateConstantBuffer(u32 binding, Buffer<graphics_platform::direct3d12>* buffer, 
                                 u64 offset = 0, u64 range = 0) override
        {
            // 使用现有的D3D12描述符更新逻辑
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc{};
            cbv_desc.BufferLocation = buffer->GetGPUAddress() + offset;
            cbv_desc.SizeInBytes = (range > 0) ? range : buffer->GetSize();
            
            device_->CreateConstantBufferView(&cbv_desc, cpu_handle_);
        }
        
        // 其他更新方法...
        
    private:
        ID3D12Device* device_;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle_;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_;
    };
    
    // Metal描述符集包装（使用参数缓冲区）
    template<>
    class DescriptorSet<graphics_platform::metal> : public DescriptorSetBase
    {
    public:
        DescriptorSet(MTL::Device* device, DescriptorPool<graphics_platform::metal>* pool, 
                      DescriptorSetLayout<graphics_platform::metal>* layout)
        {
            // 创建Metal参数缓冲区
            MTL::ArgumentDescriptor* arg_desc = MTL::ArgumentDescriptor::argumentDescriptor();
            // 设置参数描述...
            
            argument_buffer_ = device->newBuffer(length, MTL::ResourceStorageModeShared);
        }
        
        void UpdateConstantBuffer(u32 binding, Buffer<graphics_platform::metal>* buffer, 
                                 u64 offset = 0, u64 range = 0) override
        {
            // 使用Metal参数缓冲区更新逻辑
            // 设置参数缓冲区中的指针...
        }
        
        // 其他更新方法...
        
    private:
        MTL::Device* device_;
        MTL::Buffer* argument_buffer_;
    };
}
```

#### 11.2.4 表面管理集成

将现有的表面管理集成到RHI层的表面系统：

```cpp
namespace primal::graphics::rhi {
    // Direct3D12表面包装
    template<>
    class Surface<graphics_platform::direct3d12> : public SurfaceBase
    {
    public:
        explicit Surface(platform::window window)
        {
            // 使用现有的表面创建逻辑
            surface_id_ = d3d12::core::create_surface(window);
        }
        
        ~Surface()
        {
            if (surface_id_.is_valid())
            {
                d3d12::core::remove_surface(surface_id_);
            }
        }
        
        void Resize(u32 width, u32 height) override
        {
            d3d12::core::resize_surface(surface_id_, width, height);
        }
        
        u32 GetWidth() const override
        {
            return d3d12::core::surface_width(surface_id_);
        }
        
        u32 GetHeight() const override
        {
            return d3d12::core::surface_height(surface_id_);
        }
        
        void Render(const frame_info& info) override
        {
            d3d12::core::render_surface(surface_id_, info);
        }
        
    private:
        surface_id surface_id_;
    };
    
    // Metal表面包装
    template<>
    class Surface<graphics_platform::metal> : public SurfaceBase
    {
    public:
        explicit Surface(platform::window window)
        {
            // 使用现有的表面创建逻辑
            surface_id_ = metal::core::create_surface(window);
        }
        
        ~Surface()
        {
            if (surface_id_.is_valid())
            {
                metal::core::remove_surface(surface_id_);
            }
        }
        
        void Resize(u32 width, u32 height) override
        {
            metal::core::resize_surface(surface_id_, width, height);
        }
        
        u32 GetWidth() const override
        {
            return metal::core::surface_width(surface_id_);
        }
        
        u32 GetHeight() const override
        {
            return metal::core::surface_height(surface_id_);
        }
        
        void Render(const frame_info& info) override
        {
            metal::core::render_surface(surface_id_, info);
        }
        
    private:
        surface_id surface_id_;
    };
}
```

### 11.3 迁移策略

#### 11.3.1 渐进式迁移

1. **第一阶段**：创建RHI层模板接口，同时保持现有接口不变
2. **第二阶段**：将现有平台特定实现包装为RHI层模板特化
3. **第三阶段**：逐步将上层代码迁移到RHI层接口
4. **第四阶段**：移除旧的接口实现

#### 11.3.2 兼容性保证

在迁移过程中，可以通过适配器模式保持现有代码的兼容性：

```cpp
namespace primal::graphics {
    // 兼容性适配器
    class LegacyGraphicsAdapter
    {
    public:
        static bool Initialize(graphics_platform platform)
        {
            // 使用RHI层初始化
            return RHISystem::Initialize(platform);
        }
        
        static surface create_surface(platform::window window)
        {
            // 使用RHI层创建表面
            auto rhi_surface = RHISystem::CreateSurface(window);
            return surface(rhi_surface->GetId());
        }
        
        // 其他适配方法...
    };
}
```

### 11.4 优势与收益

通过集成现有结构，我们的RHI层设计可以获得以下优势：

1. **减少重复开发**：复用现有的平台特定实现，减少开发工作量
2. **降低风险**：基于已经验证的代码，降低引入新bug的风险
3. **平滑迁移**：通过渐进式迁移，减少对现有代码的影响
4. **保持一致性**：与现有引擎架构保持一致，便于维护
5. **利用现有优化**：复用现有的性能优化和平台特定优化

这种集成方案使得RHI层设计不仅提供了统一的模板化接口，还能够充分利用现有的代码基础，实现平滑的迁移和升级。

## 12. WebGPU支持与Dawn库集成

### 12.1 WebGPU概述

WebGPU是下一代Web图形API，提供了现代GPU功能的低级访问。它设计为Vulkan、Metal和Direct3D12的上层抽象，使开发者能够编写一次代码并在多个平台上运行。在我们的游戏引擎中，通过集成Dawn库（Google的WebGPU实现），我们可以将WebGPU作为另一个图形后端，实现真正的跨平台渲染。

### 12.2 Dawn库集成

#### 12.2.1 Dawn库简介

Dawn是Google开发的WebGPU开源实现，作为Chromium的WebGPU后端。它提供了：
- 跨平台WebGPU实现（Windows、Linux、macOS）
- 原生后端支持（Direct3D12、Vulkan、Metal）
- 详细的验证层和调试工具
- 高效的GPU资源管理

#### 12.2.2 集成步骤

1. **获取Dawn库**：
   
   **方法一：手动构建（推荐）**
   ```bash
   # 克隆Dawn仓库
   git clone https://dawn.googlesource.com/dawn
   cd dawn
   
   # 设置gclient配置
   cp scripts/standalone.gclient client.gclient
   
   # 同步依赖
   gclient sync
   
   # 生成构建文件（Release版本）
   gn gen out/Release --args='is_debug=false target_cpu="x64" dawn_build_tests=false dawn_build_samples=false'
   
   # 构建Dawn
   ninja -C out/Release
   
   # 生成构建文件（Debug版本，可选）
   gn gen out/Debug --args='is_debug=true target_cpu="x64" dawn_build_tests=false dawn_build_samples=false'
   
   # 构建Dawn Debug版本
   ninja -C out/Debug
   ```
   
   **方法二：使用CMake自动构建（可选）**
   ```bash
   # 在项目根目录下创建ThirdParty目录
   mkdir -p ThirdParty
   cd ThirdParty
   
   # 克隆Dawn仓库
   git clone https://dawn.googlesource.com/dawn
   
   # 返回项目根目录
   cd ..
   
   # 配置CMake，启用Dawn自动构建
   cmake -B build -S . -DBUILD_DAWN=ON -DENABLE_WEBGPU=ON
   
   # 构建项目（会自动构建Dawn）
   cmake --build build --config Release
   ```
   
   **方法三：使用预构建库（如果可用）**
   ```bash
   # 下载预构建库（假设提供）
   # 将库文件放置在ThirdParty/Dawn/lib目录下
   # 将头文件放置在ThirdParty/Dawn/include目录下
   ```

2. **CMake集成**：
   
   **2.1 基础CMake配置**
   ```cmake
   # 添加Dawn库路径
   set(DAWN_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Dawn")
   
   # 设置Dawn构建目录
   set(DAWN_BUILD_DIR "${DAWN_ROOT_DIR}/out")
   
   # 根据构建类型选择输出目录
   if(CMAKE_BUILD_TYPE STREQUAL "Debug")
       set(DAWN_OUTPUT_DIR "${DAWN_BUILD_DIR}/Debug")
   else()
       set(DAWN_OUTPUT_DIR "${DAWN_BUILD_DIR}/Release")
   endif()
   
   # 查找Dawn库
   find_library(DAWN_WEBGPU_LIB 
       NAMES webgpu_dawn
       PATHS "${DAWN_OUTPUT_DIR}/obj"
       NO_DEFAULT_PATH
   )
   
   find_library(DAWN_NATIVE_LIB 
       NAMES dawn_native
       PATHS "${DAWN_OUTPUT_DIR}/obj"
       NO_DEFAULT_PATH
   )
   
   # 检查库是否找到
   if(NOT DAWN_WEBGPU_LIB OR NOT DAWN_NATIVE_LIB)
       message(WARNING "Dawn libraries not found. Please build Dawn first.")
   endif()
   
   # 添加到项目中
   target_link_libraries(${PROJECT_NAME} 
       PRIVATE 
       ${DAWN_WEBGPU_LIB}
       ${DAWN_NATIVE_LIB}
   )
   
   # 包含目录
   target_include_directories(${PROJECT_NAME} 
       PRIVATE 
       "${DAWN_ROOT_DIR}/include"
       "${DAWN_ROOT_DIR}/gen/include"
   )
   ```
   
   **2.2 平台特定配置**
   ```cmake
   # Windows平台特定配置
   if(WIN32)
       # 添加DirectX Shader Compiler依赖
       find_package(DirectXShaderCompiler REQUIRED)
       target_link_libraries(${PROJECT_NAME} 
           PRIVATE 
           DirectXShaderCompiler::dxcompiler
       )
       
       # 设置Dawn后端为Direct3D12
       target_compile_definitions(${PROJECT_NAME} 
           PRIVATE 
           DAWN_ENABLE_BACKEND_D3D12
       )
       
       # 添加Windows特定库
       target_link_libraries(${PROJECT_NAME} 
           PRIVATE 
           d3d12
           dxgi
           dxguid
       )
   endif()
   
   # macOS平台特定配置
   if(APPLE)
       # 设置Dawn后端为Metal
       target_compile_definitions(${PROJECT_NAME} 
           PRIVATE 
           DAWN_ENABLE_BACKEND_METAL
       )
       
       # 添加Metal框架
       target_link_libraries(${PROJECT_NAME} 
           PRIVATE 
           "-framework Metal"
           "-framework QuartzCore"
       )
       
       # 设置最低macOS版本
       set_target_properties(${PROJECT_NAME} PROPERTIES
           MACOSX_DEPLOYMENT_TARGET "10.15"
       )
   endif()
   
   # Linux平台特定配置
   if(UNIX AND NOT APPLE)
       # 设置Dawn后端为Vulkan
       target_compile_definitions(${PROJECT_NAME} 
           PRIVATE 
           DAWN_ENABLE_BACKEND_VULKAN
       )
       
       # 查找Vulkan
       find_package(Vulkan REQUIRED)
       target_link_libraries(${PROJECT_NAME} 
           PRIVATE 
           Vulkan::Vulkan
       )
       
       # 添加X11依赖
       find_package(X11 REQUIRED)
       target_link_libraries(${PROJECT_NAME} 
           PRIVATE 
           ${X11_LIBRARIES}
       )
   endif()
   ```
   
   **2.3 构建选项配置**
   ```cmake
   # Dawn构建选项
   option(ENABLE_WEBGPU "Enable WebGPU support via Dawn" ON)
   option(ENABLE_WEBGPU_VALIDATION "Enable WebGPU validation layer" ON)
   option(ENABLE_WEBGPU_DEBUG_UTILS "Enable WebGPU debug utils" ON)
   
   if(ENABLE_WEBGPU)
       # 添加WebGPU支持
       target_compile_definitions(${PROJECT_NAME} 
           PRIVATE 
           PRIMAL_ENABLE_WEBGPU
       )
       
       if(ENABLE_WEBGPU_VALIDATION)
           target_compile_definitions(${PROJECT_NAME} 
               PRIVATE 
               PRIMAL_ENABLE_WEBGPU_VALIDATION
           )
       endif()
       
       if(ENABLE_WEBGPU_DEBUG_UTILS)
           target_compile_definitions(${PROJECT_NAME} 
               PRIVATE 
               PRIMAL_ENABLE_WEBGPU_DEBUG_UTILS
           )
       endif()
   endif()
   ```
   
   **2.4 自动构建Dawn（可选）**
   ```cmake
   # 自动构建Dawn（如果未找到预构建库）
   option(BUILD_DAWN "Build Dawn from source" OFF)
   
   if(BUILD_DAWN AND (NOT DAWN_WEBGPU_LIB OR NOT DAWN_NATIVE_LIB))
       message(STATUS "Building Dawn from source...")
       
       # 包含ExternalProject模块
       include(ExternalProject)
       
       # 设置Dawn构建参数
       set(DAWN_BUILD_ARGS 
           "is_debug=${CMAKE_BUILD_TYPE STREQUAL \"Debug\"}"
           "target_cpu=\"${CMAKE_SYSTEM_PROCESSOR}\""
           "dawn_build_tests=false"
           "dawn_build_samples=false"
       )
       
       # 添加平台特定构建参数
       if(WIN32)
           list(APPEND DAWN_BUILD_ARGS "dawn_enable_d3d12=true")
       elseif(APPLE)
           list(APPEND DAWN_BUILD_ARGS "dawn_enable_metal=true")
       else()
           list(APPEND DAWN_BUILD_ARGS "dawn_enable_vulkan=true")
       endif()
       
       # 添加Dawn作为外部项目
       ExternalProject_Add(
           DawnProject
           PREFIX ${CMAKE_BINARY_DIR}/Dawn
           GIT_REPOSITORY https://dawn.googlesource.com/dawn
           GIT_TAG main
           BUILD_COMMAND ${CMAKE_COMMAND} -E env 
               PYTHONPATH=${DAWN_ROOT_DIR}/third_party/gn
               ${DAWN_ROOT_DIR}/third_party/depot_tools/gn gen ${DAWN_OUTPUT_DIR} --args='${DAWN_BUILD_ARGS}'
               COMMAND ${CMAKE_COMMAND} -E env 
               PYTHONPATH=${DAWN_ROOT_DIR}/third_party/gn
               ${DAWN_ROOT_DIR}/third_party/depot_tools/ninja -C ${DAWN_OUTPUT_DIR}
           INSTALL_COMMAND ""
           BUILD_BYPRODUCTS 
               ${DAWN_OUTPUT_DIR}/obj/libwebgpu_dawn${CMAKE_STATIC_LIBRARY_SUFFIX}
               ${DAWN_OUTPUT_DIR}/obj/libdawn_native${CMAKE_STATIC_LIBRARY_SUFFIX}
       )
       
       # 添加依赖关系
       add_dependencies(${PROJECT_NAME} DawnProject)
       
       # 更新库路径
       set(DAWN_WEBGPU_LIB ${DAWN_OUTPUT_DIR}/obj/libwebgpu_dawn${CMAKE_STATIC_LIBRARY_SUFFIX})
       set(DAWN_NATIVE_LIB ${DAWN_OUTPUT_DIR}/obj/libdawn_native${CMAKE_STATIC_LIBRARY_SUFFIX})
   endif()
   ```

3. **初始化Dawn**：
   ```cpp
   namespace primal::graphics::webgpu {
   
   // Dawn初始化结构
   struct DawnInitInfo
   {
       // 后端选择
       bool enableVulkan = true;
       bool enableMetal = true;
       bool enableDirect3D12 = true;
       
       // 调试选项
       bool enableValidation = true;
       bool enableDebugUtils = true;
       
       // 其他选项
       const char* traceFile = nullptr;
   };
   
   // 初始化Dawn
   bool initialize(const DawnInitInfo& info = {});
   
   // 关闭Dawn
   void shutdown();
   
   // 获取WebGPU实例
   WGPUInstance get_instance();
   
   // 获取WebGPU适配器
   WGPUAdapter get_adapter();
   
   // 获取WebGPU设备
   WGPUDevice get_device();
   
   }
   ```

#### 12.2.3 依赖管理与平台注意事项

**1. 依赖管理**

Dawn库依赖于以下第三方库和工具：

- **depot_tools**: Google的开发工具集，包含gn和ninja构建工具
- **Python 3**: 用于构建脚本和依赖管理
- **Node.js**: 用于某些构建步骤和Web相关工具
- **平台特定图形SDK**:
  - Windows: Windows SDK, DirectX Shader Compiler
  - macOS: Xcode Command Line Tools, Metal框架
  - Linux: Vulkan SDK, X11开发库

**2. 平台特定注意事项**

**Windows平台**:
```bash
# 安装Visual Studio 2019或更高版本，包含Windows SDK
# 安装DirectX Shader Compiler
# 确保PATH中包含depot_tools
```

**macOS平台**:
```bash
# 安装Xcode Command Line Tools
xcode-select --install

# 安装Homebrew（如果尚未安装）
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 安装Python 3和Node.js
brew install python node
```

**Linux平台**:
```bash
# Ubuntu/Debian
sudo apt update
sudo apt install -y python3 python3-pip nodejs npm build-essential libvulkan-dev vulkan-tools libx11-dev

# CentOS/RHEL/Fedora
sudo dnf install -y python3 python3-pip nodejs npm gcc-c++ vulkan-devel libX11-devel
```

**3. 构建优化选项**

为了优化Dawn库的大小和性能，可以考虑以下构建选项：

```bash
# 发布版本构建参数
gn gen out/Release --args='
    is_debug=false
    is_component_build=false
    symbol_level=1
    target_cpu="x64"
    dawn_build_tests=false
    dawn_build_samples=false
    dawn_enable_d3d12=true
    dawn_enable_metal=false
    dawn_enable_vulkan=false
    dawn_enable_opengl=false
    dawn_enable_null=false
    use_custom_libcxx=false
'

# 调试版本构建参数
gn gen out/Debug --args='
    is_debug=true
    is_component_build=false
    symbol_level=2
    target_cpu="x64"
    dawn_build_tests=false
    dawn_build_samples=false
    dawn_enable_d3d12=true
    dawn_enable_metal=false
    dawn_enable_vulkan=false
    dawn_enable_opengl=false
    dawn_enable_null=false
    use_custom_libcxx=false
'
```

**4. 集成测试**

为确保Dawn库正确集成，可以添加以下测试代码：

```cpp
// 测试Dawn初始化
bool TestDawnInitialization()
{
    DawnInitInfo info{};
    info.enableValidation = true;
    info.enableDebugUtils = true;
    
    if (!webgpu::initialize(info))
    {
        printf("Failed to initialize Dawn\n");
        return false;
    }
    
    // 检查基本WebGPU对象
    WGPUInstance instance = webgpu::get_instance();
    WGPUAdapter adapter = webgpu::get_adapter();
    WGPUDevice device = webgpu::get_device();
    
    if (!instance || !adapter || !device)
    {
        printf("Failed to get WebGPU objects\n");
        webgpu::shutdown();
        return false;
    }
    
    printf("Dawn initialized successfully\n");
    webgpu::shutdown();
    return true;
}
```

#### 12.2.4 Dawn实现示例

```cpp
namespace primal::graphics::webgpu {

// Dawn实例
static WGPUInstance s_instance = nullptr;
static WGPUAdapter s_adapter = nullptr;
static WGPUDevice s_device = nullptr;

// 初始化Dawn
bool initialize(const DawnInitInfo& info)
{
    // 创建Dawn实例
    WGPUInstanceDescriptor instanceDesc{};
    instanceDesc.nextInChain = nullptr;
    
    s_instance = wgpuCreateInstance(&instanceDesc);
    if (!s_instance)
    {
        return false;
    }
    
    // 请求适配器
    WGPURequestAdapterOptions adapterOptions{};
    adapterOptions.nextInChain = nullptr;
    adapterOptions.powerPreference = WGPUPowerPreference_HighPerformance;
    
    // 根据平台选择后端
    std::vector<WGPURequestAdapterOptions> backendOptions;
    
#if defined(_WIN32)
    if (info.enableDirect3D12)
    {
        WGPUD3D12BackendOptions d3d12Options{};
        d3d12Options.chain.sType = WGPUSType_D3D12BackendOptions;
        adapterOptions.nextInChain = &d3d12Options.chain;
    }
#elif defined(__APPLE__)
    if (info.enableMetal)
    {
        WGPUMetalBackendOptions metalOptions{};
        metalOptions.chain.sType = WGPUSType_MetalBackendOptions;
        adapterOptions.nextInChain = &metalOptions.chain;
    }
#else
    if (info.enableVulkan)
    {
        WGPUVulkanBackendOptions vulkanOptions{};
        vulkanOptions.chain.sType = WGPUSType_VulkanBackendOptions;
        adapterOptions.nextInChain = &vulkanOptions.chain;
    }
#endif
    
    // 请求适配器
    struct AdapterData {
        WGPUAdapter adapter = nullptr;
        bool requestEnded = false;
    };
    
    AdapterData adapterData;
    
    auto onAdapterRequestEnded = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, const char* message, void* userdata) {
        AdapterData* data = static_cast<AdapterData*>(userdata);
        if (status == WGPURequestAdapterStatus_Success) {
            data->adapter = adapter;
        } else {
            printf("Could not get WebGPU adapter: %s\n", message);
        }
        data->requestEnded = true;
    };
    
    wgpuInstanceRequestAdapter(s_instance, &adapterOptions, onAdapterRequestEnded, &adapterData);
    
    // 等待适配器请求完成
    while (!adapterData.requestEnded) {
        wgpuInstanceProcessEvents(s_instance);
    }
    
    if (!adapterData.adapter) {
        return false;
    }
    
    s_adapter = adapterData.adapter;
    
    // 请求设备
    WGPUDeviceDescriptor deviceDesc{};
    deviceDesc.nextInChain = nullptr;
    deviceDesc.label = "GameEngine Device";
    deviceDesc.requiredFeatureCount = 0;
    deviceDesc.requiredFeatures = nullptr;
    deviceDesc.requiredLimits = nullptr;
    
    if (info.enableValidation) {
        WGPUDawnTogglesDeviceDescriptor togglesDesc{};
        togglesDesc.chain.sType = WGPUSType_DawnTogglesDeviceDescriptor;
        
        const char* toggleNames[] = {
            "allow_unsafe_apis",
            "disable_lazy_clear",
            "timestamp_quantization",
            "use_user_defined_labels_in_backend"
        };
        
        togglesDesc.enabledToggleCount = 4;
        togglesDesc.enabledToggles = toggleNames;
        
        deviceDesc.nextInChain = &togglesDesc.chain;
    }
    
    struct DeviceData {
        WGPUDevice device = nullptr;
        bool requestEnded = false;
    };
    
    DeviceData deviceData;
    
    auto onDeviceRequestEnded = [](WGPURequestDeviceStatus status, WGPUDevice device, const char* message, void* userdata) {
        DeviceData* data = static_cast<DeviceData*>(userdata);
        if (status == WGPURequestDeviceStatus_Success) {
            data->device = device;
        } else {
            printf("Could not get WebGPU device: %s\n", message);
        }
        data->requestEnded = true;
    };
    
    wgpuAdapterRequestDevice(s_adapter, &deviceDesc, onDeviceRequestEnded, &deviceData);
    
    // 等待设备请求完成
    while (!deviceData.requestEnded) {
        wgpuInstanceProcessEvents(s_instance);
    }
    
    if (!deviceData.device) {
        return false;
    }
    
    s_device = deviceData.device;
    
    // 设置设备丢失回调
    wgpuDeviceSetUncapturedErrorCallback(s_device, 
        [](WGPUErrorType type, const char* message, void*) {
            printf("WebGPU uncaptured error: %s\n", message);
        }, 
        nullptr
    );
    
    return true;
}

// 关闭Dawn
void shutdown()
{
    if (s_device) {
        wgpuDeviceRelease(s_device);
        s_device = nullptr;
    }
    
    if (s_adapter) {
        wgpuAdapterRelease(s_adapter);
        s_adapter = nullptr;
    }
    
    if (s_instance) {
        wgpuInstanceRelease(s_instance);
        s_instance = nullptr;
    }
}

// 获取WebGPU实例
WGPUInstance get_instance()
{
    return s_instance;
}

// 获取WebGPU适配器
WGPUAdapter get_adapter()
{
    return s_adapter;
}

// 获取WebGPU设备
WGPUDevice get_device()
{
    return s_device;
}

}
```

### 12.3 WebGPU平台特化实现

#### 12.3.1 WebGPU平台特化架构

WebGPU平台特化实现需要遵循RHI层的统一接口设计，同时充分利用WebGPU的特性。以下是WebGPU平台特化的核心架构组件：

1. **平台接口特化**：提供WebGPU特定的初始化、关闭和设备访问
2. **资源工厂特化**：创建WebGPU特定的资源对象
3. **资源类特化**：实现WebGPU特定的缓冲区、纹理、着色器等
4. **命令缓冲区特化**：实现WebGPU特定的命令记录和提交
5. **同步对象特化**：实现WebGPU特定的围栏和信号量

#### 12.3.2 WebGPU平台接口

```cpp
namespace primal::graphics::rhi {

// WebGPU平台接口特化
template<>
struct PlatformInterface<graphics_platform::webgpu>
{
    static bool Initialize()
    {
        DawnInitInfo info{};
        info.enableValidation = true;
        info.enableDebugUtils = true;
        
        // 根据平台选择后端
#if defined(_WIN32)
        info.enableDirect3D12 = true;
#elif defined(__APPLE__)
        info.enableMetal = true;
#else
        info.enableVulkan = true;
#endif
        
        return webgpu::initialize(info);
    }
    
    static void Shutdown()
    {
        webgpu::shutdown();
    }
    
    // 获取设备
    static WGPUDevice GetDevice()
    {
        return webgpu::get_device();
    }
    
    // 获取实例
    static WGPUInstance GetInstance()
    {
        return webgpu::get_instance();
    }
    
    // 获取适配器
    static WGPUAdapter GetAdapter()
    {
        return webgpu::get_adapter();
    }
    
    // 其他WebGPU特定方法...
};

}
```

#### 12.3.3 WebGPU资源类特化

**1. WebGPU缓冲区实现**

```cpp
namespace primal::graphics::rhi {

// WebGPU缓冲区特化
template<>
class Buffer<graphics_platform::webgpu> : public BufferBase
{
public:
    Buffer(WGPUDevice device, const BufferDesc& desc)
        : BufferBase(desc), device_(device)
    {
        // 转换缓冲区使用标志
        WGPUBufferUsageFlags usage = WGPUBufferUsage_None;
        
        if (desc.usage & BufferUsage::Vertex) {
            usage |= WGPUBufferUsage_Vertex;
        }
        if (desc.usage & BufferUsage::Index) {
            usage |= WGPUBufferUsage_Index;
        }
        if (desc.usage & BufferUsage::Uniform) {
            usage |= WGPUBufferUsage_Uniform;
        }
        if (desc.usage & BufferUsage::Storage) {
            usage |= WGPUBufferUsage_Storage;
        }
        if (desc.usage & BufferUsage::TransferSrc) {
            usage |= WGPUBufferUsage_CopySrc;
        }
        if (desc.usage & BufferUsage::TransferDst) {
            usage |= WGPUBufferUsage_CopyDst;
        }
        
        // 创建缓冲区描述
        WGPUBufferDescriptor bufferDesc{};
        bufferDesc.nextInChain = nullptr;
        bufferDesc.label = "WebGPU Buffer";
        bufferDesc.size = desc.size;
        bufferDesc.usage = usage;
        bufferDesc.mappedAtCreation = (desc.memoryType == MemoryType::HostVisible);
        
        // 创建WebGPU缓冲区
        buffer_ = wgpuDeviceCreateBuffer(device_, &bufferDesc);
        
        if (!buffer_) {
            throw std::runtime_error("Failed to create WebGPU buffer");
        }
        
        // 如果需要，映射缓冲区
        if (desc.memoryType == MemoryType::HostVisible && bufferDesc.mappedAtCreation) {
            mappedData_ = wgpuBufferGetMappedRange(buffer_, 0, desc.size);
        }
    }
    
    ~Buffer() override
    {
        if (mappedData_) {
            wgpuBufferUnmap(buffer_);
        }
        
        if (buffer_) {
            wgpuBufferRelease(buffer_);
        }
    }
    
    void* Map() override
    {
        if (!mappedData_) {
            // 异步映射缓冲区
            struct MapData {
                void* mappedData = nullptr;
                bool mapEnded = false;
            };
            
            MapData mapData;
            
            auto onMapCallback = [](WGPUBufferMapAsyncStatus status, void* userdata) {
                MapData* data = static_cast<MapData*>(userdata);
                if (status == WGPUBufferMapAsyncStatus_Success) {
                    // 获取映射范围
                    data->mappedData = wgpuBufferGetMappedRange(static_cast<Buffer*>(userdata)->buffer_, 0, static_cast<Buffer*>(userdata)->GetSize());
                }
                data->mapEnded = true;
            };
            
            wgpuBufferMapAsync(buffer_, WGPUMapMode_Write, 0, GetSize(), onMapCallback, &mapData);
            
            // 等待映射完成
            WGPUDevice device = PlatformInterface<graphics_platform::webgpu>::GetDevice();
            while (!mapData.mapEnded) {
                wgpuDeviceTick(device);
            }
            
            mappedData_ = mapData.mappedData;
        }
        
        return mappedData_;
    }
    
    void Unmap() override
    {
        if (mappedData_) {
            wgpuBufferUnmap(buffer_);
            mappedData_ = nullptr;
        }
    }
    
    WGPUBuffer GetNativeBuffer() const { return buffer_; }
    
private:
    WGPUDevice device_ = nullptr;
    WGPUBuffer buffer_ = nullptr;
    void* mappedData_ = nullptr;
};

}
```

**2. WebGPU纹理实现**

```cpp
namespace primal::graphics::rhi {

// WebGPU纹理特化
template<>
class Texture<graphics_platform::webgpu> : public TextureBase
{
public:
    Texture(WGPUDevice device, const TextureDesc& desc)
        : TextureBase(desc), device_(device)
    {
        // 转换纹理维度
        WGPUTextureDimension dimension;
        switch (desc.type) {
            case TextureType::Texture1D:
                dimension = WGPUTextureDimension_1D;
                break;
            case TextureType::Texture2D:
                dimension = WGPUTextureDimension_2D;
                break;
            case TextureType::Texture3D:
                dimension = WGPUTextureDimension_3D;
                break;
            default:
                dimension = WGPUTextureDimension_2D;
                break;
        }
        
        // 转换纹理使用标志
        WGPUTextureUsageFlags usage = WGPUTextureUsage_None;
        
        if (desc.usage & TextureUsage::ShaderRead) {
            usage |= WGPUTextureUsage_TextureBinding;
        }
        if (desc.usage & TextureUsage::ShaderWrite) {
            usage |= WGPUTextureUsage_StorageBinding;
        }
        if (desc.usage & TextureUsage::RenderTarget) {
            usage |= WGPUTextureUsage_RenderAttachment;
        }
        if (desc.usage & TextureUsage::TransferSrc) {
            usage |= WGPUTextureUsage_CopySrc;
        }
        if (desc.usage & TextureUsage::TransferDst) {
            usage |= WGPUTextureUsage_CopyDst;
        }
        
        // 转换纹理格式
        WGPUTextureFormat format = ConvertTextureFormat(desc.format);
        
        // 创建纹理描述
        WGPUTextureDescriptor textureDesc{};
        textureDesc.nextInChain = nullptr;
        textureDesc.label = "WebGPU Texture";
        textureDesc.usage = usage;
        textureDesc.dimension = dimension;
        textureDesc.size.width = desc.width;
        textureDesc.size.height = desc.height;
        textureDesc.size.depthOrArrayLayers = desc.depth;
        textureDesc.format = format;
        textureDesc.mipLevelCount = desc.mipLevels;
        textureDesc.sampleCount = desc.sampleCount;
        
        // 创建WebGPU纹理
        texture_ = wgpuDeviceCreateTexture(device_, &textureDesc);
        
        if (!texture_) {
            throw std::runtime_error("Failed to create WebGPU texture");
        }
        
        // 创建纹理视图
        WGPUTextureViewDescriptor viewDesc{};
        viewDesc.nextInChain = nullptr;
        viewDesc.label = "WebGPU Texture View";
        viewDesc.format = format;
        viewDesc.dimension = (dimension == WGPUTextureDimension_2D && desc.depth == 1) ? 
                            WGPUTextureViewDimension_2D : WGPUTextureViewDimension_2DArray;
        viewDesc.baseMipLevel = 0;
        viewDesc.mipLevelCount = desc.mipLevels;
        viewDesc.baseArrayLayer = 0;
        viewDesc.arrayLayerCount = desc.depth;
        viewDesc.aspect = WGPUTextureAspect_All;
        
        textureView_ = wgpuTextureCreateView(texture_, &viewDesc);
        
        if (!textureView_) {
            wgpuTextureRelease(texture_);
            throw std::runtime_error("Failed to create WebGPU texture view");
        }
    }
    
    ~Texture() override
    {
        if (textureView_) {
            wgpuTextureViewRelease(textureView_);
        }
        
        if (texture_) {
            wgpuTextureRelease(texture_);
        }
    }
    
    WGPUTexture GetNativeTexture() const { return texture_; }
    WGPUTextureView GetNativeTextureView() const { return textureView_; }
    
private:
    WGPUDevice device_ = nullptr;
    WGPUTexture texture_ = nullptr;
    WGPUTextureView textureView_ = nullptr;
    
    // 转换纹理格式
    WGPUTextureFormat ConvertTextureFormat(TextureFormat format)
    {
        switch (format) {
            case TextureFormat::RGBA8_UNorm:
                return WGPUTextureFormat_RGBA8Unorm;
            case TextureFormat::BGRA8_UNorm:
                return WGPUTextureFormat_BGRA8Unorm;
            case TextureFormat::R32_Float:
                return WGPUTextureFormat_R32Float;
            case TextureFormat::RG32_Float:
                return WGPUTextureFormat_RG32Float;
            case TextureFormat::RGBA32_Float:
                return WGPUTextureFormat_RGBA32Float;
            case TextureFormat::D32_Float:
                return WGPUTextureFormat_Depth32Float;
            case TextureFormat::D24_UNorm_S8_UInt:
                return WGPUTextureFormat_Depth24PlusStencil8;
            default:
                return WGPUTextureFormat_RGBA8Unorm;
        }
    }
};

}
```

**3. WebGPU着色器实现**

```cpp
namespace primal::graphics::rhi {

// WebGPU着色器特化
template<>
class Shader<graphics_platform::webgpu> : public ShaderBase
{
public:
    Shader(WGPUDevice device, const ShaderDesc& desc)
        : ShaderBase(desc), device_(device)
    {
        // 转换着色器阶段
        WGPUShaderStage stage;
        switch (desc.stage) {
            case ShaderStage::Vertex:
                stage = WGPUShaderStage_Vertex;
                break;
            case ShaderStage::Fragment:
                stage = WGPUShaderStage_Fragment;
                break;
            case ShaderStage::Compute:
                stage = WGPUShaderStage_Compute;
                break;
            default:
                throw std::runtime_error("Unsupported shader stage");
        }
        
        // 创建着色器模块描述
        WGPUShaderModuleWGSLDescriptor wgslDesc{};
        wgslDesc.chain.sType = WGPUSType_ShaderModuleWGSLDescriptor;
        wgslDesc.code = desc.source.c_str();
        
        WGPUShaderModuleDescriptor shaderDesc{};
        shaderDesc.nextInChain = &wgslDesc.chain;
        shaderDesc.label = "WebGPU Shader";
        
        // 创建WebGPU着色器模块
        shaderModule_ = wgpuDeviceCreateShaderModule(device_, &shaderDesc);
        
        if (!shaderModule_) {
            throw std::runtime_error("Failed to create WebGPU shader module");
        }
    }
    
    ~Shader() override
    {
        if (shaderModule_) {
            wgpuShaderModuleRelease(shaderModule_);
        }
    }
    
    WGPUShaderModule GetNativeShaderModule() const { return shaderModule_; }
    
private:
    WGPUDevice device_ = nullptr;
    WGPUShaderModule shaderModule_ = nullptr;
};

}
```

#### 12.3.4 WebGPU资源工厂

```cpp
namespace primal::graphics::rhi {

// WebGPU资源工厂特化
template<>
class ResourceFactory<graphics_platform::webgpu> : public ResourceFactoryBase
{
public:
    ResourceFactory()
    {
        device_ = PlatformInterface<graphics_platform::webgpu>::GetDevice();
        instance_ = PlatformInterface<graphics_platform::webgpu>::GetInstance();
    }
    
    // 创建缓冲区
    std::unique_ptr<Buffer<graphics_platform::webgpu>> CreateBuffer(const BufferDesc& desc) override
    {
        return std::make_unique<WebGPUBuffer>(device_, desc);
    }
    
    // 创建纹理
    std::unique_ptr<Texture<graphics_platform::webgpu>> CreateTexture(const TextureDesc& desc) override
    {
        return std::make_unique<WebGPUTexture>(device_, desc);
    }
    
    // 创建采样器
    std::unique_ptr<Sampler<graphics_platform::webgpu>> CreateSampler(const SamplerDesc& desc) override
    {
        return std::make_unique<WebGPUSampler>(device_, desc);
    }
    
    // 创建着色器
    std::unique_ptr<Shader<graphics_platform::webgpu>> CreateShader(const ShaderDesc& desc) override
    {
        return std::make_unique<WebGPUShader>(device_, desc);
    }
    
    // 创建图形管线
    std::unique_ptr<GraphicsPipeline<graphics_platform::webgpu>> CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override
    {
        return std::make_unique<WebGPUGraphicsPipeline>(device_, desc);
    }
    
    // 创建计算管线
    std::unique_ptr<ComputePipeline<graphics_platform::webgpu>> CreateComputePipeline(const ComputePipelineDesc& desc) override
    {
        return std::make_unique<WebGPUComputePipeline>(device_, desc);
    }
    
    // 创建描述符池
    std::unique_ptr<DescriptorPool<graphics_platform::webgpu>> CreateDescriptorPool(const std::vector<std::pair<DescriptorType, u32>>& poolSizes) override
    {
        return std::make_unique<WebGPUDescriptorPool>(device_, poolSizes);
    }
    
    // 创建描述符集布局
    std::unique_ptr<DescriptorSetLayout<graphics_platform::webgpu>> CreateDescriptorSetLayout(const std::vector<BindingPoint>& bindingPoints) override
    {
        return std::make_unique<WebGPUDescriptorSetLayout>(device_, bindingPoints);
    }
    
    // 创建命令缓冲区
    std::unique_ptr<CommandBuffer<graphics_platform::webgpu>> CreateCommandBuffer(CommandQueueType queueType) override
    {
        return std::make_unique<WebGPUCommandBuffer>(device_, queueType);
    }
    
    // 创建表面
    std::unique_ptr<Surface<graphics_platform::webgpu>> CreateSurface(platform::window window) override
    {
        return std::make_unique<WebGPUSurface>(instance_, device_, window);
    }
    
    // 创建同步对象
    std::unique_ptr<Fence<graphics_platform::webgpu>> CreateFence(bool signaled = false) override
    {
        return std::make_unique<WebGPUFence>(device_, signaled);
    }
    
private:
    WGPUDevice device_ = nullptr;
    WGPUInstance instance_ = nullptr;
};

}
```

#### 12.3.5 WebGPU命令缓冲区实现

```cpp
namespace primal::graphics::rhi {

// WebGPU命令缓冲区特化
template<>
class CommandBuffer<graphics_platform::webgpu> : public CommandBufferBase
{
public:
    CommandBuffer(WGPUDevice device, CommandQueueType queueType)
        : CommandBufferBase(queueType), device_(device)
    {
        // 获取命令队列
        WGPUQueueDescriptor queueDesc{};
        queueDesc.nextInChain = nullptr;
        queueDesc.label = "WebGPU Command Queue";
        
        queue_ = wgpuDeviceGetQueue(device_);
        
        if (!queue_) {
            throw std::runtime_error("Failed to get WebGPU command queue");
        }
        
        // 创建命令编码器
        encoder_ = nullptr;
        renderPassEncoder_ = nullptr;
        computePassEncoder_ = nullptr;
    }
    
    ~CommandBuffer() override
    {
        if (renderPassEncoder_) {
            wgpuRenderPassEncoderEnd(renderPassEncoder_);
            renderPassEncoder_ = nullptr;
        }
        
        if (computePassEncoder_) {
            wgpuComputePassEncoderEnd(computePassEncoder_);
            computePassEncoder_ = nullptr;
        }
        
        if (encoder_) {
            wgpuCommandEncoderEnd(encoder_);
            encoder_ = nullptr;
        }
        
        if (queue_) {
            wgpuQueueRelease(queue_);
        }
    }
    
    void Begin() override
    {
        // 创建命令编码器
        WGPUCommandEncoderDescriptor encoderDesc{};
        encoderDesc.nextInChain = nullptr;
        encoderDesc.label = "WebGPU Command Encoder";
        
        encoder_ = wgpuDeviceCreateCommandEncoder(device_, &encoderDesc);
        
        if (!encoder_) {
            throw std::runtime_error("Failed to create WebGPU command encoder");
        }
    }
    
    void End() override
    {
        // 结束任何活动的编码器
        if (renderPassEncoder_) {
            wgpuRenderPassEncoderEnd(renderPassEncoder_);
            renderPassEncoder_ = nullptr;
        }
        
        if (computePassEncoder_) {
            wgpuComputePassEncoderEnd(computePassEncoder_);
            computePassEncoder_ = nullptr;
        }
        
        // 结束命令编码器
        if (encoder_) {
            commandBuffer_ = wgpuCommandEncoderFinish(encoder_, nullptr);
            wgpuCommandEncoderRelease(encoder_);
            encoder_ = nullptr;
            
            if (!commandBuffer_) {
                throw std::runtime_error("Failed to finish WebGPU command buffer");
            }
        }
    }
    
    void Submit() override
    {
        if (commandBuffer_) {
            wgpuQueueSubmit(queue_, 1, &commandBuffer_);
            wgpuCommandBufferRelease(commandBuffer_);
            commandBuffer_ = nullptr;
        }
    }
    
    void BeginRenderPass(const RenderPassDesc& desc) override
    {
        // 转换渲染通道描述
        WGPURenderPassColorAttachment colorAttachments[8] = {};
        WGPURenderPassDepthStencilAttachment depthStencilAttachment = {};
        
        // 设置颜色附件
        for (u32 i = 0; i < desc.colorAttachmentCount; ++i) {
            colorAttachments[i].view = static_cast<Texture<graphics_platform::webgpu>*>(desc.colorAttachments[i].texture)->GetNativeTextureView();
            colorAttachments[i].resolveTarget = nullptr;
            colorAttachments[i].loadOp = ConvertLoadOp(desc.colorAttachments[i].loadOp);
            colorAttachments[i].storeOp = ConvertStoreOp(desc.colorAttachments[i].storeOp);
            colorAttachments[i].clearValue = ConvertClearColor(desc.colorAttachments[i].clearColor);
        }
        
        // 设置深度模板附件
        if (desc.depthStencilAttachment.texture) {
            depthStencilAttachment.view = static_cast<Texture<graphics_platform::webgpu>*>(desc.depthStencilAttachment.texture)->GetNativeTextureView();
            depthStencilAttachment.depthLoadOp = ConvertLoadOp(desc.depthStencilAttachment.depthLoadOp);
            depthStencilAttachment.depthStoreOp = ConvertStoreOp(desc.depthStencilAttachment.depthStoreOp);
            depthStencilAttachment.stencilLoadOp = ConvertLoadOp(desc.depthStencilAttachment.stencilLoadOp);
            depthStencilAttachment.stencilStoreOp = ConvertStoreOp(desc.depthStencilAttachment.stencilStoreOp);
            depthStencilAttachment.clearDepth = desc.depthStencilAttachment.clearDepth;
            depthStencilAttachment.clearStencil = desc.depthStencilAttachment.clearStencil;
        }
        
        // 创建渲染通道描述
        WGPURenderPassDescriptor renderPassDesc{};
        renderPassDesc.nextInChain = nullptr;
        renderPassDesc.colorAttachmentCount = desc.colorAttachmentCount;
        renderPassDesc.colorAttachments = colorAttachments;
        renderPassDesc.depthStencilAttachment = desc.depthStencilAttachment.texture ? &depthStencilAttachment : nullptr;
        
        // 开始渲染通道
        renderPassEncoder_ = wgpuCommandEncoderBeginRenderPass(encoder_, &renderPassDesc);
        
        if (!renderPassEncoder_) {
            throw std::runtime_error("Failed to begin WebGPU render pass");
        }
    }
    
    void EndRenderPass() override
    {
        if (renderPassEncoder_) {
            wgpuRenderPassEncoderEnd(renderPassEncoder_);
            renderPassEncoder_ = nullptr;
        }
    }
    
    void BindPipeline(GraphicsPipeline<graphics_platform::webgpu>* pipeline) override
    {
        if (renderPassEncoder_) {
            wgpuRenderPassEncoderSetPipeline(renderPassEncoder_, pipeline->GetNativeRenderPipeline());
        }
    }
    
    void BindDescriptorSets(GraphicsPipeline<graphics_platform::webgpu>* pipeline, u32 firstSet, 
                           const std::vector<DescriptorSet<graphics_platform::webgpu>*>& descriptorSets) override
    {
        if (renderPassEncoder_) {
            for (size_t i = 0; i < descriptorSets.size(); ++i) {
                u32 set = firstSet + static_cast<u32>(i);
                WGPUBindGroup bindGroup = descriptorSets[i]->GetNativeBindGroup();
                wgpuRenderPassEncoderSetBindGroup(renderPassEncoder_, set, bindGroup, 0, nullptr);
            }
        }
    }
    
    void BindVertexBuffers(u32 firstBinding, u32 bindingCount, 
                          Buffer<graphics_platform::webgpu>* const* buffers, const u64* offsets) override
    {
        if (renderPassEncoder_) {
            for (u32 i = 0; i < bindingCount; ++i) {
                WGPUBuffer buffer = buffers[i]->GetNativeBuffer();
                wgpuRenderPassEncoderSetVertexBuffer(renderPassEncoder_, firstBinding + i, buffer, offsets[i], WGPU_WHOLE_SIZE);
            }
        }
    }
    
    void BindIndexBuffer(Buffer<graphics_platform::webgpu>* buffer, u64 offset = 0) override
    {
        if (renderPassEncoder_) {
            WGPUBuffer indexBuffer = buffer->GetNativeBuffer();
            wgpuRenderPassEncoderSetIndexBuffer(renderPassEncoder_, indexBuffer, WGPUIndexFormat_Undefined, offset, WGPU_WHOLE_SIZE);
        }
    }
    
    void Draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0, u32 firstInstance = 0) override
    {
        if (renderPassEncoder_) {
            wgpuRenderPassEncoderDraw(renderPassEncoder_, vertexCount, instanceCount, firstVertex, firstInstance);
        }
    }
    
    void DrawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0, u32 baseVertex = 0, u32 firstInstance = 0) override
    {
        if (renderPassEncoder_) {
            wgpuRenderPassEncoderDrawIndexed(renderPassEncoder_, indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
        }
    }
    
    void SetViewport(f32 x, f32 y, f32 width, f32 height, f32 minDepth = 0.0f, f32 maxDepth = 1.0f) override
    {
        if (renderPassEncoder_) {
            wgpuRenderPassEncoderSetViewport(renderPassEncoder_, x, y, width, height, minDepth, maxDepth);
        }
    }
    
    void SetScissorRect(u32 x, u32 y, u32 width, u32 height) override
    {
        if (renderPassEncoder_) {
            wgpuRenderPassEncoderSetScissorRect(renderPassEncoder_, x, y, width, height);
        }
    }
    
private:
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUCommandEncoder encoder_ = nullptr;
    WGPUCommandBuffer commandBuffer_ = nullptr;
    WGPURenderPassEncoder renderPassEncoder_ = nullptr;
    WGPUComputePassEncoder computePassEncoder_ = nullptr;
    
    // 转换加载操作
    WGPULoadOp ConvertLoadOp(LoadOp loadOp)
    {
        switch (loadOp) {
            case LoadOp::Load:
                return WGPULoadOp_Load;
            case LoadOp::Clear:
                return WGPULoadOp_Clear;
            case LoadOp::DontCare:
                return WGPULoadOp_Undefined;
            default:
                return WGPULoadOp_Load;
        }
    }
    
    // 转换存储操作
    WGPUStoreOp ConvertStoreOp(StoreOp storeOp)
    {
        switch (storeOp) {
            case StoreOp::Store:
                return WGPUStoreOp_Store;
            case StoreOp::DontCare:
                return WGPUStoreOp_Undefined;
            default:
                return WGPUStoreOp_Store;
        }
    }
    
    // 转换清除颜色
    WGPUColor ConvertClearColor(const f32 color[4])
    {
        WGPUColor clearColor{};
        clearColor.r = color[0];
        clearColor.g = color[1];
        clearColor.b = color[2];
        clearColor.a = color[3];
        return clearColor;
    }
};

}
```

#### 12.3.6 WebGPU同步对象实现

```cpp
namespace primal::graphics::rhi {

// WebGPU围栏特化
template<>
class Fence<graphics_platform::webgpu> : public FenceBase
{
public:
    Fence(WGPUDevice device, bool signaled)
        : device_(device), signaled_(signaled)
    {
        // WebGPU没有直接的围栏对象，使用回调来模拟
        // 在实际实现中，可能需要使用其他机制
    }
    
    ~Fence() override
    {
        // WebGPU没有直接的围栏释放
    }
    
    void Reset() override
    {
        signaled_ = false;
    }
    
    bool Wait(u64 timeoutMs = UINT64_MAX) override
    {
        // WebGPU没有直接的围栏等待，使用设备轮询
        auto startTime = std::chrono::high_resolution_clock::now();
        
        while (!signaled_) {
            wgpuDeviceTick(device_);
            
            auto currentTime = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count();
            
            if (elapsed >= timeoutMs) {
                return false;
            }
        }
        
        return true;
    }
    
    void Signal() override
    {
        signaled_ = true;
    }
    
    bool IsSignaled() const override
    {
        return signaled_;
    }
    
private:
    WGPUDevice device_ = nullptr;
    std::atomic<bool> signaled_;
};

}
```

### 12.4 WebGPU使用示例

#### 12.4.1 初始化WebGPU

```cpp
// 初始化WebGPU平台
bool InitializeWebGPU()
{
    // 设置平台接口
    graphics_platform platform = graphics_platform::webgpu;
    
    // 初始化RHI系统
    if (!RHISystem::Initialize(platform))
    {
        printf("Failed to initialize WebGPU RHI system\n");
        return false;
    }
    
    printf("WebGPU RHI system initialized successfully\n");
    return true;
}
```

#### 12.4.2 创建和使用资源

```cpp
// 创建WebGPU缓冲区
void CreateWebGPUBuffer()
{
    auto factory = RHISystem::GetResourceFactory();
    
    BufferDesc bufferDesc{};
    bufferDesc.size = 1024 * 1024; // 1MB
    bufferDesc.usage = BufferUsage::Vertex | BufferUsage::TransferDst;
    bufferDesc.memoryType = MemoryType::DeviceLocal;
    
    auto vertexBuffer = factory->CreateBuffer(bufferDesc);
    
    // 映射并更新缓冲区数据
    void* mappedData = vertexBuffer->Map();
    memcpy(mappedData, vertexData, dataSize);
    vertexBuffer->Unmap();
}

// 创建WebGPU纹理
void CreateWebGPUTexture()
{
    auto factory = RHISystem::GetResourceFactory();
    
    TextureDesc textureDesc{};
    textureDesc.width = 1024;
    textureDesc.height = 1024;
    textureDesc.depth = 1;
    textureDesc.format = TextureFormat::RGBA8_UNorm;
    textureDesc.usage = TextureUsage::ShaderRead | TextureUsage::RenderTarget;
    textureDesc.type = TextureType::Texture2D;
    
    auto texture = factory->CreateTexture(textureDesc);
    
    // 上传纹理数据
    auto commandBuffer = RHISystem::GetCommandBuffer(CommandQueueType::Graphics);
    commandBuffer->Begin();
    commandBuffer->UploadTexture(texture, textureData, dataSize);
    commandBuffer->End();
    commandBuffer->Submit();
}
```

#### 12.4.3 渲染流程

```cpp
// WebGPU渲染流程
void RenderFrame()
{
    auto factory = RHISystem::GetResourceFactory();
    auto commandBuffer = RHISystem::GetCommandBuffer(CommandQueueType::Graphics);
    
    // 开始命令缓冲区记录
    commandBuffer->Begin();
    
    // 设置渲染目标
    commandBuffer->BeginRenderPass(renderPass, framebuffer);
    
    // 绑定管线
    commandBuffer->BindPipeline(graphicsPipeline);
    
    // 绑定资源
    std::vector<DescriptorSet<graphics_platform::webgpu>*> descriptorSets = { descriptorSet };
    commandBuffer->BindDescriptorSets(graphicsPipeline, 0, descriptorSets);
    
    // 绑定顶点和索引缓冲区
    Buffer<graphics_platform::webgpu>* vertexBuffers[] = { vertexBuffer };
    u64 offsets[] = { 0 };
    commandBuffer->BindVertexBuffers(0, 1, vertexBuffers, offsets);
    commandBuffer->BindIndexBuffer(indexBuffer);
    
    // 设置视口和裁剪矩形
    commandBuffer->SetViewport(0, 0, 1920, 1080);
    commandBuffer->SetScissorRect(0, 0, 1920, 1080);
    
    // 绘制
    commandBuffer->DrawIndexed(indexCount);
    
    // 结束渲染通道
    commandBuffer->EndRenderPass();
    
    // 结束命令缓冲区记录
    commandBuffer->End();
    
    // 提交命令缓冲区
    commandBuffer->Submit();
    
    // 呈现
    RHISystem::Present();
}
```

### 12.5 WebGPU优势与限制

#### 12.5.1 优势

1. **真正的跨平台**：一套代码在Windows、Linux、macOS上运行
2. **现代GPU功能**：支持计算着色器、光线追踪等现代GPU功能
3. **Web兼容性**：与Web平台共享相同的API，便于移植
4. **安全性**：内置验证层，减少GPU崩溃
5. **未来导向**：作为Web图形的未来标准，持续发展

#### 12.5.2 限制

1. **性能开销**：作为抽象层，可能比原生API有轻微性能开销
2. **功能限制**：某些平台特定功能可能不可用
3. **生态成熟度**：相比Direct3D12/Vulkan/Metal，生态还不够成熟
4. **调试工具**：调试工具相比原生API还不够完善

### 12.5 WebGPU性能优化与最佳实践

#### 12.5.1 性能优化策略

**1. 资源管理优化**

```cpp
// 使用资源池减少创建/销毁开销
template<typename T>
class WebGPUResourcePool
{
public:
    std::shared_ptr<T> Acquire()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (!pool_.empty())
        {
            auto resource = pool_.back();
            pool_.pop_back();
            return resource;
        }
        
        return CreateResource();
    }
    
    void Release(std::shared_ptr<T> resource)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pool_.push_back(resource);
    }
    
private:
    std::vector<std::shared_ptr<T>> pool_;
    std::mutex mutex_;
    
    virtual std::shared_ptr<T> CreateResource() = 0;
};

// 缓冲区池实现
class WebGPUBufferPool : public WebGPUResourcePool<Buffer<graphics_platform::webgpu>>
{
private:
    std::shared_ptr<Buffer<graphics_platform::webgpu>> CreateResource() override
    {
        BufferDesc desc{};
        desc.size = 1024 * 1024; // 1MB
        desc.usage = BufferUsage::Vertex | BufferUsage::TransferDst;
        desc.memoryType = MemoryType::DeviceLocal;
        
        auto device = PlatformInterface<graphics_platform::webgpu>::GetDevice();
        return std::make_shared<Buffer<graphics_platform::webgpu>>(device, desc);
    }
};
```

**2. 命令缓冲区批处理**

```cpp
// 批处理命令缓冲区提交
class WebGPUCommandBatcher
{
public:
    void AddCommandBuffer(CommandBuffer<graphics_platform::webgpu>* commandBuffer)
    {
        batch_.push_back(commandBuffer);
        
        // 达到批处理大小时提交
        if (batch_.size() >= batchSize_)
        {
            SubmitBatch();
        }
    }
    
    void SubmitBatch()
    {
        if (batch_.empty())
        {
            return;
        }
        
        // 转换为WebGPU命令缓冲区
        std::vector<WGPUCommandBuffer> webgpuCommandBuffers;
        webgpuCommandBuffers.reserve(batch_.size());
        
        for (auto* cmdBuffer : batch_)
        {
            // 假设CommandBuffer有GetNativeCommandBuffer方法
            webgpuCommandBuffers.push_back(cmdBuffer->GetNativeCommandBuffer());
        }
        
        // 批量提交
        auto queue = PlatformInterface<graphics_platform::webgpu>::GetQueue();
        wgpuQueueSubmit(queue, static_cast<u32>(webgpuCommandBuffers.size()), webgpuCommandBuffers.data());
        
        batch_.clear();
    }
    
    void Flush()
    {
        SubmitBatch();
    }
    
private:
    std::vector<CommandBuffer<graphics_platform::webgpu>*> batch_;
    size_t batchSize_ = 16; // 批处理大小
};
```

**3. 描述符集缓存**

```cpp
// 描述符集布局缓存
class WebGPUDescriptorSetLayoutCache
{
public:
    WGPUBindGroupLayout GetOrCreateLayout(const std::vector<BindingPoint>& bindingPoints)
    {
        // 生成布局键
        size_t key = GenerateLayoutKey(bindingPoints);
        
        // 查找缓存
        auto it = cache_.find(key);
        if (it != cache_.end())
        {
            return it->second;
        }
        
        // 创建新布局
        auto device = PlatformInterface<graphics_platform::webgpu>::GetDevice();
        WGPUBindGroupLayout layout = CreateBindGroupLayout(device, bindingPoints);
        
        // 添加到缓存
        cache_[key] = layout;
        
        return layout;
    }
    
private:
    size_t GenerateLayoutKey(const std::vector<BindingPoint>& bindingPoints)
    {
        size_t key = 0;
        for (const auto& binding : bindingPoints)
        {
            // 简单哈希组合
            key ^= std::hash<u32>{}(binding.binding) + 0x9e3779b9 + (key << 6) + (key >> 2);
            key ^= std::hash<u32>{}(static_cast<u32>(binding.type)) + 0x9e3779b9 + (key << 6) + (key >> 2);
            key ^= std::hash<u32>{}(binding.count) + 0x9e3779b9 + (key << 6) + (key >> 2);
        }
        return key;
    }
    
    WGPUBindGroupLayout CreateBindGroupLayout(WGPUDevice device, const std::vector<BindingPoint>& bindingPoints)
    {
        std::vector<WGPUBindGroupLayoutEntry> entries;
        entries.reserve(bindingPoints.size());
        
        for (const auto& binding : bindingPoints)
        {
            WGPUBindGroupLayoutEntry entry{};
            entry.binding = binding.binding;
            entry.visibility = ConvertShaderStageFlags(binding.stageFlags);
            entry.buffer.type = ConvertBufferType(binding.type);
            entry.buffer.hasDynamicOffset = false;
            entry.buffer.minBindingSize = 0;
            
            entries.push_back(entry);
        }
        
        WGPUBindGroupLayoutDescriptor layoutDesc{};
        layoutDesc.nextInChain = nullptr;
        layoutDesc.label = "WebGPU Bind Group Layout";
        layoutDesc.entryCount = static_cast<u32>(entries.size());
        layoutDesc.entries = entries.data();
        
        return wgpuDeviceCreateBindGroupLayout(device, &layoutDesc);
    }
    
    std::unordered_map<size_t, WGPUBindGroupLayout> cache_;
};
```

#### 12.5.2 最佳实践

**1. 资源生命周期管理**

```cpp
// 使用RAII管理WebGPU资源
template<typename T>
class WebGPURAIIGuard
{
public:
    WebGPURAIIGuard(T* resource) : resource_(resource) {}
    
    ~WebGPURAIIGuard()
    {
        if (resource_)
        {
            ReleaseResource(resource_);
        }
    }
    
    // 禁止拷贝
    WebGPURAIIGuard(const WebGPURAIIGuard&) = delete;
    WebGPURAIIGuard& operator=(const WebGPURAIIGuard&) = delete;
    
    // 允许移动
    WebGPURAIIGuard(WebGPURAIIGuard&& other) noexcept : resource_(other.resource_)
    {
        other.resource_ = nullptr;
    }
    
    WebGPURAIIGuard& operator=(WebGPURAIIGuard&& other) noexcept
    {
        if (this != &other)
        {
            if (resource_)
            {
                ReleaseResource(resource_);
            }
            
            resource_ = other.resource_;
            other.resource_ = nullptr;
        }
        
        return *this;
    }
    
    T* Get() const { return resource_; }
    T* operator->() const { return resource_; }
    
private:
    T* resource_ = nullptr;
    
    void ReleaseResource(T* resource);
};

// 特化释放函数
template<>
void WebGPURAIIGuard<WGPUBuffer>::ReleaseResource(WGPUBuffer* resource)
{
    if (resource)
    {
        wgpuBufferRelease(resource);
    }
}
```

**2. 错误处理与调试**

```cpp
// WebGPU错误处理包装器
class WebGPUErrorHandler
{
public:
    static void CheckError(WGPUErrorType type, const char* message)
    {
        if (type != WGPUErrorType_NoError)
        {
            std::string errorType = GetErrorTypeString(type);
            std::string fullMessage = errorType + ": " + message;
            
            // 记录错误
            LogError(fullMessage);
            
            // 在调试模式下断言
            #ifdef _DEBUG
            assert(false && fullMessage.c_str());
            #endif
        }
    }
    
    static void SetDeviceErrorCallback(WGPUDevice device)
    {
        wgpuDeviceSetUncapturedErrorCallback(device, 
            [](WGPUErrorType type, const char* message, void* userdata) {
                CheckError(type, message);
            }, 
            nullptr
        );
    }
    
private:
    static std::string GetErrorTypeString(WGPUErrorType type)
    {
        switch (type)
        {
            case WGPUErrorType_Validation: return "Validation";
            case WGPUErrorType_OutOfMemory: return "Out of Memory";
            case WGPUErrorType_Unknown: return "Unknown";
            case WGPUErrorType_DeviceLost: return "Device Lost";
            default: return "Invalid";
        }
    }
    
    static void LogError(const std::string& message)
    {
        // 使用引擎的日志系统
        // Logger::Error("WebGPU: {}", message);
        printf("WebGPU Error: %s\n", message.c_str());
    }
};
```

**3. 性能分析**

```cpp
// WebGPU性能分析器
class WebGPUProfiler
{
public:
    struct QueryResult
    {
        u64 timestamp;
        std::string label;
    };
    
    WebGPUProfiler(WGPUDevice device) : device_(device)
    {
        // 创建查询集
        WGPUQuerySetDescriptor queryDesc{};
        queryDesc.nextInChain = nullptr;
        queryDesc.label = "WebGPU Timestamp Query Set";
        queryDesc.type = WGPUQueryType_Timestamp;
        queryDesc.count = MaxQueries;
        
        querySet_ = wgpuDeviceCreateQuerySet(device_, &queryDesc);
        
        if (!querySet_)
        {
            throw std::runtime_error("Failed to create WebGPU query set");
        }
    }
    
    ~WebGPUProfiler()
    {
        if (querySet_)
        {
            wgpuQuerySetRelease(querySet_);
        }
    }
    
    void BeginTimestamp(CommandBuffer<graphics_platform::webgpu>* commandBuffer, const std::string& label)
    {
        if (currentQueryIndex_ >= MaxQueries - 1)
        {
            return; // 查询集已满
        }
        
        // 记录开始时间戳
        wgpuRenderPassEncoderWriteTimestamp(
            commandBuffer->GetNativeRenderPassEncoder(),
            querySet_,
            currentQueryIndex_
        );
        
        // 保存标签
        queryLabels_[currentQueryIndex_] = label;
        currentQueryIndex_++;
    }
    
    void EndTimestamp(CommandBuffer<graphics_platform::webgpu>* commandBuffer)
    {
        if (currentQueryIndex_ >= MaxQueries)
        {
            return; // 查询集已满
        }
        
        // 记录结束时间戳
        wgpuRenderPassEncoderWriteTimestamp(
            commandBuffer->GetNativeRenderPassEncoder(),
            querySet_,
            currentQueryIndex_
        );
        
        currentQueryIndex_++;
    }
    
    std::vector<QueryResult> GetResults()
    {
        // 解析查询结果
        std::vector<u64> timestamps(currentQueryIndex_);
        wgpuQueueWriteTimestamp(
            PlatformInterface<graphics_platform::webgpu>::GetQueue(),
            querySet_,
            0,
            timestamps.size(),
            timestamps.data()
        );
        
        // 转换为结果
        std::vector<QueryResult> results;
        for (size_t i = 0; i < timestamps.size(); i += 2)
        {
            if (i + 1 < timestamps.size())
            {
                QueryResult result{};
                result.timestamp = timestamps[i + 1] - timestamps[i];
                result.label = queryLabels_[i];
                results.push_back(result);
            }
        }
        
        // 重置查询索引
        currentQueryIndex_ = 0;
        
        return results;
    }
    
private:
    static constexpr size_t MaxQueries = 1024;
    
    WGPUDevice device_ = nullptr;
    WGPUQuerySet querySet_ = nullptr;
    size_t currentQueryIndex_ = 0;
    std::array<std::string, MaxQueries> queryLabels_;
};
```

### 12.6 总结

通过集成Dawn库，我们的RHI层设计现在支持WebGPU作为第四个图形后端，实现了真正的跨平台渲染能力。WebGPU的加入不仅扩展了引擎的平台支持范围，还为未来的Web平台移植奠定了基础。结合模板化设计，开发者可以无缝地在Direct3D12、Vulkan、Metal和WebGPU之间切换，选择最适合目标平台的图形API。

本章节详细介绍了WebGPU平台特化的实现方案，包括：
1. Dawn库的集成方法和CMake配置
2. WebGPU平台接口和资源工厂的特化实现
3. WebGPU资源类（缓冲区、纹理、着色器）的详细实现
4. WebGPU命令缓冲区和同步对象的实现
5. WebGPU性能优化策略和最佳实践

这些实现方案为游戏引擎提供了完整的WebGPU支持，使开发者能够充分利用WebGPU的现代GPU功能，同时保持与现有RHI架构的一致性。通过性能优化和最佳实践，确保WebGPU后端能够提供高性能的渲染体验。