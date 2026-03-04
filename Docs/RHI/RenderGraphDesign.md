# Render Graph (Frame Graph) 架构设计文档

## 1. 概述
Render Graph (Frame Graph) 是一种用于管理渲染帧的高级架构。它将渲染过程抽象为一个有向无环图 (DAG)，其中节点代表渲染通道 (Pass)，边代表资源 (Resource) 的依赖关系。

通过 Render Graph，我们可以实现：
- **自动资源管理**: 自动创建和销毁临时资源 (Transient Resources)，优化内存使用 (Memory Aliasing)。
- **依赖分析**: 自动计算 Pass 之间的同步 (Barriers) 和资源状态转换 (Transitions)。
- **Pass 剔除**: 自动剔除对最终输出没有贡献的 Pass。
- **模块化**: 渲染逻辑被拆分为独立的 Pass，易于维护和扩展。

## 2. 核心概念

### 2.1 RGResource (渲染图资源)
代表渲染图中的一个资源（纹理或缓冲区）。它只是一个虚拟句柄，不直接持有 GPU 资源。
- **属性**:
  - `Name`: 资源名称 (调试用)。
  - `Desc`: 资源描述符 (TextureDesc / BufferDesc)。
  - `Lifetime`: 资源的生命周期 (First Pass / Last Pass)。
  - `Imported`: 是否为外部导入资源 (如 Backbuffer)。

### 2.2 RGPass (渲染图通道)
代表渲染图中的一个节点。
- **属性**:
  - `Name`: Pass 名称。
  - `Type`: Pass 类型 (`Depth`, `PrePass`, `Main`, `Post`, `Visibility` 等)。
  - `Inputs`: 输入资源列表。
  - `Outputs`: 输出资源列表。
  - `ExecuteFunc`: 执行回调函数。

### 2.3 RGBuilder (渲染图构建器)
用于在 `Setup` 阶段辅助 Pass 声明其输入输出。
- **接口**:
  - `Read(ResourceHandle)`: 声明读取资源。
  - `Write(ResourceHandle)`: 声明写入资源。
  - `CreateTexture(Name, Desc)`: 创建新的临时纹理。
  - `CreateBuffer(Name, Desc)`: 创建新的临时缓冲区。

### 2.4 RenderGraph (渲染图管理器)
核心类，负责构建和执行图。
- **接口**:
  - `AddPass(Name, SetupFunc, ExecuteFunc)`: 添加一个 Pass。
  - `ImportResource(Name, Handle)`: 导入外部资源。
  - `Compile()`: 编译图。
  - `Execute()`: 执行图。

## 3. 工作流程

### 3.1 Setup 阶段
用户代码通过 `RenderGraph::AddPass` 注册 Pass。
在 `SetupFunc` 中，Pass 使用 `RGBuilder` 声明它需要读取和写入的资源。如果需要创建新资源，也在此阶段声明。

```cpp
graph.AddPass("GBufferPass", 
    [&](RGBuilder& builder) {
        auto& data = passData;
        data.albedo = builder.CreateTexture("Albedo", ...);
        data.depth = builder.Write(depthBuffer);
    },
    [=](RenderGraphContext& context) {
        // Render commands...
    }
);
```

### 3.2 Compile 阶段
1.  **Culling (剔除)**: 从最终输出资源 (Backbuffer) 开始反向遍历，标记所有对输出有贡献的 Pass。未标记的 Pass 将被剔除。
2.  **Resource Lifetime (生命周期计算)**: 遍历活跃 Pass，记录每个资源的第一次使用和最后一次使用。
3.  **Dependency Analysis (依赖分析)**: 根据 Pass 的读写声明，计算 Pass 之间的依赖关系，并插入 Resource Barrier。
4.  **Resource Allocation (资源分配)**: 根据生命周期分配物理 GPU 资源。支持内存别名 (Aliasing)，即互不重叠生命周期的资源可以共享同一块内存。

### 3.3 Execute 阶段
1.  按拓扑序遍历 Pass。
2.  在 Pass 开始前，执行必要的 Resource Barriers。
3.  调用 Pass 的 `ExecuteFunc`。
4.  在 Pass 结束后，如果某些资源的生命周期结束，将其标记为可回收 (供后续资源复用)。

## 4. 详细设计

### 4.1 类结构

```cpp
// 资源句柄
struct RGResourceHandle {
    uint32_t index;
    uint32_t version;
};

// Pass 基类
class RGPassBase {
public:
    virtual void Execute(RenderGraphContext& context) = 0;
    // ...
};

// 具体的 Pass 封装
template<typename Data>
class RGPass : public RGPassBase {
    // ...
};

class RenderGraph {
public:
    // 添加 Pass
    template<typename Data, typename SetupFn, typename ExecuteFn>
    Data& AddPass(const std::string& name, SetupFn setup, ExecuteFn execute);

    // 编译
    void Compile();

    // 执行
    void Execute(RHICommandBuffer* cmdBuffer);
    
    // ...
};
```

### 4.2 资源状态管理
每个资源在每一时刻都有一个状态 (ResourceState)。
Compile 阶段需要推导每个 Pass 开始和结束时的资源状态，并在需要时插入 Transition Barrier。

## 5. 实现步骤
1.  **基础架构**: 实现 `RenderGraph`, `RGPass`, `RGResource` 等基本类。
2.  **构建器**: 实现 `RGBuilder` 用于资源声明。
3.  **编译逻辑**: 实现 Pass 剔除和拓扑排序。
4.  **资源管理**: 实现简单的资源分配 (暂不包含 Aliasing)。
5.  **同步**: 实现简单的 Barrier 插入。
6.  **高级特性**: 实现 Aliasing 和 异步计算 (Async Compute) 支持 (后续)。

