# 资源系统短期架构与实现规划（Short-Term Plan）

> 目标：在不改变现有渲染框架整体结构的前提下，重构 Engine 侧资源系统以支持更高效的加载与管理；在 ContentTools/C# Cooker 侧建立离线烹饪产物（含 NaniteMesh 包 .nmeshbin）规范与最小可行产线，形成“离线烹饪 + 运行时消费”的分层架构。当前重点平台为 macOS（Metal-CPP），渲染框架参考 D3D12 设计。

## 1. 背景与原则
- 项目技术栈：核心 C++17（Engine）、UI C#（Editor）、脚本 Python；构建 CMake；跨平台（Win/Linux/macOS）；渲染 API：D3D12/Vulkan/Metal。
- 当前问题：
  - 运行时直接读取原始 .png/.obj/.fbx 等格式并调用 `ContentToEngine::create_resource`，与引擎期望的“引擎包二进制格式（含元信息与分片目录）”不匹配。
  - ResourceManager 中内存预算/LRU/卸载策略未闭环；异步 IO 与 GPU 上传未严格分离；`get_gpu_resource` 语义与赋值不清。
  - Editor/C# Cooker 尚未输出 `.meshbin/.texbin`（标准包），更未输出 `.nmeshbin`（NaniteMesh 包）。
- 分层原则（短期）：
  - C# / ContentTools：离线烹饪与静态资源管理，负责导入、压缩、meshlet/cluster 构建、层级与分页、生成“引擎包”。
  - C++（Engine）：运行时资源管理（生命周期/内存/GPU 上传/流式与调度），通过统一 Loader 解析“引擎包”，并在渲染后端（Metal-CPP/D3D12/Vulkan）创建 GPU 对象。

## 2. Engine 侧模块化重构（目录与职责）
建议在 `Engine/Content/ResourceSystem/` 下拆分模块（不强制立即移动，先以命名空间/文件前缀过渡）：
- ResourceManager/（核心）：
  - 统一句柄（FreeList + generation）、状态机（Unloaded/LoadingIO/Parsed/PendingUpload/Uploading/Ready/Error）。
  - 预算/LRU/卸载策略闭环；访问统计与优先级。
  - get API 类型化访问（避免 `void*`）。
- ResourceLoader/（解析层）：
  - Loader 注册表：按扩展名与类型选择解析器。
  - 只负责把“引擎包字节”解析成标准化初始化数据（如 texture_init、geometry_init、nanite_init），不触碰 GPU。
- UploadQueue/（上传调度）：
  - 后台 IO/解码 与 前台 GPU 上传队列分离；Metal-CPP/D3D12/Vulkan 统一抽象。
  - 由渲染线程或专用上传线程绑定命令队列执行创建/拷贝。
- Formats/（公共格式定义）：
  - PackHeader/目录块/偏移索引/版本与 GUID；texbin/meshbin/nmeshbin 的结构与对齐。
- Cache/（缓存与热重载）：
  - 文件哈希与修改时间；开发模式文件监控；增量重载。
- Stats/（统计与日志）：
  - CPU/GPU 使用量、吞吐、解析/上传耗时；统一错误日志。

目录示例（后续逐步创建）：
```
Engine/Content/ResourceSystem/
  ResourceManager.h/.cpp
  ResourceLoaderRegistry.h/.cpp
  Loaders/
    TextureBinLoader.h/.cpp
    MeshBinLoader.h/.cpp
    NaniteMeshLoader.h/.cpp
  UploadQueue.h/.cpp
  Formats/
    PackCommon.h
    TextureBinFormat.h
    MeshBinFormat.h
    NaniteMeshFormat.h
  Cache.h/.cpp
  Stats.h/.cpp
```

## 3. ContentTools / C# Cooker 离线产线（短期最小可行）
- 目标：产出 `.texbin`（纹理包）、`.meshbin`（标准网格包）与 `.nmeshbin`（NaniteMesh 包）。
- 输入：`.png/.jpg/.tga/.dds`、`.obj/.fbx/.gltf`、材质 JSON/Editor .asset 元数据。
- 输出：
  - `.texbin`：包含纹理元信息（宽高、mips、格式）、各 mip 的偏移与尺寸、像素数据块；支持 Metal 的优选格式（ASTC 或 RGBA8），并可扩展多后端变体。
  - `.meshbin`：标准网格（LOD 可选），顶点/索引量化，属性（pos/normal/tangent/uv）打包，子网格目录与材质索引。
  - `.nmeshbin`：cluster（meshlet）构建（每 cluster 128–256 triangles）、法线锥、边界体（AABB/OBB/sphere 可选）、误差度量（LOD 选择用）、层级（BVH/树）、页表与分页布局、运行时快速目录与偏移索引。
- 组织：短期可在 ContentTools（C++）中先实现命令行烹饪，Editor（C#）通过 DllWrappers 调用；后续把逻辑上移到 C#，C++ 提供算法库。

## 4. 二进制包格式草案（稳定性与可扩展性）
统一说明：
- 字节序：Little-Endian；对齐：文件内结构按 4/8 字节对齐，数据块起始按 16 字节对齐（便于 GPU copy）。
- 版本：`uint32_t version`；GUID：`uint128`（或 16 字节数组）。
- 目录（Directory）与偏移表：每个数据块在目录中记录 `offset + size + type + flags`，便于运行时快速寻址。

### 4.1 通用包头（PackHeader）
```
magic      : char[8]   // "NPACK\0\0" 或 "TEXBIN\0" 等，或统一使用一个魔数 + 子类型字段
version    : uint32_t  // 数据版本
asset_type : uint32_t  // 枚举：texture/mesh/nanite_mesh/material...
guid       : uint8_t[16]
flags      : uint32_t  // 压缩/加密/平台变体标志
dir_offset : uint64_t  // 目录块文件偏移
dir_count  : uint32_t  // 目录项数量
reserved   : uint32_t  // 对齐/保留
```

### 4.2 `.texbin`（纹理包）
目录项建议包含：
- Header 块：
  - width/height/mips/array_layers/cubemap/format（Metal: ASTC_4x4 / RGBA8；D3D12: BCn；Vulkan: ETC2/ASTC 可选）。
  - mip_offsets[]/mip_sizes[]（每个 mip 的文件偏移与大小；如压缩格式则为块数据长度）。
  - row_pitch[]（可选，运行时验证）。
- PixelData 块：按照 mip 顺序存储，16 字节对齐。

### 4.3 `.meshbin`（标准网格包）
- Header：vertex_count/index_count/submesh_count/vertex_format（按 attribute mask）/index_format（16/32-bit）。
- VertexData：按 attribute 打包（pos3f, nrm10_10_10_2, tan10_10_10_2, uv16/uv32 等）；量化参数（scale/bias）记录于 Header。
- IndexData：16-bit 优先；必要时 32-bit。
- Submesh 目录：每个 submesh 记录 index_range、material_id、lod、bounding。
- 可选 LOD 目录：LOD 层级与误差阈值。

### 4.4 `.nmeshbin`（NaniteMesh 包）
- ClusterData 块：
  - cluster_count
  - 对每个 cluster：
    - triangles（索引或显式三角面列表，优先索引 + 顶点重排）
    - bounding（aabb/minmax；可选 obb；sphere）
    - normal_cone（轴 + 角度）
    - error_metric（用于 LOD 选择的几何误差）
    - adjacency（相邻 cluster id 列表，压缩存储）
    - parent/children（层级引用，或由层级块统一管理）
  - 顶点/索引量化与重排（16-bit 优先）。
- Hierarchy 块（BVH/树）：
  - 节点数组：每节点 bounding（aabb），child_range，cluster_range，lod_threshold。
  - root 指针与层次深度；可支持多根（分区）。
- PageTable 块（分页布局）：
  - page_count；每页记录包含哪些 cluster/节点；页大小目标（例如 64KB/256KB）。
  - 运行时分页与流式加载依据该表。
- Directory/Index 块：快速偏移索引，便于运行时定位 cluster/hierarchy/page 的文件偏移。

## 5. 运行时加载与上传流程（Engine）
短期状态机：
1) Unloaded：未加载；
2) LoadingIO：后台线程读取文件（支持并行）；
3) Parsed：通过 ResourceLoader 解析为初始化数据（engine_init_blob）；
4) PendingUpload：等待上传队列；
5) Uploading：渲染线程/上传线程在正确的命令队列执行创建/拷贝；
6) Ready：GPU 可用；
7) Error：失败，保留错误信息。

关键点：
- IO/解析 与 GPU 上传严格分离；Metal-CPP 在渲染或专用上传队列执行创建，避免后台线程触碰 GPU 对象。
- `get_gpu_resource` 改为类型化访问：例如 `get_texture_view(handle)`、`get_mesh_gpu_ids(handle)`，或通过 legacy_id 统一查询。
- 默认在 Ready 后释放 CPU 副本（保留元信息与哈希），可配置 `keep_cpu_copy`。

## 6. 内存预算/LRU/卸载策略（最小闭环）
- 预算：`set_memory_budget(cpu_bytes, gpu_bytes)`；`_current_cpu_usage/_current_gpu_usage` 原子更新。
- LRU：在资源被访问时更新 `last_access_time` 与 `access_weight`；周期性评估或在超限时触发评估。
- 卸载判定：
  - 超出 GPU 预算且长时间未访问；非高优先级；未被 pin；允许卸载。
  - 卸载执行：销毁 GPU 对象；释放 CPU 副本；保留基础元信息（用于懒加载）。
- 降级策略（预留）：超限时丢弃远 mip、保留近 mip；或将低优先级资源切到低分辨率。

## 7. 错误处理与日志
- 统一日志：路径、扩展名、预期类型/版本、读取大小、解析耗时、上传耗时、哈希、错误描述。
- 调试接口：`dump_all_resources()` 输出状态机分布、内存占用、访问 Top-N 列表。
- 失败恢复：标记 Error 状态并可重试；热重载时优先使用新版本。

## 8. 与现有工程的接口衔接
- ResourceAPI/Adapter：把扩展名映射迁移到引擎包（.texbin/.meshbin/.nmeshbin/.matbin）。原始 .png/.obj 等走“离线烹饪”路径。
- ContentToEngine：
  - 提供类型化 `create_*` 与 `destroy_*` 接口；返回 `id::id_type` 并提供 GPU 查询接口。
  - 对 `.nmeshbin` 初期仅解析目录与页表，不立即实现分页渲染；先能把包读入与注册，后续逐步实现 culling/paging。
- Graphics 后端：统一 UploadQueue 抽象；Metal-CPP 在主/上传队列创建与复制；与 D3D12/Vulkan 行为对齐。

## 9. 开发阶段与里程碑（4–6 周）
- 阶段 1（第 1 周）：文档与目录基础
  - 完成本规划文档；创建 `Engine/Content/ResourceSystem` 目录框架与空壳文件；定义状态机与统计接口草案。
  - 把 ResourceManager 的预算/LRU/卸载函数声明补全与占位实现（不影响现有功能）。

- 阶段 2（第 2 周）：格式与 Loader 最小版
  - 确认 `.texbin/.meshbin` 最小格式；实现 TextureBinLoader/MeshBinLoader（解析到 init 结构）。
  - 迁移 ResourceAPI/Adapter 扩展映射；原始文件必须走 Cooker（临时可在 ContentTools 命令行调用）。

- 阶段 3（第 3 周）：上传队列与类型化访问
  - 引入 UploadQueue；把 GPU 创建/上传从后台线程移到渲染线程或专用线程。
  - 实现 `get_texture_view()/get_mesh_gpu_ids()` 等类型化接口，替换 `void*`。

- 阶段 4（第 4 周）：预算/LRU/卸载闭环 + CPU副本释放
  - 完成内存预算闭环；Ready 后默认释放 CPU 副本；日志/统计完善。
  - EngineTest 增加批量加载与压力测试（自研测试框架）。

- 阶段 5（第 5–6 周）：`.nmeshbin` 初版与目录/层级
  - ContentTools/C# Cooker 实现 cluster 构建（128–256 triangles）、法线锥、边界体、误差度量、层级（BVH）与页表；输出 `.nmeshbin`。
  - Engine 侧 NaniteMeshLoader 解析头/目录/页表；注册资源并可查询目录；暂不实现分页渲染与运行时裁剪。

## 10. 验收标准与测试计划
- `.texbin/.meshbin`：加载 100+ 资源并在 Metal-CPP 正常渲染；Ready 后 CPU 副本释放；预算统计正确；LRU 卸载在超限时触发，帧稳定。
- 错误注入：损坏的包/版本不匹配/哈希错误能被识别并记录；系统不崩溃。
- `.nmeshbin`：能够在运行时解析目录与层级，提供查询接口；不要求本阶段分页渲染。
- 跨平台编译通过（至少 macOS + Windows），行为一致。

## 11. 风险与应对
- 风险：Editor 未就绪（无 `.meshbin/.texbin`）。
  - 应对：短期用 ContentTools 命令行实现烹饪，Editor 通过 DllWrapper 调用；待 Editor 接入后替换前端入口。
- 风险：后端差异（Metal-CPP 与 D3D12/Vulkan 的上传队列行为）。
  - 应对：抽象 UploadQueue 与 GPUResourceCreation，使用统一接口；在平台层适配。
- 风险：格式升级迭代。
  - 应对：严格 version 与 GUID；保留兼容路径；通过目录与偏移表避免硬编码布局。

## 12. 后续扩展（超出短期范围的方向）
- Nanite 运行时分页与裁剪：
  - 离线页表 + 运行时可见性裁剪（视锥/法线锥/屏幕误差阈值），按优先级队列进行页级加载与回收。
  - 与 ResourceManager 的优先级/预算/LRU 融合，形成“可见性驱动”的流式系统。
- 多后端纹理变体：同一 `.texbin` 内保存多个格式变体（ASTC/BCn/RGBA8），运行时挑选最优变体。
- 增量构建：基于哈希与依赖图的增量烹饪与加载。

---

附：接口与结构建议（示例，最终以工程实现为准）

```cpp
// 文件: Engine/Content/ResourceSystem/ResourceLoaderRegistry.h
// 说明: Loader 注册表，按扩展名选择加载器；仅解析，不触碰 GPU。
class IResourceLoader {
public:
    virtual ~IResourceLoader() = default;
    // 函数: parse
    // 说明: 将引擎包原始字节解析为初始化数据，写入 runtime 的 engine_init_blob（或类型化 init 结构）。
    virtual bool parse(const std::string& path, const void* raw, size_t size, ResourceRuntimeInfo* runtime) = 0;
    // 函数: version
    // 说明: 返回数据版本，便于兼容。
    virtual uint32_t version() const = 0;
};

class ResourceLoaderRegistry {
public:
    void register_loader(const std::string& ext, IResourceLoader* loader);  // 注册扩展名 -> 加载器
    IResourceLoader* get_loader(const std::string& ext) const;               // 查询加载器
};

// 文件: Engine/Content/ResourceSystem/UploadQueue.h
// 说明: 把 GPU 创建/上传从后台线程移到渲染/上传线程队列执行。
class UploadQueue {
public:
    // 函数: enqueue
    // 说明: 投递一个上传任务（创建 GPU 对象、执行拷贝）；由正确的命令队列执行。
    void enqueue(std::function<void()> task);
};

// 文件: Engine/Content/ResourceSystem/ResourceManager.h
// 说明: 预算/LRU/卸载闭环与类型化访问，避免 void*。
class ResourceManager {
public:
    void set_memory_budget(size_t cpu_budget, size_t gpu_budget); // 设置预算并触发评估
    void update_lru_cache(ResourceHandle handle);                  // 访问更新LRU
    bool should_unload_resource(ResourceHandle handle) const;      // 是否卸载

    // 类型化访问（示例）：
    TextureView get_texture_view(ResourceHandle h) const;          // 替代 void*
    MeshGpuIds  get_mesh_gpu_ids(ResourceHandle h) const;          // 返回子网格 GPU id 列表
};
```

以上方案为短期落地路径，优先解决：格式不匹配、上传调度分离、预算/LRU 闭环与类型化访问。待 `.nmeshbin` 目录/层级成型后，下一阶段将推进分页与运行时裁剪，以支撑更大规模场景与 Nanite 风格渲染。