# PCG 系统使用手册

**版本**: Phase 1 (CPU)
**日期**: 2026-06-06
**适用**: 引擎编辑器 UI 集成、工具开发、技术美术

---

## 1. 系统概述

PCG (Procedural Content Generation) 系统是一个基于有向无环图 (DAG) 的程序化内容生成框架。通过连接不同功能的节点，将标量场数据转化为场景中的实例化物体。

### 1.1 核心流程

```
场数据生成 → 散点生成 → 约束过滤 → 变换 → 网格分配 → 渲染实例
   │              │           │          │          │
   ▼              ▼           ▼          ▼          ▼
 NoiseField   FieldScatter  SDFConstraint Transform MeshAssign
 ReferenceField             DensityFilter
```

### 1.2 数据类型

系统中流转三种核心数据类型：

| 类型 | 说明 | 产生者 | 消费者 |
|------|------|--------|--------|
| **Field** (标量场) | 连续的空间采样函数，输入世界坐标返回浮点值 | NoiseFieldNode, ReferenceFieldNode | FieldScatterNode, SDFConstraintNode |
| **PointSet** (散点集) | 离散点集合 + SoA 属性数组 | FieldScatterNode | 所有过滤/变换节点, PCGInstanceBuilder |
| **PCGInstanceData** | GPU 渲染实例 (model_matrix + mesh_index) | PCGInstanceBuilder | ForwardSceneRenderer |

### 1.3 命名空间

所有 PCG 类型位于 `primal::graphics::pcg` 命名空间。

---

## 2. 图框架 (PCGGraph)

### 2.1 原理

PCGGraph 使用有向无环图 (DAG) 组织节点。执行时按拓扑排序 (Kahn's BFS 算法) 确定节点顺序，确保上游节点先于下游节点执行。

**数据传播机制**: 执行每个节点前，图引擎将上游节点输出 pin 的数据指针复制到该节点对应的输入 pin。节点通过 `inputs[i].AsField()` / `inputs[i].AsPointSet()` 读取数据。

### 2.2 基本用法

```cpp
using namespace primal::graphics::pcg;

// 1. 创建图
PCGGraph graph;

// 2. 创建并配置节点
auto noise = std::make_unique<NoiseFieldNode>();
noise->frequency = 0.05f;
noise->octaves = 4;
noise->seed = 123;
u32 noise_id = graph.AddNode(std::move(noise));

auto scatter = std::make_unique<FieldScatterNode>();
scatter->target_count = 1000;
scatter->bounds_min = {-50, 0, -50};
scatter->bounds_max = {50, 5, 50};
u32 scatter_id = graph.AddNode(std::move(scatter));

// 3. 连线: noise 输出 pin 0 → scatter 输入 pin 0 (密度场)
graph.Connect(noise_id, 0, scatter_id, 0);

// 4. 执行
graph.Execute();

// 5. 读取结果
auto* points = graph.GetOutputPoints(scatter_id);
// points->count = 实际生成的点数
// points->positions[i] = 第 i 个点的世界坐标
// points->GetAttr(i, PCGAttr::Density) = 第 i 个点的密度值
```

### 2.3 转换为渲染实例

```cpp
PCGInstanceBuilder builder;
std::vector<PCGInstanceData> instances;
builder.Build(*points, instances);

// 传递给渲染器
auto* fwd = pipeline->GetForwardRenderer();
fwd->SetPCGInstances(instances);
```

---

## 3. 节点详细说明

### 3.1 NoiseFieldNode — 程序化噪声场

**功能**: 生成基于 FBM (Fractal Brownian Motion) 的 2D Simplex 噪声标量场。

**原理 — FBM 叠加算法**:

FBM 通过叠加多个频率递增、振幅递减的噪声层来模拟自然现象的分形特征：

```
FBM(x, y) = Σ(i=0..octaves-1) [ persistence^i × Simplex2D(x × lacunarity^i, y × lacunarity^i) ]
```

- 每层 octave 频率乘以 `lacunarity` (通常 2.0)，振幅乘以 `persistence` (通常 0.5)
- `octaves` 控制叠加层数：更多层 = 更精细细节 + 更高计算成本
- 输出范围约 [-1, 1] (单层精确，多层可能超出)

**参数**:

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| frequency | f32 | 0.02 | 空间频率。越低越平滑，越高越细碎 |
| octaves | u32 | 6 | FBM 叠加层数。推荐 3-6 |
| lacunarity | f32 | 2.0 | 每层频率倍数。通常 2.0 |
| persistence | f32 | 0.5 | 每层振幅衰减。越低细节越弱 |
| seed | u32 | 0 | 随机种子。同种子 = 同输出 |

**Pin 布局**:
- Outputs: `[0]` Field — PCGNoiseField

**典型配置示例**:

| 场景 | frequency | octaves | lacunarity | persistence |
|------|-----------|---------|------------|-------------|
| 森林密度 (大块聚散) | 0.03 | 4 | 2.0 | 0.5 |
| 草地密度 (细碎变化) | 0.1 | 3 | 2.0 | 0.4 |
| 岩石分布 (中频) | 0.05 | 5 | 2.5 | 0.6 |

**Phase 1 限制**: 仅实现 `NoiseType::Simplex`。Worley 和 Ridged 噪声已声明但未实现。

---

### 3.2 ReferenceFieldNode — 外部场引用

**功能**: 从 FieldRegistry 引用已注册的标量场 (如 GlobalSDF)，转换为 PCGField 供下游节点采样。

**原理**: FieldRegistry 是全局单例，维护所有已注册场的元数据 (FieldDescriptor)。GlobalSDF 在初始化时将级联信息注册到 FieldRegistry。ReferenceFieldNode 通过 `FieldSemantic` 查询注册表，创建 PCGReferenceField 封装。

**参数**:

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| semantic | FieldSemantic | GlobalSDF | 引用的场类型 |

**Pin 布局**:
- Outputs: `[0]` Field — PCGReferenceField

**SDF 符号约定**:
- 正值 = 在表面外部 (例如地面上方)
- 负值 = 在表面内部 (例如地面下方)
- 零值 = 在表面上

**Phase 1 限制**: 无论 `semantic` 设置如何，始终返回 ground plane SDF: `SampleFloat(pos) = pos.y`。真实 GPU 场采样需要 readback，属于 Phase 2+。

---

### 3.3 FieldScatterNode — 密度场散点

**功能**: 在指定 AABB 体积内生成散点，根据密度场的值控制每个位置的概率保留率。

**原理 — Jittered Grid + Probability Retention 算法**:

1. **网格化**: 根据 `target_count` 和 bounds 范围计算 jittered grid 尺寸：
   ```
   grid_x = sqrt(target_count × range_x / range_z)
   grid_z = target_count / grid_x
   ```

2. **候选点生成**: 每个网格单元中心加上随机抖动 (jitter) 产生候选点。y 坐标固定为 bounds 中点。

3. **密度采样**: 在候选点位置采样密度场，将 [-1,1] 重映射到 [0,1]：
   ```
   density = 0.5 + 0.5 × field.SampleFloat(pos)
   ```

4. **概率保留**: 生成随机阈值，若 `threshold < density × points_per_unit_area` 则保留该点。

5. **后处理**: 写入默认属性 (Density=1, Scale=(1,1,1)) + 位置微抖动。

**参数**:

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| target_count | u32 | 1000 | 近似候选点数 (实际可能因网格取整略少) |
| bounds_min | v3 | (-50,0,-50) | 散点区域最小角 |
| bounds_max | v3 | (50,5,50) | 散点区域最大角 |
| seed | u32 | 42 | 随机种子 |
| points_per_unit_area | f32 | 1.0 | 全局接受概率乘数 |

**Pin 布局**:
- Inputs: `[0]` Field (可选) — 密度场。未连接则均匀散布
- Outputs: `[0]` PointSet — 散点 + Density 属性

**密度解释**:
- 有密度场连接: [-1,1] → [0,1] 重映射，高密度区域保留更多点
- 无密度场连接: density = 1.0 (均匀分布)

**典型输出**: `target_count=2000` 加噪声密度场通常产出 800-1200 个点 (约 50% 被概率淘汰)。

---

### 3.4 SDFConstraintNode — SDF 距离约束

**功能**: 根据有符号距离场过滤散点，只保留 SDF 距离在指定范围内的点。

**原理**: 遍历所有输入点，在每点位置采样 SDF 场。SDF 值表示该点到最近表面的带符号距离。只保留 `[min_dist, max_dist]` 范围内的点。

**参数**:

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| min_dist | f32 | 0.1 | 最小距离 (inclusive) |
| max_dist | f32 | 1000.0 | 最大距离 (inclusive) |

**Pin 布局**:
- Inputs: `[0]` PointSet — 候选点
- Inputs: `[1]` Field — SDF 距离场
- Outputs: `[0]` PointSet — 过滤后的点

**典型用法**:

| 场景 | min_dist | max_dist | 效果 |
|------|----------|----------|------|
| 地面上方 0.5m ~ 100m | 0.5 | 100.0 | 剔除地下和贴近地面的点 |
| 地面附近 ±0.2m | -0.2 | 0.2 | 仅保留地面表面附近的点 |
| 远离建筑 2m+ | 2.0 | 1000.0 | 剔除贴近建筑表面的点 |

---

### 3.5 DensityFilterNode — 密度属性过滤

**功能**: 根据 Density 属性值过滤散点，移除密度过低或过高的点。

**原理**: 遍历输入点集的 Density 属性，保留 `[min_density, max_density]` 范围内的点。过滤通过原地压缩实现 (双指针法)，避免额外内存分配。

**参数**:

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| min_density | f32 | 0.0 | 最小密度 (inclusive) |
| max_density | f32 | 1.0 | 最大密度 (inclusive) |

**Pin 布局**:
- Inputs: `[0]` PointSet — 带 Density 属性的散点
- Outputs: `[0]` PointSet — 过滤后的点

**典型用法**: 放在 FieldScatterNode 之后，设置 `min_density=0.3` 移除低密度区域的孤立散点，使分布更自然。

---

### 3.6 TransformNode — 随机变换

**功能**: 为每个散点赋予随机缩放和位置抖动。

**原理**: 为每个点生成三轴独立的均匀随机缩放值 (在 [scale_min, scale_max] 范围内)，写入 ScaleX/Y/Z 属性。若 position_jitter > 0，额外在 x/z 方向施加随机位移。

**参数**:

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| scale_min | v3 | (0.8,0.8,0.8) | 三轴最小缩放 |
| scale_max | v3 | (1.2,1.2,1.2) | 三轴最大缩放 |
| rotation_range | f32 | 2π | 最大旋转角度 (Phase 1 未应用到矩阵) |
| position_jitter | f32 | 0.0 | 水平位置抖动幅度 |
| seed | u32 | 0 | 随机种子 |

**Pin 布局**:
- Inputs: `[0]` PointSet — 输入散点
- Outputs: `[0]` PointSet — 带缩放属性的散点

**典型用法**:

| 场景 | scale_min | scale_max | jitter |
|------|-----------|-----------|--------|
| 树木 (高度变化大) | (0.3,0.5,0.3) | (1.0,2.5,1.0) | 0.5 |
| 岩石 (均匀缩放) | (0.5,0.5,0.5) | (1.5,1.5,1.5) | 0.2 |
| 草 (很矮，微小变化) | (0.8,0.3,0.8) | (1.2,0.6,1.2) | 0.1 |

**Phase 1 限制**: `rotation_range` 参数存在但未应用到输出矩阵。

---

### 3.7 MeshAssignNode — 网格分配

**功能**: 通过加权随机为每个散点分配 mesh_index，控制不同网格类型的比例。

**原理 — 累积分布采样 (CDF Inversion)**:

1. 构建 weights 的累积分布: `cumulative[i] = Σ(weights[0..i])`
2. 对每个点生成 [0, total_weight) 均匀随机值
3. 在累积分布中查找第一个 ≥ 随机值的槽位 (线性搜索)
4. 将槽位索引写入 MeshIndex 属性

**参数**:

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| weights | vector\<f32\> | {1.0} | 每个 mesh 槽位的权重 |

**Pin 布局**:
- Inputs: `[0]` PointSet — 输入散点
- Outputs: `[0]` PointSet — 带 MeshIndex 属性的散点

**权重示例**:

| weights | 预期比例 | 说明 |
|---------|----------|------|
| {1.0} | 100% / 0% | 所有实例使用同一个网格 |
| {0.7, 0.3} | 70% / 30% | 两种网格，7:3 比例 |
| {0.5, 0.3, 0.2} | 50% / 30% / 20% | 三种网格混合 |

**mesh_index 与渲染的关系**: `mesh_index` 对应 `ForwardSceneRenderer` 中加载的网格列表索引。实例渲染时根据 `PCGInstanceData::mesh_index` 选择绘制哪个网格。

---

### 3.8 PCGInstanceBuilder — 实例构建

**功能**: 将 PCGPointSet 转换为 GPU 可消费的渲染实例数据。

**原理**: 遍历散点集，为每个点构建 4×4 模型矩阵：

```
model_matrix = Scale(sx, sy, sz) × Translation(px, py, pz)
```

从 ScaleX/Y/Z 属性读取缩放，从 positions 读取位置。mesh_index 从 MeshIndex 属性读取。

**Phase 1 限制**: 不应用旋转。仅编码缩放 + 平移。

---

## 4. 完整图管线示例

### 4.1 森林场景 (1000 棵树 + 多种类)

```cpp
PCGGraph graph;

// 节点 0: 地面 SDF 引用
auto ref = std::make_unique<ReferenceFieldNode>();
u32 n0 = graph.AddNode(std::move(ref));

// 节点 1: 噪声密度场
auto noise = std::make_unique<NoiseFieldNode>();
noise->frequency = 0.03f;  // 大块聚散
noise->octaves = 4;
noise->seed = 42;
u32 n1 = graph.AddNode(std::move(noise));

// 节点 2: 体积散点
auto scatter = std::make_unique<FieldScatterNode>();
scatter->target_count = 2000;
scatter->bounds_min = {-40, 1, -40};
scatter->bounds_max = {40, 5, 40};
scatter->seed = 42;
u32 n2 = graph.AddNode(std::move(scatter));

// 节点 3: SDF 约束 (保留地面以上)
auto sdf = std::make_unique<SDFConstraintNode>();
sdf->min_dist = 0.5f;
sdf->max_dist = 100.0f;
u32 n3 = graph.AddNode(std::move(sdf));

// 节点 4: 密度过滤 (移除稀疏孤立点)
auto dfilter = std::make_unique<DensityFilterNode>();
dfilter->min_density = 0.3f;
u32 n4 = graph.AddNode(std::move(dfilter));

// 节点 5: 随机变换
auto xform = std::make_unique<TransformNode>();
xform->scale_min = {0.5f, 1.0f, 0.5f};
xform->scale_max = {1.5f, 3.0f, 1.5f};
xform->position_jitter = 0.3f;
xform->seed = 99;
u32 n5 = graph.AddNode(std::move(xform));

// 节点 6: 网格分配 (70% 树, 30% 灌木)
auto mesh = std::make_unique<MeshAssignNode>();
mesh->weights = {0.7f, 0.3f};
u32 n6 = graph.AddNode(std::move(mesh));

// 连线
graph.Connect(1, 0, 2, 0);  // Noise → Scatter (密度)
graph.Connect(2, 0, 3, 0);  // Scatter → SDF (点集)
graph.Connect(0, 0, 3, 1);  // RefField → SDF (距离场)
graph.Connect(3, 0, 4, 0);  // SDF → DensityFilter
graph.Connect(4, 0, 5, 0);  // DensityFilter → Transform
graph.Connect(5, 0, 6, 0);  // Transform → MeshAssign

graph.Execute();

// 构建渲染实例
auto* points = graph.GetOutputPoints(n6);
PCGInstanceBuilder builder;
std::vector<PCGInstanceData> instances;
builder.Build(*points, instances);

// 提交渲染
pipeline->GetForwardRenderer()->SetPCGInstances(instances);
```

**预期输出**: 约 800-1200 个实例，自然聚类分布，70% 为树 (mesh 0)，30% 为灌木 (mesh 1)。

---

## 5. UI 层集成指南

### 5.1 Graph Editor 映射

每个 C++ 节点类映射为 UI 中的一个节点类型。UI 层需要：

| C++ 类 | UI 节点名 | 分类 | 颜色建议 |
|--------|-----------|------|----------|
| NoiseFieldNode | Noise Field | Field Generators | 橙色 |
| ReferenceFieldNode | Reference Field | Field Generators | 橙色 |
| FieldScatterNode | Field Scatter | Scatter | 绿色 |
| SDFConstraintNode | SDF Constraint | Filters | 蓝色 |
| DensityFilterNode | Density Filter | Filters | 蓝色 |
| TransformNode | Transform | Transforms | 紫色 |
| MeshAssignNode | Mesh Assign | Output | 灰色 |

### 5.2 Pin 可视化

| Pin 类型 | 颜色 | 连接规则 |
|----------|------|----------|
| Field (输出/输入) | 橙色圆点 | Field → Field 输入 或 Scatter/SDF 约束输入 |
| PointSet (输出/输入) | 青色圆点 | PointSet → PointSet 输入 |

### 5.3 参数控件映射

| C++ 参数类型 | UI 控件 | 附加信息 |
|-------------|---------|----------|
| f32 (单值) | Slider + 数值输入 | 需提供 min/max/default |
| u32 (整数) | 数值输入 + Spinner | 需提供 min/max |
| v3 (向量) | 3 × Slider/输入 | 分别标注 X/Y/Z |
| vector\<f32\> (权重) | 动态列表 | 每项一个 Slider，总和显示 |
| FieldSemantic (枚举) | 下拉选择 | 列出可用场类型 |

### 5.4 节点参数面板定义

#### NoiseFieldNode
```
┌─ Noise Field ────────────────────┐
│ Frequency:   [===|=====] 0.050   │
│ Octaves:     [==|=======] 4      │
│ Lacunarity:  [===|=====] 2.000   │
│ Persistence: [==|=======] 0.500  │
│ Seed:        [42        ]        │
└──────────────────────────────────┘
```

#### FieldScatterNode
```
┌─ Field Scatter ──────────────────────────┐
│ Target Count: [2000      ]               │
│ Bounds Min:   X[-40.0] Y[1.0]  Z[-40.0] │
│ Bounds Max:   X[40.0]  Y[3.0]  Z[40.0]  │
│ Density Scale:[===|=====] 1.000          │
│ Seed:         [42        ]               │
├─ Inputs ────────────────────────────────┤
│ [0] Density Field  ○── (optional)       │
└──────────────────────────────────────────┘
```

#### SDFConstraintNode
```
┌─ SDF Constraint ─────────────────┐
│ Min Distance: [===|=====] 0.500  │
│ Max Distance: [========|=] 100.0 │
├─ Inputs ─────────────────────────┤
│ [0] Point Set   ●── (required)   │
│ [1] SDF Field   ●── (required)   │
└──────────────────────────────────┘
```

#### DensityFilterNode
```
┌─ Density Filter ─────────────────┐
│ Min Density: [==|=======] 0.300  │
│ Max Density: [========|=] 1.000  │
├─ Inputs ─────────────────────────┤
│ [0] Point Set   ●── (required)   │
└──────────────────────────────────┘
```

#### TransformNode
```
┌─ Transform ───────────────────────────────────┐
│ Scale Min: X[0.30] Y[0.30] Z[0.30]            │
│ Scale Max: X[1.00] Y[2.00] Z[1.00]            │
│ Rotation Range: [========|==] 6.283 (2π)      │
│ Position Jitter: [==|=======] 0.000            │
│ Seed:           [99        ]                   │
├─ Inputs ───────────────────────────────────────┤
│ [0] Point Set   ●── (required)                 │
└────────────────────────────────────────────────┘
```

#### MeshAssignNode
```
┌─ Mesh Assign ─────────────────────────────────┐
│ Weights:                                      │
│   [0] Mesh Slot 0:  [====|====] 0.700         │
│   [1] Mesh Slot 1:  [==|======] 0.300         │
│   [+ Add Slot]                                 │
│ Total: 1.000                                   │
├─ Inputs ───────────────────────────────────────┤
│ [0] Point Set   ●── (required)                 │
└────────────────────────────────────────────────┘
```

### 5.5 图序列化格式 (建议)

```json
{
  "version": 1,
  "nodes": [
    {
      "id": 0,
      "type": "ReferenceField",
      "params": { "semantic": "GlobalSDF" }
    },
    {
      "id": 1,
      "type": "NoiseField",
      "params": { "frequency": 0.05, "octaves": 4, "seed": 123 }
    },
    {
      "id": 2,
      "type": "FieldScatter",
      "params": { "target_count": 2000, "bounds_min": [-40,1,-40], "bounds_max": [40,3,40], "seed": 42 }
    }
  ],
  "connections": [
    { "from_node": 1, "from_pin": 0, "to_node": 2, "to_pin": 0 }
  ]
}
```

### 5.6 执行结果反馈

执行后 UI 应显示：

```
┌─ Execution Result ──────────────────┐
│ ✓ Graph executed successfully       │
│                                     │
│ Node statistics:                    │
│   FieldScatter  → 997 points        │
│   SDFConstraint → 997 points (0 filtered) │
│   DensityFilter → 997 points (0 filtered) │
│   Transform     → 997 points        │
│   MeshAssign    → 997 points        │
│     mesh 0: 709 (71.1%)             │
│     mesh 1: 288 (28.9%)             │
│                                     │
│ Total instances: 997                 │
│ Execution time: 2.3ms               │
└─────────────────────────────────────┘
```

---

## 6. SDF 符号约定

PCG 系统中所有 SDF (Signed Distance Field) 遵循同一符号约定：

| SDF 值 | 含义 | 示例 (ground plane at y=0) |
|--------|------|---------------------------|
| **正值** | 在表面外部 | y > 0: 地面以上 |
| **零** | 在表面上 | y = 0: 地面表面 |
| **负值** | 在表面内部 | y < 0: 地面以下 |

此约定保证：
- `SDFConstraintNode(min=0.5)` 只保留地面上方 0.5m 以上的点
- `SDFConstraintNode(min=-0.2, max=0.2)` 只保留地面表面附近的点

---

## 7. 性能特征

### Phase 1 (CPU) 性能参考

| 操作 | 规模 | 耗时 |
|------|------|------|
| 7 节点图执行 | 2000 候选 → 997 实例 | < 5ms |
| PCGInstanceBuilder | 1000 实例 | < 1ms |
| 渲染 (loop-draw) | 1000 实例 | 60fps |

### 瓶颈

- Phase 1 使用 loop-draw (每个实例一次 Draw call)，1000+ 实例可能成为 GPU 瓶颈
- `std::srand()` 全局状态不线程安全，当前不支持并行节点执行
- FBM 采样是计算密集点，octaves 越高越慢

---

## 8. 已知限制与 Phase 2 展望

| 限制 | Phase 2 计划 |
|------|-------------|
| ReferenceField 用占位 SDF | 接入真实 GlobalSDF GPU readback |
| 仅 Simplex 噪声 | 添加 Worley / Ridged 噪声 |
| Transform 无旋转 | 在 PCGInstanceBuilder 中应用 Y 轴旋转 |
| loop-draw 渲染 | 改为 instanced draw (一次 Draw call) |
| 纯 CPU | GPU compute shader 加速散点/过滤 |
| 无序列化 | 引入 JSON/binary 图序列化 |
| 无增量更新 | 参数变更时全量重算 |
