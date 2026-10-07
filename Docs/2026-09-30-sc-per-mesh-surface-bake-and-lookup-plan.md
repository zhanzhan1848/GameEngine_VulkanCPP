# SC per-mesh 表面烘焙、所有权与射线 lookup 改造规划

日期：2026-09-30。状态：**规划，未实施**。

本文件记录侧讨论形成的工程方案。首个适用范围为静态 Sponza；不表示当前问题已经修复，不替代正在进行的诊断阶段与验收记录。本轮只形成文档，不修改渲染代码、候选默认值或主线程实验状态。

## 1. 目标与决策

目标是建立可验证的 `mesh → card → 表面 texel → atlas` 对应关系，使 SC 查询获得所属 mesh 的真实表面属性，再独立处理 SDF 命中误差与 DDGI 更新、历史问题。

已采纳的设计方向：

1. **SC card 必须 per-mesh 烘焙。** 静态资产离线生成布局及真实表面数据，而不只保存位置、范围、方向。
2. **card → mesh 必须严格限制。** card 的表面数据只能来自其 owner mesh；实例化后还必须区分 owner instance。
3. 静态 albedo、normal、材质参数、depth/位置、有效 mask 离线烘焙；运行时更新 radiance，处理实例变换、页分配和明确的失效事件。
4. bbox 用于运行时射线候选筛选；结合实际 depth、mask、texel 覆盖范围选择最近的有效表面。
5. 四叉树与 Morton/Z-order 用于 atlas 管理、局部性优化；card 数量根据尺寸、目标 texel 密度和表面复杂度调整。
6. SDF 命中精度、同 mesh 多层表面、DDGI 改进分别立项验证。任何一项不能仅凭画面变暗认定修复。

## 2. 当前依据与证据边界

以下为既有源码和阶段记录反映的问题；实施前需要固定当前 revision、dirty diff、运行参数及数据版本，重新核对适用性。

| 项目 | 已有依据 | 本规划中的含义 |
|---|---|---|
| 离线 card | `ContentTools/common/CardBake.h` 当前生成布局及 surfel/覆盖信息，并非完整材质纹理页 | 完整表面数据烘焙属于新增能力 |
| runtime capture 所有权 | `SurfaceCachePass.cpp` 中 `PRIMAL_SC_CAPTURE_OWNER` 为可选开关；`GPUDrivenDrawPipeline.cpp` 的 owner 过滤受该开关控制 | 布局属于某 mesh 不等于 capture 自动只绘制该 mesh |
| foreign writer | [0330 handoff](../autoresearch/classic-260930-0330/SESSION_HANDOFF.md) 记录 owner 17/76 的 card 中出现 instance 7 的 writer | 应建立写入约束与可审计身份，不能只限制 lookup 的候选列表 |
| SDF 伪早命中 | [1441 记录](../autoresearch/classic-260928-1441/README.md) 区分 AABB 距离下界与真实三角形距离 | 下界只能指导步进/候选，不能认证真实表面 |
| padding/decode | [1454 记录](../autoresearch/classic-260928-1454/README.md) 记录编码使用未 padding 的几何跨度、解码使用 padded grid 跨度的距离尺度不一致 | padding 本身不是错误；坐标范围与距离编码尺度须分开定义 |
| trace/SC 对应 | [trace-distance 记录](../autoresearch/classic-260930-sc-trace-distance/README.md) 记录新鲜更新链中的早命中距离差 | 正确 owner 不能把错误 hit point 自动变成真实首交点 |
| atlas 分配 | 0330 记录中排序改善覆盖，但完整画面门禁失败 | packing 优化与表面正确性分别验收，不能将覆盖提升等同于画质通过 |
| 410 history | [history-lineage 记录](../autoresearch/classic-260930-history-lineage/README.md) 单列解析 history 路径 | 不归入“新鲜射线材质变化”的证据 |

继续遵守 [原阶段 handoff](../autoresearch/classic-260928-1748/SESSION_HANDOFF.md) 的证据边界。历史文档标注完成不意味着本次发现的所有权、命中及 lookup 问题已解决。

## 3. 必须成立的数据契约

### 3.1 身份与关联

建议采用以下逻辑关联；这是设计契约，不是已经实现的 ABI：

```text
MeshAssetID + GeometryVersion
  └─ CardID + LayoutVersion
      └─ LocalSurfacePages + MaterialVariantVersion

InstanceID → MeshAssetID + TransformVersion + MaterialVariant
(InstanceID, CardID) → AtlasPageHandle + PageGeneration
AtlasPageHandle → SurfaceDataReference + RuntimeRadiance
```

- CardID 在 mesh 资产范围内定义，owner 不能被 atlas 槽位复用隐式改变。
- 同一 mesh 的多个实例可以共享静态表面数据，但各自的世界变换、radiance 和必要历史应明确隔离。
- Atlas 地址属于存储映射，不能作为几何身份；释放/复用后必须通过 generation 或等价机制拒绝旧引用。
- 烘焙纹理使用 card 局部 UV，运行时 atlas placement 不改变其几何语义。
- 材质覆盖、几何版本、布局版本不匹配时，应失效重建或进入明确 fallback，不读取旧页冒充新数据。

### 3.2 表面写入硬限制

- 离线捕获只提交 owner mesh 的几何与对应材质；运行时重捕获只提交 owner instance。
- 邻近 mesh 可以影响光照、遮挡和 radiance，但不能成为该 card 的 albedo、normal、depth 或有效 mask 的来源。
- 禁止通过全局 SDF 加 owner 的中心 UV 材质填充，把空 texel 标记为有效表面。
- 无真实覆盖的 texel 保持无效。缺页、未烘焙、版本不匹配与被遮挡等状态必须可区分。
- owner-only 不能仅作为诊断开关；最终生产路径必须建立不可绕过的约束。

### 3.3 离线页内容

最小数据集：实际表面覆盖 mask、约定坐标系下的 depth 或 position、normal、albedo、roughness/metallic 等实际使用的材质参数、emissive 及必要材质标志。

实施前固定颜色空间、单位、normal 空间、depth 的方向/原点/符号、投影范围、裁剪与背面策略。材质 flag 保持整数位语义，不经浮点数值转换解释。

primitive/source 标签可用于烘焙验证和诊断；**不要求生产中每条射线重新遍历原生三角形**。烘焙存储精度与滤波 footprint 需要单独规定误差预算。

## 4. 离线与运行时职责

| 时机 | 职责 | 限制 |
|---|---|---|
| 离线 | per-mesh 生成 card 布局，捕获真实几何/材质页，验证覆盖与身份 | 不捕获场景内其他 mesh；不把动态光照固化为静态 radiance |
| 运行时实例化 | 变换 card/bbox，绑定静态页，分配 atlas，建立 owner/generation | 非均匀缩放使用正确 normal 变换；负缩放明确方向约定 |
| 运行时光照 | 更新 radiance、遮挡及需要的时序状态 | 静态表面身份与光照变化分开 |
| 运行时重捕获 | 处理明确的几何/材质失效或支持的动态对象 | 必须 owner-only，替换后使依赖历史失效 |

用户提出的“运行时烘焙并按 bbox 获取更近 card”在这里拆为：静态表面默认离线烘焙；运行时使用这些 depth/位置做对应校验，并允许在明确失效时 owner-only 重捕获。若选择全部运行时捕获作为中间实现，它同样必须满足上述身份、覆盖和版本契约，且不能被称为已完成离线表面烘焙。

第一阶段优先静态、受支持材质的 Sponza。透明/alpha-test、变形、动画、材质动态参数与程序几何须列出支持范围及 fallback；不默认离线静态页适用于这些对象。

## 5. Card 数量与 atlas 优化

Card 数量以 mesh bbox 尺寸和目标世界 texel 密度为起点，结合投影覆盖、法线分布、凹面、多层遮挡和最大资源预算。**bbox 大小不能单独决定足够的 card 数量。**

静态布局优先在离线阶段调整。运行时动态增加 card 属于后续可选能力：必须同步定义新 ID、布局版本、页分配、上传完成及历史失效，否则不纳入首阶段。

- 二维四叉树/buddy allocator：管理 atlas 块、空闲空间与连续分配。
- Morton/Z-order：可用于 texel/page 排序及访问局部性；不保证几何命中正确。
- 三维射线候选加速：使用 mesh/card bbox 的 BVH、网格或适合场景的结构；不能用二维 atlas 四叉树替代三维求交。
- 先完成正确性，再比较 packing、访问成本与覆盖率。避免一次候选同时修改 card 数量、分配策略和 lookup。

## 6. 运行时 lookup：最近的有效表面

**最近 bbox、最近 card 平面或最近 card 中心，都不等于最近真实表面。** bbox 求交负责 broad phase；card 的 depth/位置与 mask 负责表面校验。

建议流程：

1. 以射线与 mesh/card bbox 求交，得到正向候选区间；近到远遍历。
2. 校验 owner instance、资产/布局版本与 atlas generation。
3. 在 card 投影域求候选 UV，读取有效 mask 和 depth/position，重建其真实表面位置。
4. 求表面在射线上的参数 `t_surface`，校验正向性、横向残差、投影覆盖与 texel footprint；需要时做有界细化。单次平面投影再读 depth 不保证完成求交。
5. 使用约定的法线/背面与深度规则剔除不对应的表面，选择最小有效正向 `t_surface`。
6. 只有剩余候选的保守进入距离大于当前有效交点时，才可提前终止。
7. 无有效匹配则明确报告未支持/未命中；不能仅选一个 card 就声明其绝对正确。

同 mesh 内不同层表面需要额外 card、布局拆分或有界多层 depth 表示。单层投影不能恢复被烘焙遮挡的层；有效 mask 与宽 depth tolerance 也不能代替缺失的几何信息。

“自动获取更近 card”是实现目标，须通过同 mesh 多层、相邻 mesh、薄墙、凹面与低分辨率 footprint 的对照证明。

## 7. 剩余问题一：SDF hit point

“可能是精度问题”保留为待测假设，但目前不能将全部错误归为分辨率不足。至少分开处理：

- **语义错误**：AABB 距离下界被当作实际表面距离/命中，source 标签被当作真实命中 owner。
- **编码尺度错误**：padding 的坐标域与距离归一化尺度混用。
- **数值近似**：voxel 分辨率、插值、量化、步进阈值和法线估计误差。
- **表示限制**：薄面、多层与缺失几何，可能需要改变表示或有界回退。

正确 SC 只保证候选表面数据可信，不能修正任意错误 hit point。评估 SDF 作为步进/候选生成器，结合可信 card 表面校验；不能在错误点附近随意选 card 视为修复。

测试需在固定射线集合比较原生三角形首交点、SDF 点和 SC 重建点，记录距离误差、横向误差、owner/层匹配、miss/false-hit 与成本。原生三角形用于诊断基准，是否进入生产回退路径待性能证据决定。只改变一个分辨率/阈值/语义因素，分别判断原因。

## 8. 剩余问题二：DDGI 后续研究

本次先登记研究任务，尚未新增网上调研结论，也不宣称 Unreal 的具体实现已核验。

后续优先查官方资料、公开原始论文及可访问的 Unreal 源码/文档，结合现有 [DDGI 网格参考](2026-09-16-ddgi-world-space-probe-grid-reference.md) 与 [Lumen 分析笔记](2026-09-07-lumen-source-analysis-notes.md)：

1. 核查 DDGI 与 Unreal 相关 GI 路径的职责差异，哪些机制可以迁移。
2. Probe relocation/classification、距离可见性、漏光与背面处理如何依赖可靠射线命中。
3. 更新调度、有效性、历史拒绝及收敛如何处理缺失更新、资源版本变化和 probe 移动。
4. SC radiance 查询如何验证几何支持，以及失败查询是否污染 irradiance/history。
5. 各方案资源、射线数量、性能与 Vulkan/MoltenVK 可实现性。

研究产物必须包含原始来源链接、对应代码位置、适用条件和有界实验；不直接照搬参数。不以全局 GI 压暗、天空屏蔽或 ROI 系数补偿替代源头修复。

## 9. 分阶段实施与验收

| 阶段 | 交付 | 进入下一阶段的条件 |
|---|---|---|
| P0 | 固定当前基线、身份/坐标/版本契约、支持范围与资源预算 | 已有证据可复现且合同无歧义 |
| P1 | per-mesh 离线真实表面页、格式版本与烘焙审计 | owner、mask、depth、材质在资产样本上通过对照 |
| P2 | 运行时实例绑定、上传、atlas generation、owner-only fallback 与失效 | 外来 mesh 无法写入静态表面，页复用拒绝旧引用 |
| P3 | card 密度/覆盖与 atlas 优化，逐个单变量候选 | 正确性保持，内存与耗时满足预先登记预算 |
| P4 | bbox 候选与最近有效表面 lookup；SDF 误差分类实验 | 首交点/层/owner 错误被量化并有针对性改善 |
| P5 | DDGI 官方资料研究与更新/历史有界候选 | 新鲜源头与 history 证据独立成立 |
| P6 | 集成、完整画面门禁、性能记录与 keep/discard | 所有要求同时满足，才考虑改变默认值 |

P3 的 packing、P4 的 SDF 和 lookup、P5 的 DDGI 分别实验；不得同时打开多项后归因。任何候选初始默认关闭；失败保持默认不变。

### 9.1 必须覆盖的场景

同资产多实例、邻近外来 mesh、同 mesh 多层/凹面、薄墙、card 边界与无效 texel、低分辨率/mip 滤波、非均匀与负缩放、材质变体、页释放复用、更新缺失及重捕获失效。未支持材质要有明确结果，不能通过伪有效 texel 静默通过。

### 9.2 证据与画面门禁

- 诊断读回必须认证 GPU 提交、完成与 buffer 的同步/可见性；Vulkan `InsertBarrier` 跳过 buffer 时不能用它证明新鲜数据。
- `DDGI_DUMP=2` 诊断采用实际 PATH_DEBUG card/texel 标签，射线与同帧 update list、relocated origins、SC atlas 独立对账。
- 409/410/507 区分颜色变化、lookup 变化、几何/深度错配；410 解析 history 单列。缺失更新不能用旧 ray 替代。
- 保持原 **350/480 两帧、完整六 ROI** 门禁：相对基线 R arcade 与 floor 的参考误差各改善至少 1，另外四 ROI 各恶化不超过 2；沿用原 ROI 顺序、参考值 `[162, 22.5, 8.9, 120.5, 87, 59.5]`、相机、运行参数及统计定义。
- 正式画面评估使用生产路径；诊断输出不能污染待测 radiance 或替代正式运行。源头正确性通过不等于六 ROI 通过，反之也一样。
- 记录烘焙大小、静态页与实例 radiance 内存、card 数、覆盖、失败查询、capture/lookup/GI 耗时；先测基线、预登记预算，再验收。当前无数据支持承诺节省多少性能。
- 每阶段保存实际 diff、原始数据、运行参数、验证结果、keep/discard 与下一阶段 handoff。遵循仓库 RHI 文档与 Vulkan 构建树约束，不提交/推送，不混用构建树。

## 10. 实施前仍需确定

1. 静态页格式及压缩精度；depth 还是 position，单层还是受限多层。
2. 原生材质求值与烘焙结果的颜色空间、flag、normal 一致性。
3. 初始支持的材质类别、变形对象 fallback 与失效触发点。
4. texel footprint 容差、首交点误差预算、候选上限和失败语义。
5. 共享静态页与实例 radiance 的资源布局，以及多后端实现边界。
6. 离线 card 数上限、运行时可选扩展与性能预算。

下一步先完成 P0 合同和 P1 小范围资产样本方案；本文件不授权侧对话启动主线程代码实验。SDF/lookup/DDGI 的最终原因与改进效果仍以独立证据和完整门禁判定。
