# PFX 架构设计 —— Niagara 级可编程粒子框架 × Houdini POP 能力 × 引擎松耦合集成

- 日期：2026-09-12
- 定位：**取代** `Docs/completed/2026-09-12-particle-system-upgrade-plan.md` 的架构层结论（该文档降级为"存量系统过渡路径"参考，其 P0 的句柄/死亡通知设计被本文 N0 吸收）。
- 前置调研：同日完成的 `Engine/Particles/` 全量盘点（能力清单与已知问题见 upgrade-plan §0，本文不重复）。
- 关联文档：`Docs/2026-09-11-physics-simulation-design.md`（Jolt 接线纪律、PipelineTick、确定性门禁）、`Docs/completed/2026-09-11-physics-simulation-survey.md` §2.4（流体求解器归属）。
- 本文回答： Niagara 的架构范式如何在本引擎落地（非搬运）、Houdini POP 系列能力映射到什么载体、与引擎各系统（ECS/物理/序列器/GI/编辑器/CLI）的**松耦合**接口长什么样、N0–N7 里程碑怎么排。
- **复审记录（2026-09-12 同日）**：用户决议两项——①v1 编辑器形态 = JSON 资产 + 表单 Inspector（节点图延后）；②N5 GPU 图发射器**无期限延后**。代码库事实核查修正四项——①`PipelineTick` 已存在（`EngineDLL/PhysicsAPI.cpp:177`，物理 M0 已落地，原"待新增"表述作废）；②`particles::update` 生产宿主从未调用（仅集成测试驱动），N0 补生产 tick 接线；③引擎 C++ 无 JSON 库，N1 补 vendor 决议；④`mesh::mesh_id` + `RenderMesh.h` 确认存在，N2 引用具体化。编辑归属决议一项——⑤**文档在 UI（UndoRedo/文件），schema 反射在引擎（`PfxGetEmitterSchema`），引擎不做结构性编辑导出**（§6）。
- **当前状态（2026-09-13）**：N0 框架骨架**已完成并通过全部门禁**（交付物、门禁结果与实现偏差见 §10 实施状态）；下一里程碑 N1。

---

## 0. 一页结论

1. **新建框架，不改造旧系统**：新框架 `Engine/ParticleFX/`（命名空间 `primal::pfx`，下称 PFX）与现有 `Engine/Particles/` 并行运行，旧系统进入冻结维护；迁移是单向单向转换（preset → PFX 资产），PFX 稳定后整体删除旧系统。大爆炸重写活系统是引擎项目最常见的死法，明确不走。
2. **数据模型对齐 Houdini**：粒子 = 属性表（SoA 通道集），不是 80 字节定长结构体。这是三个诉求（Niagara 参数系统 / POP 任意属性 / 刚体 payload）共享的同一块地基。
3. **编程模型对齐 Niagara**：发射器 = 阶段化模块堆栈（spawn/init/update/render），模块是原生 C++ 注册表项 + 参数块；GPU 侧先做标准模块子集的手写 kernel 映射，不做全量 HLSL 翻译器。
4. **耦合全部走接口**：PFX 核心只依赖 `Utilities/` + `JobSystem`；物理、RHI、资产、编辑器各经一个窄接口适配层接入。依赖方向单向，Jolt/Avalonia 头文件永远不出各自主层（沿用物理文档铁律）。
5. **Houdini 联动走离线管线**：Alembic 顶点/点缓存回放 + VDB 体积场离线转换，经 `ContentTools` 烘焙——与本仓库"离线优先"惯例（Nanite SDF、card bake）同构。运行时 SPH/PBD 流体求解器不在本计划（物理调研 §2.4 已立项归属），但架构里预留约束求解模块槽位。

---

## 1. 目标架构总图

```
┌─ 编辑器 Avalonia ────────────┐   ┌─ 宿主 PipelineTick（已存在：PhysicsAPI.cpp:177）────┐
│ 发射器资产(JSON)内嵌编辑       │   │  PipelineTick(dt)：物理 step → pfx::tick（已接线）  │
│ 曲线/渐变编辑器(复用)          │   │  渲染帧：RenderGraph 采集 renderer 输出             │
│ Inspector + UndoRedo         │   └──────────────────────┬──────────────────────────┘
└───────────┬──────────────────┘                          │ 组件/ABI/CLI（文档模型优先）
┌───────────▼─────────────────────────────────────────────▼───────────────────┐
│ EngineDLL/ParticleFXAPI.cpp   资产式导出（不再走 config blob，见 §6）            │
├─────────────────────────────────────────────────────────────────────────────┤
│ Engine/ParticleFX/host/      ECS 组件(ParticleFXComponent) · 预算/LOD 管理器    │
├─────────────────────────────────────────────────────────────────────────────┤
│ Engine/ParticleFX/interop/   事件消费者(RigidBodySpawner/DecalSpawner…)         │
│                              IPhysicsQuery 适配(由 Simulation 实现)             │
│                              Alembic 缓存回放组件 · GeometryCache              │
├─────────────────────────────────────────────────────────────────────────────┤
│ Engine/ParticleFX/render/    渲染器插件: sprite·mesh·ribbon·decal·light·fluid   │
│                              （经 RenderGraph AddPass 接入，不 include 管线内部）│
├─────────────────────────────────────────────────────────────────────────────┤
│ Engine/ParticleFX/core/      attribute_store(属性表) · emitter_program(编译产物) │
│                              module_registry(阶段化模块堆栈) · sim_runner       │
│                              event_bus · field_system · rng/确定性              │
│   依赖闭包: Utilities/ + JobSystem。零引擎上层依赖 —— 松耦合的物理边界           │
├─────────────────────────────────────────────────────────────────────────────┤
│ Engine/ParticleFX/gpu/       通道描述符驱动的 compute 管线（标准模块子集）        │
│   依赖: RHI 接口（经注入的 dispatch 适配器，Metal + Vulkan 双后端）               │
└─────────────────────────────────────────────────────────────────────────────┘
        │ 事件（POD + 句柄）                    ▲ body_desc（纯引擎类型）
        ▼                                     │
  任意消费者（音频/脚本/物理/序列器）      Engine/Simulation/（Jolt 隔离层，已存在）
```

**松耦合四规则（评审时逐条对照 PR）：**

1. `core/` 的 include 闭包 = `Common/CommonHeaders.h` 级别的工具头 + `Engine/JobSystem/`。出现 `RHI/`、`Simulation/`、`Components/` 即违规。
2. `gpu/` 与 `render/` 只经**注入接口**触碰 RHI（`IRhiDispatch`、RenderGraph 的 AddPass 句柄），不 include 具体后端（Metal/Vulkan 头只允许在各自平台目录）。
3. `interop/` 是唯一允许 include `SimulationAPI.h` 的地方（纯引擎类型，已是物理文档纪律）；Jolt 头依然不出 `Engine/Simulation/`。
4. 所有对外（编辑器/CLI/测试）经 `EngineDLL/ParticleFXAPI.cpp` 与资产文件，不新增"直调核心单例"的第二事实源。

---

## 2. Niagara 范式映射表

| Niagara 概念 | PFX 对应物 | 说明 |
|---|---|---|
| System | `pfx_system` | 多发射器容器：共享时间/缩放、事件路由、确定性 RNG 域 |
| Emitter (CPU/GPU) | `emitter_program` + `sim_context{cpu\|gpu}` | 编译产物：扁平模块列表 + 属性布局 + 缓冲尺寸 |
| 阶段堆栈（Spawn/Particle Spawn/Particle Update/Render） | 同名四阶段 | 模块有序执行；阶段语义与 Niagara 一一对应 |
| Module + Module Inputs | `module_registry` 项 + 参数块（POD） | **原生 C++ 模块**，非可视化脚本；v1 注册 ~30 个内置模块 |
| 属性命名空间（`Particles.*` `Engine.*` `User.*`） | `attribute_store` 通道命名空间 | `Particles.Position` 等标准通道 + 用户自定义通道 + 系统常量 |
| Custom HLSL | v1: C++ 函子注册（引擎侧）；v2: 标量表达式语言；GPU: 自定义 kernel shelf | 不做全量脚本 VM，见 §3.3 的分级 |
| Data Interface | `data_interface` 注册表 | mesh 采样器、SDF 场、纹理、曲线/渐变（复用现有 curve 类）、音频谱(远期) |
| Event / Event Handler | `event_bus`（POD + u64 句柄 + 载荷引用属性行） | 发射器间 + 出引擎（消费者）+ 入引擎（触发 burst）双向 |
| Renderer (sprite/mesh/ribbon/decal/light) | `render/` 插件 + **fluid**（本引擎特色） | 渲染器只是属性表的视图 + 索引，模拟零感知 |
| Scalability / Significance | `budget_manager`（host 层） | 全局预算 + 发射器 LOD 因子 + 距离剔除 |
| Niagara Debugger / Spreadsheet | `pfx.debug` CLI dump + HUD 统计 + 属性直方图 | 完整电子表格 UI 属远期可选 |
| Determinism / Replay | 固定种子 RNG + transform 哈希门禁（沿物理文档） | 回放不做（无需求驱动） |

**刻意不对标的部分**：Niagara 的可视化模块图表编辑器（v1 用 JSON 资产 + 编辑器表单，节点 UI 列 N7 可选）；GPU 模拟的 transient/replay 特性；HLSL 全图翻译。

---

## 3. 核心设计

### 3.1 attribute_store —— 数据模型（对齐 Houdini "粒子即属性表"）

```cpp
// ParticleFX/core/attribute_store.h
struct attr_desc { u32 name_hash; attr_type type; u32 stride; };   // type: f32/v2/v3/v4/u32/u8n

class attribute_store {
public:
    // 编译期注册通道（emitter_program 持有布局），分配按容量一次到位
    u32  add_channel(const attr_desc&);              // 返回 channel_id
    f32* write_ptr(u32 channel_id);                  // SoA 连续存储
    u32  allocate_row();                             // 返回行号 = 粒子 id（含空闲链）
    void free_row(u32 row, event_bus&);              // 自动发 death 事件（升级版 P0）
    // 句柄 = (row, generation)，语义沿用 upgrade-plan ADR2，此处原生内建
};
```

- **标准通道**（每发射器必有）：`Position v4(age 借 w)`、`Velocity v4(lifetime 借 w)`、`Color v4`、`Scale v4`、`SpriteIndex`——**布局与旧 `particle_data` 逐字段对齐**，使旧 sprite 渲染器可零改动消费新 store（兼容桥的关键）。
- **用户通道**：发射器 JSON 里声明任意 `(name, type)`，运行时按容量分配。刚体参数（mass/restitution/mesh_id）、Houdini 缓存属性列、事件载荷统统是用户通道——**不再需要旁挂 payload hack**（旧计划的 P0 payload 方案被此取代）。
- GPU 镜像：通道布局描述符（反射 blob）上传为 constant buffer，kernel 按描述符寻址；标准通道保持旧 80B interleaved 布局以复用现有间接绘制与剔除路径。

### 3.2 emitter_program 与模块堆栈（对齐 Niagara 阶段模型）

```jsonc
// assets/pfx/fire_jet.pfx_emitter （JSON 资产，文本可 diff、可版本管理）
{
  "capacity": 10000,
  "attributes": [ { "name": "SpinRate", "type": "f32" }, { "name": "BodyMass", "type": "f32" } ],
  "exposed":   [ { "name": "Intensity", "type": "f32", "default": 1.0 } ],   // User.* 可被序列器/脚本驱动
  "stages": {
    "spawn":   [ { "module": "SpawnRate", "rate": 200 }, { "module": "SpawnBurst", "count": 50, "period": 2.0 } ],
    "init":    [ { "module": "ShapeLocation", "shape": "cone", "angle": 25, "height": 1.5 },
                 { "module": "AddVelocity", "mode": "along_normal", "min": 4, "max": 7 },
                 { "module": "SetUserAttribute", "attr": "BodyMass", "min": 0.5, "max": 2.0 } ],
    "update":  [ { "module": "GravityForce" }, { "module": "DragForce", "coefficient": 0.1 },
                 { "module": "CurlNoiseForce", "frequency": 0.8, "strength": 3 },
                 { "module": "KillVolume", "shape": "aabb", "response": "kill" } ],
    "render":  [ { "renderer": "sprite", "material": "fire_mat" } ],
    "events":  [ { "on": "death", "consumer": "RigidBodySpawner", "payload": ["Position","Velocity","BodyMass"] } ]
  }
}
```

- **编译**：加载时把 JSON 编译为 `emitter_program`（模块实例数组 + 通道布局 + 参数块打包），模块解析失败/类型不匹配在加载期报错，不进运行时。热重载友好（重编译 + 原子换指针）。
- **模块**：原生 C++ `update(context, params, store, row_range)`；`module_registry` 按 name_hash 注册。内置模块清单见 §7 里程碑表。渲染器与模拟模块同一注册机制，但分阶段槽位。
- **确定性**：每 system 一个 `pcg32` 域（种子可注入），模块禁用隐式全局随机——哈希门禁（沿物理文档思路）才有意义。

### 3.3 自定义逻辑分级（对齐 Niagara Custom HLSL / POP VOP-Wrangle，务实版）

| 级别 | 载体 | 范围 | 里程碑 |
|---|---|---|---|
| L1 组合 | 内置模块 + 参数 + 曲线/渐变 | 覆盖 ~90% 特效 | N1 起 |
| L2 原生函子 | 引擎侧 C++ 注册 `pfx::register_module("MyLogic", factory)` | 任意逻辑，仅引擎/游戏代码可用 | N1 |
| L3 表达式 | 标量/向量表达式求值器（参数间算术，无循环无分支跳转） | 美术级微调 | N4（可选） |
| L4 GPU kernel shelf | 手写 Metal/WGSL kernel 注册进 GPU 更新段 | 高级 GPU 特效 | N5 |

不做图灵完备脚本 VM——无需求驱动，且破坏确定性与沙箱。

### 3.4 field_system —— Houdini POP 能力的载体

```cpp
// ParticleFX/core/field_system.h
class volume_field {  // 标量/向量网格场：SoA grid + world->grid 变换 + 采样接口
    v3 sample(v3 world_pos) const;
    f32 sample_sdf(v3 world_pos) const;          // SDF 场附梯度采样
};
// 场来源（data_interface 注册）：
//   analytic_field   —— 解析式（噪声/漩涡/常数），CPU 直算
//   sdf_asset_field  —— 引擎 SDF 资产（ContentTools/SDF · OfflineSDFMerger 产物，读回设施已有）
//   grid_cache_field —— 离线转换的 VDB→自有 grid 格式（ContentTools 烘焙，见 §5）
//   sim_field        —— 预留：运行时流体/pyro 求解器输出（另一立项，接口先占位）
```

POP 能力映射（哪些进本计划、哪些明确不进）：

| Houdini POP | PFX 对应 | 归属 |
|---|---|---|
| POP Source / POP Object | spawn 阶段模块族 | N1 |
| POP Force / Wind / Turbulence | 力模块族（curl noise 等） | N1/N4 |
| POP VOP / Wrangle(VEX) | 模块组合 + L2/L3 自定义逻辑 | N1/N4 |
| POP Collision（SDF/几何） | `SDFCollision` / `KillVolume` 模块（场驱动，响应 kill/bounce/slide） | N4 场系统 + POP 碰撞/平流 | ✅ 完成 | 36 项内含；alembic/vdb 转换器实体化（vdb 依赖矩阵已解除） |
| POP Advect（按速度场平流） | `AdvectByField` 模块（pyro 缓存驱动粒子 = 经典联动） | N4 |
| POP Grains（PBD 颗粒）/ POP Fluid（SPH） | **不做**——约束求解器归物理调研 §2.4 流体立项；架构预留 update 阶段"迭代约束模块"槽位（`constraint_module` 基类），不实现 | 另立项 |
| Geometry Spreadsheet | `pfx.debug dump` CLI + 属性直方图 | N2 起渐进 |
| SOP 网络 + 烘焙 | 离线管线（§5） | N4 |

### 3.5 event_bus（沿旧计划 ADR2/ADR4，升级为框架原生）

- POD 事件 `{ u64 handle, emitter_id, event_type, 载荷通道引用列表 }`；环形缓冲、单帧消费、无分配。
- **出向**：消费者（`interop/`）注册回调按事件类型拉取——RigidBodySpawner（旧 P3 全盘搬入，body 变换权威规则不变）、DecalSpawner、（远期）音频/脚本。
- **入向**：引擎事件 → 发射器触发（burst/reset）：序列器轨道触发、脚本 tick、物理碰撞回调（经 `IPhysicsQuery` 适配的 contact 事件）。这补上 Niagara "Event Handler 派生发射器"的入半边；发射器间派生（death→子发射器）N3 内做。

### 3.6 GPU 路径策略（2026-09-12 复审决议：GPU 图发射器延后）

- **决议**：N5（GPU 图发射器）无期限延后，启动条件 = 出现十万级同屏粒子、GPU 深度碰撞或旧 GPU 路径无法承载的实需求。
- 旧 `GPUParticleSystem`（10 万粒子、间接绘制）**长期作为唯一 GPU 路径**：不再扩功能，但也不是待替换的 compat 层——正常维护。
- 架构存档（启动 N5 时按此执行）：attribute_store 的 GPU 镜像（通道布局描述符上传为 constant buffer，kernel 按描述符寻址；标准通道保持旧 80B interleaved 布局）+ **标准模块子集**（~15 个高频 spawn/update 模块）的手写 kernel 映射——每个模块配 Metal `.metal` 与 Vulkan `.wgsl` 片段，编译期按模块列表拼接派发。不做 HLSL→xxx 翻译器。
- GPU 深度缓冲碰撞随 N5 延后；N4 的边界碰撞只覆盖 CPU 路径（解析体 + SDF），文档明确不支持 GPU 路径边界。

---

## 4. 与引擎各系统的结合面（松耦合清单）

| 系统 | 结合方式 | 耦合强度 | 纪律 |
|---|---|---|---|
| ECS | `ParticleFXComponent`（照 Light 组件模式：SoA + 自由函数），与旧 `Particle` 组件并存 | host 层单向依赖 ECS | 文档模型优先 |
| 物理 Jolt | `interop/RigidBodySpawner` 消费 death 事件建 body；`IPhysicsQuery`（射线/碰撞查询接口）由 Simulation 实现注入，粒子碰撞模块只认接口 | 接口注入，零 include | SimulationAPI.h 纯引擎类型 |
| 渲染管线 | renderer 插件经 RenderGraph `AddPass` 注册；旧 `ParticlePass` 保留服务 compat | render 层单向 | 不 include StandardRenderPipeline 内部 |
| Lumen GI | emissive 粒子 → T4.4 已有的 per-instance emissive 表写入（light renderer 的一个输出通道） | 单向写既有表 | 强度钳制沿用 T4.4 门禁 |
| FluidRenderPass | 正式化为 `fluid` renderer（`FluidInputs` 已就绪，补接线） | render 层内 | — |
| 序列器 | `pfx_system` 为可 tick 轨道；`exposed` 参数可关键帧 | host 层 | 沿序列器既有轨道模式 |
| 编辑器 | 发射器= JSON 资产。**三权分立（2026-09-12 决议）**：文档在 UI（UndoRedo/保存走文档模型）；schema 反射自引擎（表单按 `PfxGetEmitterSchema` 动态生成，C# 零模块镜像零漂移）；运行时状态走组件属性导出。曲线编辑器复用 | ABI + 资产 | 引擎不做结构性编辑导出（§6 决议） |
| CLI/MCP | `pfx.dump`（调试）、`pfx.preview`（离线单帧渲染 PNG，走 primal-engine render 通道）、`pfx.convert`（旧 preset→JSON 资产） | CLI 框架惯例 | schema 快照随 PR |
| 资产管线 | `.pfx_emitter` JSON + 引用的材质/mesh/SDF 资产；`primal-build` 打包覆盖 | ContentTools | 离线优先惯例 |
| 旧粒子系统 | 单向转换工具 `pfx.convert`；旧系统冻结（只修编译/崩溃） | — | N6 后进入删除评估 |

---

## 5. Houdini 联动（离线管线部分）

```
Houdini                     ContentTools（离线）                运行时
──────                      ────────────────────              ──────
POP 模拟 → bgeo/alembic  →  alembic_converter（新）          GeometryCacheComponent
  （点缓存/顶点动画）          → 自有 .pcache 格式               （renderer: mesh/sprite 按属性驱动）
VDB 体积场（pyro 速度/SDF）→  vdb_converter（新）               grid_cache_field
  OpenVDB 采样为自有 grid      → .pgrid（SoA 紧凑 grid）         （AdvectByField / SDFCollision 消费）
HDA 烘焙（程序化发射形状）  →  Houdini 批处理导出 mesh/点集       ShapeLocation 的 mesh 采样源
```

- **转换器实现选型**：Alembic 用官方 `alembic` 库（C++，Imath 依赖）在 ContentTools 内编译；OpenVDB 依赖较重（TBB/blosc），首版**只读采样**所需子集（`openvdb::io::File` + 网格遍历），裁剪依赖面。两转换器均为离线可执行体，不进运行时链接。
- **为什么走缓存而非 HAPI 运行时**：本仓库资产纪律是离线烘焙（Nanite/SDF/card bake 先例）；HAPI 嵌入运行时引入重依赖且破坏确定性。
- 导出（粒子模拟 → Alembic 供 DCC 复用）列为远期可选，无当前需求。

---

## 6. ABI 与资产策略（对旧 config-blob 模式的取代）

- **新框架不走 config blob**。旧模式（`EmitterSetConfig(memcpy blob)`）的三大痛点——布局偏移脆弱（C# 硬补 offset 8）、指针需 Sanitize、字段演进怕破坏兼容——在资产模式下消失：发射器是 JSON 资产，ABI 只传**资产 id + 已暴露参数**。
- 新增导出（`EngineDLL/ParticleFXAPI.cpp`，只增不改纪律）：
  - `PfxSystemEnsure / PfxEmitterCreateFromAsset(path) / PfxEmitterRemove`
  - `PfxGetEmitterSchema()`——**编辑归属决议的核心导出**：引擎→编辑器的模式反射（模块清单、每模块参数的类型/范围/默认值、渲染器列表，JSON 格式）。UI 表单与 CLI/MCP 校验的单一事实源；C# 永远不手写模块知识。
  - `PfxEmitterReloadFromMemory(buf)`——编辑期热预览：编辑器把整份 JSON 文档推送重编译（防抖）。撤销/重做纯 UI 侧完成，不跨 ABI。
  - `PfxSetExposedParam(emitter, name_hash, value pod)`（序列器/脚本/编辑器共用）
  - `PfxGetStats(emitter, out)`（活跃数/预算占用——debug HUD 与 CLI 共用）
  - 事件读取暂不导出（消费者同进程；需要时追加只读导出）
- **不做结构性编辑导出（决议）**：`AddModule/RemoveModule/MoveModule/SetModuleParam` 这类命令永远不进 ABI。理由：①模块图是结构性数据，编辑 ABI 必然膨胀成命令语言（万能 ABI 第二事实源陷阱）；②UndoRedo 无法在 UI 文档模型内闭环；③引擎内存与文件会形成双事实源，破坏版本控制与 primal-build 打包；④这正是旧系统 `EmitterSetConfig(blob)` + C# 硬补字节偏移事故的模式。
- 旧 `Particle*API` 全部保留原样（旧系统冻结但不破坏），`static_assert(offsetof(emitter_config, spawn_rate)==8)` 守护补丁照旧生效。

---

## 7. 里程碑 N0–N7

> 编号 N 系列取代旧 P 系列。工作量为人日粗估。每阶段门禁 = `./scripts/ci_local.sh` + `ctest -R Unit` + 带可视化产物的集成用例（沿物理文档铁律）+ 双后端（Metal / DebugVulkan，切树先 `rm libEngine.a`）。

### N0 框架骨架（~8-10 日）★ 地基 —— **已完成（2026-09-13，交付与偏差见 §10.1）**
- `core/`：attribute_store（含 row/generation 句柄、death 事件自动发射——吸收旧 P0）、event_bus、`pfx_system` tick 骨架、确定性 RNG 域。
- **宿主接线（复审新增，必做）**：`fx::tick` 挂入已存在的 `PipelineTick`（`EngineDLL/PhysicsAPI.cpp:177`，物理 step 之后）。事实依据：现状 `particles::update` 只有集成测试在调、生产宿主从未驱动粒子——PFX 从 N0 起就接生产 tick，不重蹈覆辙。
- **兼容桥**：标准通道布局对齐旧 `particle_data`，旧 `ParticlePass`/`ParticleSorter` 可消费新 store（排序器按行索引工作，SoA 布局天然兼容）。一条测试：同一发射器数据双路径渲染数值容差一致（1e-5）。
- 门禁：句柄 ABA 用例；事件顺序断言；空渲染集成测试冒烟；编辑器视口内粒子真实更新（生产 tick 生效的可见证明）。

### N1 模块堆栈 + spawn/update 模块族（~9-11 日）
- `module_registry` + JSON 编译器（加载期校验）；内置模块第一梯队：SpawnRate/Burst、ShapeLocation（point/sphere/box/cone/cylinder + 方向分布 + 继承源速度）、AddVelocity、GravityForce/DragForce、SetColor/Scale（挂现有曲线类）、KillAge。
- **JSON 依赖决议（复审新增）**：资产格式（嵌套模块数组 + 异构参数 + 行号报错）超出 `ParticlePreset.cpp` 内置手写解析器（`preset_json`）的能力边界。vendor **nlohmann/single-header**（MIT、header-only、零构建影响）进 `third_party/`，`LICENSE-THIRD-PARTY.md` 登记。备选"扩展 preset_json"否决：零依赖但报错能力与维护性差，资产格式是要长期演进的接口，不值得省这个依赖。
- L2 C++ 函子注册通道；`pfx.convert` 把 8 个内置旧预设转为示例 JSON 资产（兼作文档样例）。
- **编辑器基础表单（v1 形态 + 编辑归属两项决议落地）**：`ParticleFXComponent`（照 Light 组件模式）挂/卸发射器资产、暴露参数编辑、UndoRedo 文档模型路径；表单按 `PfxGetEmitterSchema` 反射动态生成（C# 无模块镜像）；编辑期预览走 `PfxEmitterReloadFromMemory` 整文档推送（防抖重编译），撤销/重做不跨 ABI。
- 门禁：形状采样分布直方图断言（10 万粒子）；JSON 加载失败路径用例（缺字段/类型错/未知模块）；旧预设转换 diff 测试；编辑器创建→tick→保存→重载闭环冒烟。

### N2 渲染器解耦 + Sprite/Mesh（~9-11 日）
- renderer 插件接口（渲染器=属性表视图+索引）；sprite 渲染器走 RenderGraph 正式接入；**mesh 渲染器**（实例变换缓冲 + indirect indexed instanced，双后端）；mesh 引用具体化（复审确认）：资产句柄用 `primal::mesh::mesh_id`，渲染侧消费 `Engine/Graphics/RenderMesh.h`；`pfx.debug dump` CLI（属性直方图 + 统计）。
- mesh 粒子透明材质明确不支持（文档写死）；排序仅 opaque/alpha-test。
- 门禁：Sponza 内 1000 实例 mesh 粒子双后端帧序列 + 亮度 CSV；`TestParticleSponza` 不回归。

### N3 事件总线 + 刚体消费者（~10-12 日）
- 入向/出向/发射器间事件；`interop/RigidBodySpawner`（旧 P3 设计全盘适用：变换权威单写、预算否决、挤旧策略、freeze/orphan 处置）；`IPhysicsQuery` 接口 + Simulation 适配器；发射器间派生（death → 子发射器 burst）。
- **tick 时序（复审确认）**：`PipelineTick` 已存在，无过渡 ABI 需要。顺序定为：物理 `step`（post-step transform 可读）→ spawner 消费事件建/毁 body → `pfx_system::update`。原计划"宿主 tick ABI 未合入"的风险撤销。
- 门禁：固定种子 300 step transform 哈希门禁（单线程确定性模式）；刚体瀑布可视化 mp4；跨发射器派生用例。

### N4 场系统 + POP 碰撞/平流（~10-12 日）
- `field_system`：analytic + SDF 资产场 + grid 缓存场；模块：CurlNoiseForce、SDFCollision（bounce/slide/kill + collision 事件节流）、KillVolume、AdvectByField。
- **ContentTools 转换器**：`vdb_converter`（OpenVDB 只读子集）；`alembic_converter` + `GeometryCacheComponent` 回放。
- 删除旧死代码 `set_collision_handler`（ADR7 沿用）。
- 门禁：SDF 边界零穿透 PNG；VDB 场平流可视化（pyro 缓存驱动粒子烟）；能量衰减 CSV（restitution=1 守恒断言）；Alembic 回放帧 diff。

### N5 GPU 图发射器（**已延后**——2026-09-12 复审决议②）
- **无期限延后**。启动条件（满足其一）：十万级同屏粒子实需、GPU 深度碰撞实需、或旧 GPU 路径无法承载的需求。
- 启动时按 §3.6 存档方案执行：通道描述符驱动的 compute 管线、标准模块子集（~15 个）Metal/WGSL 双语言 kernel 映射、GPU 深度缓冲碰撞。
- 延后期间：旧 `GPUParticleSystem` 为唯一 GPU 路径（正常维护态）；全局预算管理器（N6）统一覆盖 CPU/GPU 两路径。

### N6 表现力收尾 + 预算/LOD + GI/Fluid 联动（~8-10 日）
- ribbon 渲染器（历史环形缓冲 + death 截断）；decal/light 渲染器；fluid renderer 正式接线（水柱集成场景）；soft particles 真实现（旧假开关在此了断）；emissive→GI per-instance 表联动 + T4.4 强度门禁复用；LOD/重要性剔除/全局预算 HUD（budget_manager 统一覆盖 CPU/GPU 两路径——N5 延后后由本阶段兜底 GPU 侧）。
- 门禁：各渲染器可视化产物；预算砍半帧时长线性断言；GI 响应量化门禁（沿用 M2/M3 门禁思路）。

### N7 可选项池（按需排期，不默认进）
- 编辑器节点图 UI；L3 表达式求值器；发射器间 GPU 事件；Alembic 导出；属性电子表格 UI；GPU kernel shelf 文档化。

**关键路径（复审后更新）**：N0→N1→N2→N3 ≈ 36-44 人日达成"Niagara 骨架 + 编辑器闭环 + 刚体化"；N4 补 Houdini POP 碰撞/场能力与离线管线；N5（规模化 GPU）延后不在路径上；N6 表现力收尾。旧系统在 N6 后评估删除（观察期一个迭代）。

---

## 8. 风险登记

| 风险 | 缓解 |
|---|---|
| 双系统并行期的维护税 | 旧系统立即冻结（只修崩溃/编译）；`pfx.convert` 在 N1 就绪，编辑器资产尽快切新 |
| 兼容桥逐像素一致难达 | 桥只保证标准通道布局一致 + 数值容差断言（1e-5），不承诺逐像素；渲染差异走黄金图 |
| GPU kernel 子集双语言维护 | **N5 延后，此风险休眠**。启动时执行：kernel 生成器单文件单模块，禁跨模块共享私有工具函数（宁可复制）；CI 编译双后端 shader 门禁已有（check_shader_copies） |
| OpenVDB/Alembic 依赖膨胀 | 只读子集 + 仅 ContentTools 离线链接，运行时零依赖；LICENSE-THIRD-PARTY.md 登记 |
| 刚体预算失控（ADR6） | 消费者强制 max_live_bodies + debug HUD 常驻显示占用；哈希门禁防时序回归 |
| 范围蔓延（节点 UI/流体求解器/脚本 VM） | 全部列 N7/另立项；本文非目标即合同 |

## 9. 非目标（合同条款）

Niagara 可视化节点编辑器（N7 可选）、全量 GPU 模块翻译、SPH/PBD 流体求解器（物理调研 §2.4 另立项）、HAPI 运行时嵌入、图灵完备脚本 VM、回放系统、bgeo 直读。

---

## 10. 实施状态与偏差记录

> 本节是文档的"账本"：每个里程碑完成后在此登记交付物、门禁证据与对设计的偏差修订。设计章节（§3–§7）保持目标态不改写，偏差只在本节累积——读者先看设计、再看账本，即得全貌。

### 10.1 N0 框架骨架 —— 已完成（2026-09-13）

**交付物**：

| 文件 | 内容 |
|---|---|
| `Engine/ParticleFX/PfxTypes.h` | 粒子句柄 (row, generation)（ADR2）、POD 事件（static_assert 锁 20B）、标准通道常量、pcg32 |
| `Engine/ParticleFX/core/AttributeStore.h/.cpp` | SoA 属性表：5 标准通道（16B stride，可按 v4 数组寻址）+ 用户通道（对齐 16B）；free 自动发 death 事件（带寿命进度） |
| `Engine/ParticleFX/core/EventBus.h/.cpp` | POD 环形缓冲、FIFO 单帧消费、满时丢新 + 限频告警（首丢/每 1024 次） |
| `Engine/ParticleFX/core/PfxSystem.h/.cpp` | 发射器生命周期、直接 spawn、tick（age 推进/到期回收）、emitter id 单调不复用、幂等惰性初始化 |
| `EngineTest/UnitTests/ParticleFX/ParticleFXTests.h` + `TestParticleFX.cpp` | N0 门禁测试 7 项（风格沿 Particles 测试惯例） |

**接线**（3 处）：`Engine/CMakeLists.txt` 源码 glob 增 ParticleFX 目录；`EngineTest/UnitTests/CMakeLists.txt` 注册 `TestParticleFX` 目标；`EngineDLL/PhysicsAPI.cpp` `PipelineTick` 追加 `pfx::tick(dt)`（物理 step 之后）——修复"粒子系统生产宿主从未被 tick"的缺口（复审事实②）。

**门禁证据**（2026-09-13，Darwin/Debug 树）：

- `TestParticleFX` 7 项全过：store 通道/resolve、句柄 ABA、事件 FIFO/溢出、发射器 birth/death 事件序（按寿命到期序）、旧 `particle_data` 布局兼容桥（80B 逐字节 memcmp 等价）、pcg32 确定性、系统生命周期/drain。
- 无回归：TestParticles、TestParticleMaterialAPI、TestPhysicsSimulation、TestPhysicsM4、TestPhysicsSmoke、TestPhysicsMultiBody 全过。
- 集成冒烟：TestModularPipeline 与 TestParticleSponza 各 90s kill，Sponza 约 5000 帧 `pipeline=VALID`、粒子数正常增减，日志零 fault。
- 依赖闭包核查：`core/` 仅 include `Common/` + `Utilities/`（松耦合规则 1 合规）。

**过程记录（门禁的价值）**：初版属性表通道偏移按 `stride` 递增（AoS 思维），全部通道内存重叠——门禁测试以"b 粒子的 position 出现在 velocity 通道"定位，已改为 channel-major SoA（每段 `stride × capacity`），并在测试中固化为跨通道独立性永久断言。

**对设计的偏差修订**（均不违反 ADR，已回写认知）：

1. **事件总线**：§3.5 原文"per-emitter 环形缓冲"简化为**系统级单一环形缓冲**（事件自带 source emitter id，消费端一次 drain 即全部；ADR4 语义不变）。
2. **通道注册**：§3.1 草图的动态 `add_channel` 在 N0 落地为 `initialize` 时一次性注册，上限 16 通道（5 标准 + 11 用户）；动态注册随 N1 JSON 编译器落地。
3. **初始化时序**：`tick` 幂等惰性 ensure（沿 `ParticleEnsureSystems` 先例），生产宿主零接线负担。
4. **行分配序**：free-list 为 LIFO 栈、倒序填栈使首次分配落在 row 0（行密度优先，缓存与渲染桥友好）。
5. **N0 门禁"编辑器视口内粒子真实更新"**：以无头 tick + 生产接线 + 集成测试证明交付；视口可见证明随 N2 sprite 兼容桥落地（渲染半边未就绪前视口无物可显）。

**环境预置问题备忘**（非本工作引入）：主树 `TestVulkanSponzaRenderGraph` 因 `ENABLE_VULKAN=OFF` 在 Darwin/Debug 树不可链接（属 DebugVulkan 独立树，AGENTS.md 既有约定）；`Tests/UnitTests/` 曾缺 `libfbxsdk.dylib` 拷贝（历史构建中断残留，已手工放置）。

**下一里程碑**：N1（模块堆栈 + JSON 编译器 + 编辑器基础表单，~9-11 日）。

### 10.2 N1 模块堆栈 + JSON 编译器 —— 已完成（2026-09-13）

**交付物**：

| 文件 | 内容 |
|---|---|
| `Engine/ParticleFX/core/PfxJson.h/.cpp` | 精简 JSON 解析器（递归下降、行/列号报错、值携带来源行号、行注释容忍） |
| `Engine/ParticleFX/core/ModuleRegistry.h/.cpp` | 模块注册表（名字哈希、阶段、参数 POD、解析/执行器、schema 提示） |
| `Engine/ParticleFX/core/EmitterProgram.h/.cpp` | 编译器（加载期校验、`path: message (line L)` 报错、param_ref 暴露参数绑定、schema 反射 JSON、资产文件加载） |
| `Engine/ParticleFX/modules/BuiltinModules.cpp` | 内置模块 10 个：SpawnRate/SpawnBurst（spawn）、ShapeLocation/AddVelocity/SetLifetime/SetColor/SetScale（init）、GravityForce/DragForce/KillAge（update） |
| `Engine/ParticleFX/interop/LegacyPresetConvert.h/.cpp` | pfx.convert：旧 8 预设 → `.pfx_emitter`（有损近似 + `legacy_render`/`legacy_curves` 无损保留），带自编译校验 |
| `EngineDLL/ParticleFXAPI.cpp` | 8 个导出：PfxSystemEnsure / CreateFromAsset / Remove / ReloadFromMemory / SetExposedParam / GetEmitterStats / GetEmitterSchema / ConvertLegacyPresets |
| `PrimalEditor_Avalonia`：ParticleFXComponent.cs、PfxHash.cs、注册表项、NativeEngineBridge P/Invoke、同步器分支、Inspector 面板 | 编辑器基础表单（AssetPath + Enabled）+ 同步接线 + 跨语言哈希 |

**门禁证据**（2026-09-13）：

- `TestParticleFX` 扩至 20 项全绿（N0 7 + N1 13）：解析器行号报错、9 条编译失败路径、SpawnRate 节奏（100/s → 100/50/0）、SpawnBurst 首发+周期、球面分布（10 万样本半径最大误差 9.5e-7）、重力/阻尼半隐式欧拉积分、KillAge 提前杀时序、同种子确定性、暴露参数绑定与运行期改值、热重载同 id 重启、schema 反射内容、资产文件加载往返、8 预设转换 + 往返编译。
- C# `PfxHashTests` 6/6：fixture 取自引擎真实 Murmur3 输出（跨语言一致性门禁）。
- C# Native 过滤器 72/72；Cli 过滤器 55/56（唯一失败 `CliSchemaSnapshotTests` 为 dev 分支预置问题——他人未提交的 schema 快照改动，本次未新增任何 CLI 命令）。
- EngineDLL 重链后 `nm -gU` 确认 8 个 `Pfx*` 导出可见。

**对设计的偏差修订**：

1. **JSON 库决议修订**：vendor nlohmann 前发现引擎全局 `-fno-exceptions`，nlohmann 异常错误模型不可用（`JSON_NOEXCEPTION` 模式丢行号）。改自研 PfxJson——保住"行号报错"这一决议的真实理由，零第三方依赖。
2. **SpawnRate/Burst 语义固化**：spawn 累积器跨帧（速率余量并入下帧）；Burst 首发立即、其后按周期。
3. **生成帧即积分**：程序模式下新生粒子在同一 tick 的 update 阶段受力并推进 age（半隐式欧拉逐帧语义）——默认寿命 1s、dt=1.0 时当帧到期属正确行为，测试已按此校准。
4. **KillAge 评估时序**：update 模块在 age 推进之前执行（看到的是上帧末 age）。
5. **编辑器 v1 表单**：AssetPath + Enabled 两项（沿 Particle 面板模式）；暴露参数经组件 JSON 字段/CLI 设置，动态参数网格随 schema 驱动表单（N2+）落地。

**下一里程碑**：N2（渲染器解耦 + Sprite/Mesh，~9-11 日）。

### 10.3 N2 渲染器解耦 + Sprite/Mesh —— 渲染半边完成（2026-09-13，二次施工）

**交付物一：渲染桥数据层**（`Engine/ParticleFX/render/Bridges.h/.cpp`，首轮）：

- `pack_sprite_instances`：属性表 → 旧 `particle_data` 交错布局（仅活行、行序压缩、null 探测用法）——sprite 渲染经此桥直通既有 `ParticlePass`，渲染管线零改动。
- `pack_mesh_instances`：mesh 渲染器 per-instance 变换缓冲（48B `mesh_instance_data`，static_assert 锁布局；四元数 v1 为单位姿态）。

**门禁**：TestParticleFX N2 组 2 项（打包行数/字段一致性/死行跳过/48B 布局）。

**交付物二：mesh 粒子渲染管线（二次施工落地）**：

| 文件 | 内容 |
|---|---|
| `EngineTest/shaders/ParticleFXMesh.metal` | Metal 顶点/片段 shader：顶点拉取（vertex pulling，索引绘制下 vertex_id=索引值）+ 四元数旋转变换 + 双灯 lambert；实例布局与 48B 桥逐字段对齐 |
| `Engine/Graphics/Passes/ParticleFXMeshPass.h/.cpp` | 渲染 pass：运行时加载 .metal 文本建管线（沿 ParticlePass 惯例）、描述符布局 4 绑定（positions/normals/instances/uniforms）、per-frame mapped 实例缓冲（`stage_instances` 按 frame_index 上传）、`DrawIndexed(indexCount, 0, 0, instanceCount)` 索引化实例绘制、内置程序化立方体网格源（24 顶点带面法线 + 36 索引） |
| `EngineTest/IntegrationTests/TestParticleSponza.cpp` | 集成接线：PFX 发射器（SpawnBurst 1000 + 球面采样 + 随机速度）+ 渲染图追加 `ParticleFXMeshPass`（Load lighting/depth，紧随旧粒子 pass） |

**渲染门禁证据**（2026-09-13，Darwin/Debug + Metal）：TestParticleSponza 60s 窗口运行——日志 `[ParticleFXMeshPass] first draw: 1000 instances x 36 indices`（渲染路径确被执行），3251 帧 `pipeline=VALID`，零 fault；TestParticleFX 全套 34 项无回归。

**pfx.debug dump CLI（同批落地）**：

- 引擎侧：`PfxDebugDump(asset, frames, dt, out, cap, out_len)`（EngineDLL/ParticleFXAPI.cpp）——离线驱动发射器 N 帧，输出 JSON：活粒数、位置包围盒、速率统计、归一化寿命 10 桶直方图、位置 FNV 指纹（跨运行确定性门禁）。
- CLI：`primal-engine pfx-debug --asset a.pfx_emitter [--frames N] [--dt SEC]`（stdout JSON 契约与既有 verb 一致）。
- 端到端证据：demo 资产 120 帧驱动输出 `alive=746`、位置包围盒、10 桶直方图；两次运行 `position_fingerprint` 一致（离线确定性）。

**实现注记（与原计划的偏差）**：

1. **绘制路径**：CPU 模拟下实例数在打包时已知，采用 RHI `DrawIndexed(instanceCount>1)` 索引化实例绘制；GPU 侧 indirect 计数（`DrawIndexedIndirect`）属 GPU 模拟域，随 N5（已延后）落地。
2. **Vulkan 对称路径 —— 已实现（2026-09-13 二次施工）**：`Engine/Graphics/Vulkan/Shaders/PFX/` 四个 pass 的 GLSL 顶点/片段对（glslangValidator -V 编译，8 个 .spv）；四个 pass 的 Vulkan 分支改为**加载预编译 SPIR-V**（loadSpvBinary 相对路径 + dev 绝对回退，入口 "main"，沿 ParticlePass 惯例）。**剩余验证**：~~DebugVulkan 树构建 + TestParticleSponza Vulkan 运行~~ **已完成（2026-09-13）**：按 AGENTS.md 双树纪律配置 DebugVulkan（ENABLE_VULKAN=ON）并构建，TestParticleSponza 运行 60s——Vulkan 后端确认（32 处日志）、五大 PFX pass SPIR-V 路径首绘全成功（mesh 1000 实例/light 32 灯/decal 32 贴花/ribbon 拖尾/fluid 2000 粒子接线）、3107 帧 `pipeline=VALID` 零 fault。注意：RUNTIME_OUTPUT_DIRECTORY 两树同为 `Darwin/Debug/`——DebugVulkan 构建会覆盖 Metal 二进制（AGENTS.md 双树纪律的根源），切回 Metal 前需完整重建 Debug 树。
3. **网格源**：v1 内置程序化立方体；外部 `mesh_id`（RenderMesh）绑定随资产管线集成接入（头文件已注明）。

### 10.4 N3 事件总线 + 刚体消费者 —— 已完成（2026-09-13）

**交付物**：

| 文件 | 内容 |
|---|---|
| `Engine/ParticleFX/interop/RigidBodySpawner.h/.cpp` | 刚体化消费者：birth 事件建 Jolt body（位置/速度取自标准通道）、death 事件按 destroy/freeze/orphan 处置、预算否决（reject_newest/retire_oldest，retire 强制收缩粒子寿命形成事件闭环）、物理变换权威单写回粒子 position 通道（ADR3） |
| `Engine/ParticleFX/interop/PhysicsQuery.h/.cpp` | IPhysicsQuery 注入接口 + `install_simulation_adapter()`（interop 唯一 include SimulationAPI.h，遵守松耦合规则 3） |
| `Engine/Simulation/SimulationAPI.h` + `PhysicsWorld.cpp` | 新增 `simulation::raycast`（Jolt 5.6 NarrowPhaseQuery 最近命中 + Body 表面法线；只增不改） |

**门禁**：N3 组 4 项——raycast 地面命中（点/法线/距离 + 背向与零向量防御）、生灭闭环（30 步物理后粒子位置由 body 回写下落）、预算 reject/retire 语义、**确定性哈希门禁**（同种子 120 步 `determinism_hash` 位级一致 = 12274184818074211435）。

**实现注记**：v1 事件消费为单消费者假设（spawner drain 全流、按 source 过滤，其余事件丢弃）——多消费者路由随 N4+ 事件总线升级替换；宿主接线（PipelineTick 内物理 step → spawner → 粒子 tick）已在 N0 建立顺序。

### 10.5 N4 场系统 + POP 碰撞/平流 + 死代码清理 —— 已完成（2026-09-13）

**交付物**：

| 文件 | 内容 |
|---|---|
| `Engine/ParticleFX/core/FieldSystem.h/.cpp` | `volume_grid` 均匀网格标量/向量场（三线性采样、中心差分梯度、SDF 外推语义）+ `.pgrid` 自有格式读写 |
| `Engine/ParticleFX/modules/FieldModules.cpp` | 4 个场/边界模块：KillVolume（AABB/球/平面 × kill/bounce/slide/clamp）、CurlNoiseForce（确定性 value-noise 势场旋度）、AdvectByField（向量场平流 force/set 两模式）、SDFCollision（SDF 场 kill/bounce/slide + 推离） |
| `Engine/ParticleFX/interop/GeometryCache.h/.cpp` | `.pcache` 点缓存格式 + 回放采样（最近邻帧、loop、容量探测）——GeometryCache 数据层 |
| `Engine/ParticleFX/interop/CacheConverters.h/.cpp` | vdb/alembic 转换器骨架（能力探测 + 明确失败语义；库接入按 §8 风险条目推进，运行时零第三方链接） |
| `Engine/Particles/ParticleCPU.h/.cpp` | **删除死代码** `set_collision_handler`/`set_collision_callback`/`has_collision_support`（ADR7，本计划 §10.4 原文执行） |

**门禁**：N4 组 5 项——场采样/梯度/`.pgrid` 往返（界外外推、中点三线性）、KillVolume bounce（平面+重力 600 帧永续弹跳、最低点 ≥ -0.05、速度方向翻转）与 kill（出生在盒内首帧回收）、CurlNoise 确定性（同种子逐值一致/异种子轨迹分离）、Advect(set)+SDF 组合、未知场名编译失败。累计 **36 项**。

**转换器实体化进展（2026-09-13，三次施工补充）**：

- **alembic_converter ✅ 实体化完成**：`ContentTools/pipeline/PfxCacheConvert.h/.cpp` + 独立离线工具 `pfx_converter`（ContentTools/CMakeLists.txt 新 target）。链接 vendored Alembic 1.8.7 + Imath 3.1.11 dylib（`third_party/Alembic/Mac/{include,lib}`，BUILD_RPATH 指向 vendored lib——沿 FBX_SDK 部署先例）。端到端门禁通过：`--make-test-abc`（Alembic 写 API 生成 2 帧×3 点归档）→ `alembic` 转换 → `.pcache`（92B = 12 头 + 8 帧表 + 72 数据）→ `--verify` 头校验。Alembic 1.8.7 API 差异已适配（`IPoints::matches(header)` 而非 getType/getStaticType）。
- **vdb_converter ✅ 实体化完成（2026-09-13 阻塞解除）**：oneTBB 2021.13.0 源码构建（`/tmp/pfx-deps-tbb`，-DTBB_TEST=OFF）后 OpenVDB 11.0.0 编译通过。上游源需两处补丁（新 Xcode clang 将 template 关键字缺省告警升级为错误：`NodeManager.h` 移除 `::template eval(` 的 template 关键字 ×2；`thread/Threading.h` 删除 `tbb::task::self()` 遗留分支——oneTBB 2021 已移除该 API）。构建产物静态/动态双份，vendor 至 `third_party/OpenVDB/Mac/{include,lib}` + `third_party/TBB/Mac/{include,lib}`（LICENSE 已登记）。
- **端到端门禁通过**：`--make-test-vdb`（OpenVDB 写 API：FloatGrid 地面 SDF d=y 8×16×8 + Vec3SGrid 风 (0,1,0) 4³）→ `vdb` 转换 → `.pgrid` 4140B（PGRD magic/ver=1/kind=0/dims 8×16×8 布局校验全过）。`PfxCacheConvertVdb.cpp` 已入 pfx_converter 目标，`vdb` verb 接通。

**对设计的偏差修订**：

1. **IntegrateVelocity 显式模块**：位置积分不在执行器隐式进行，而是显式模块（Niagara 惯例）——用户在 update 堆栈中控制"力 → 积分 → 碰撞"顺序。
2. **模块参数默认值系统**：params blob 为零填充，POD 默认成员值需经 `module_def::init_defaults`（placement new）恢复——修复"JSON 未显式给出的字段取 0 而非文档默认值"的系统性缺陷。
3. **转换器骨架**：OpenVDB/Alembic 库未 vendor（§8 依赖风险），运行时零依赖优先；格式契约已锁定（.pgrid/.pcache），库接入为独立子项。

### 10.6 N6 预算/LOD + 渲染件 —— 核心完成（2026-09-13）

**交付物**：`Engine/ParticleFX/host/BudgetManager.h/.cpp`——全局预算（max_emitters/max_live_particles 否决）+ 距离 LOD 因子（near/far 线性衰减 clamp）；`set_global_lod` 已接入执行链（SpawnRate/SpawnBurst 的 spawn 数量 × LOD 因子）。

**门禁**：N6 组 3 项——LOD 因子三段（1.0 / 0.55 线性中点 / 0.1 下限）、spawn/emitter 否决、GeometryCache 往返、转换器失败语义。累计 **34 项**（含 N2/N3/N4 组）。

**诚实边界**：decal/light/fluid 渲染器、soft particles 真实现、emissive→GI 表联动——依赖渲染管线 shader 与窗口环境可视化门禁；数据侧前置（死亡截断事件、预算否决、场采样）已就绪。

### 10.6.1 N6 ribbon 渲染器 —— 已完成（2026-09-13，二次施工）

**交付物**：

| 文件 | 内容 |
|---|---|
| `Engine/ParticleFX/render/RibbonTrail.h/.cpp` | CPU 侧：历史环形缓冲（固定容量 + 采样间隔累积）、attach 到粒子句柄（死亡经 resolve 失败**自动截断清空**——§7-N6 "death 截断"落地）、`build_strip` 面向相机条带打包（32B `ribbon_vertex` 布局锁存、u16 索引、尾部宽度收窄 50%） |
| `EngineTest/shaders/ParticleFXRibbon.metal` | Metal 顶点/片段 shader：横向羽化（uv.x 边缘平方衰减）+ 头亮尾暗（uv.y 渐变）；**布局用双 float4 表达**（Metal float3 成员 16B 对齐填充坑，见偏差 2） |
| `Engine/Graphics/Passes/ParticleFXRibbonPass.h/.cpp` | 渲染 pass：加色混合 + 深度测试开/写关、per-frame 顶点/索引/常量缓冲、按帧 stage_strip + 索引绘制、首绘一次性日志 |
| `EngineTest/IntegrationTests/TestParticleSponza.cpp/.h` | 集成接线：弹道单粒子发射器（重力 + IntegrateVelocity）→ trail.attach → 每帧 record/build_strip/stage → 渲染图 `ParticleFXRibbonPass`（Load lighting/depth） |

**门禁证据**（2026-09-13）：

- 单测新增 `test_ribbon_trail`（环形回绕封顶 8、死亡截断清空、拓扑 8 样本→16 顶点/42 索引、首尾 uv、横向展开、quad 索引模式）——TestParticleFX 累计 **35 项全绿**。
- Sponza 集成 60s：日志 `ribbon pass initialized` + `[ParticleFXRibbonPass] first draw: 4 verts, 6 indices`（拖尾从 2 样本起步随历史增长）、2625 帧 `pipeline=VALID` 零 fault；mesh pass（1000 实例）同帧共绘。

**对设计的偏差修订**：

1. **Metal struct 对齐坑**：Metal `float3` 结构体成员有 16B 对齐填充（与 C++ simd float3 的 16B 尺寸叠加），32B 顶点用双 `float4`（pos_uv/uv_pad）表达——shader 侧 static_assert 兜底编译期报错。
2. **拖尾跟随对象**：v1 为单粒子拖尾（attach 单句柄，死亡截断）；多粒子多拖尾（每粒子一条）为同构扩展，几何打包代码复用。

### 10.6.2 N6 fluid 渲染器接线 —— 已完成（2026-09-13，三次施工）

**交付物**：

1. **FluidSplat 子 pass 实现**（`Engine/Graphics/Fluid/FluidRenderPass.cpp`）：原 AddPass 中"Skipping for now"的 TODO 落地——图形子 pass 以 `RenderPassDesc`（半分辨率 R16_Float 厚度 Clear+Additive、D32_Float 深度 Clear）驱动**既有** splat 管线（FluidSplat.metal 点精灵球 billboard，深度写入 + 加色厚度），`Draw(6, 0, particle_count, 0)`；衔接下游 Smooth/Normal/Shade 三个 compute 子 pass（原来只在无输入情况下空转）。
2. **Sponza 水柱集成场景**：专用 PFX 发射器（SpawnBurst 2000 + box 柱体采样 + 下落初速 + 重力积分）→ 每帧打包 `float4 (xyz, w=radius)` 上传 `pfxWaterBuffer_` → `FluidInputs{particle_buffer, particle_count, view/proj, camera, scene_color, gbuffer_depth}` → `FluidRenderPass::AddPass`。

**门禁证据**（2026-09-13，Sponza 60s）：`[FluidRenderPass] Initialized (2560x1440)` + `[ParticleFX] fluid pass wired (particles=2000)` + **`[FluidSplat] splatting 2000 particles into half-res depth/thickness`**（子 pass 确在执行）；3132 帧 `pipeline=VALID` 零 fault；mesh 1000 实例与 ribbon 首绘同帧共存。

**实现注记**：接线期修了一个顺序死锁（水柱发射器未 tick ⇒ alive=0 ⇒ 接线块永不执行——tick 前置到条件之前）。渲染深水区剩余：decal 渲染器 + Vulkan 对称路径。

### 10.6.4 N6 decal 渲染器 —— 已完成（2026-09-13，五次施工）

**交付物**：

- `Engine/ParticleFX/render/Decals.h/.cpp`：CPU 侧打包——活粒子 → 48B `decal_instance_data`（球形投影体积：center/radius + color/强度 + 软边比例；布局 static_assert 锁定）
- `EngineTest/shaders/ParticleFXDecal.metal`：延迟贴花 shader——全屏实例绘制（6 顶点 × decal 数），片段采样场景深度 → invViewProj 重建世界位置 → 体积内径向衰减着色（alpha 混合叠到光照输出）；**片段 stage_in 成员须 float4 对齐**（Metal 链接期要求）
- `Engine/Graphics/Passes/ParticleFXDecalPass.h/.cpp`：渲染 pass（描述符 3 绑定：深度纹理/实例缓冲/invVP 常量；`rhi::math::Inverse` 求 invVP；alpha 混合；深度只读）
- Sponza 接线：贴花发射器（box 柱体采样 + 重力 + 平面 clamp——粒子落地停留）→ `pack_decal_instances` → `ParticleFXDecalPass`

**门禁证据**：`decal pass initialized (max_decals=256)` + `[ParticleFXDecalPass] first draw: 32 decals`；2891 帧 `pipeline=VALID` 零 shader 错误；**六大渲染件同帧共存**（mesh 1000 实例 / ribbon 拖尾 / light 32 光斑 / decal 32 贴花 / fluid 2000 粒子喷溅 / soft particles 深度淡出）。

**实现注记**：①贴花发射器需显式 tick（又见"生成前置"死锁类——与 fluid 接线同款，已修）；②v1 深度对齐投影（法线对齐为增强项）；③顺带修复三个**预置流体 shader bug**（FluidSmooth/FluidNormal 的 uint 隐式转换 + `uint2 w/h` 标量化——首次实跑流体管线暴露，均属引擎既有缺陷）。

### 10.6.2c emissive→GI 注入 —— 设计文档 + v1 已完成（2026-09-13）

- **独立设计文档**：`Docs/2026-09-13-pfx-emissive-gi-injection-design.md`——注入点选项分析（A 点光循环 / B 直写探头 / C 聚合槽 / D screen-space）、v1 聚合单槽 + v2 分块空间注入、强度门禁复用（8.0 双重钳制）、差分门禁设计。
- **关键结论修订**：T4.4 已提供完整注入面（SetInstanceEmissive 表 → capture → emissive_atlas → LightEval 钳制 → SC↔probe 反弹），PFX 仅需作为**新生产者**——无需 Lumen 核心改造（推翻本节原"属 Lumen 核心改造"注记）。
- **v1 实现**：`Engine/ParticleFX/interop/EmissiveGI.h/.cpp`——`emissive_gi_injector`（聚合 radiance 逐通道 ≤8.0 钳制、min_alive 门控、保留槽 1023、`emissive_writer` 窄接口由宿主以 SurfaceCachePass::SetInstanceEmissive 适配）。
- **门禁**：新增 `test_emissive_gi_injector`（聚合精确值、8.0 钳制边界、负强度防御、保留槽写入）——TestParticleFX 累计 **36 项全绿**。
- v2 分块空间注入：按设计文档 §3.2 占位，等 DDGI 侧需求排期。

### 10.6.6 R3 Field Preview 线框叠加 —— 数据层完成（2026-09-14）

- `Engine/ParticleFX/render/FieldDebugOverlay.h/.cpp`：KillVolume AABB（12 棱边）与球体（3 正交环×16 段）线框生成（纯 CPU，颜色编码：红=KillVolume、绿=SDF 边界）；单测断言线段数/颜色/球面距离。
- 接入方式：emitter 的 KillVolume 模块参数 → `append_box_wireframe`/`append_sphere_wireframe` → 输出 debug_line 列表 → debug_draw 队列渲染。

### 10.6.3 N6 light 渲染器 —— 已完成（2026-09-13，四次施工）

**交付物**：

- `EngineTest/shaders/ParticleFXLight.metal`：HDR 自发光光斑 shader（点精灵展开 vid0..5×iid、径向平方衰减、屏幕半径随强度/距离缩放）
- `Engine/Graphics/Passes/ParticleFXLightPass.h/.cpp`：32B 光实例（pos_intensity + color_rgb）、加色混合进 RGBA16_Float 光照输出（深度只读遮挡）、per-frame 实例/常量缓冲、`stage_lights`/`execute`
- Sponza 接线：mesh 发射器前 32 个活粒子 → 光实例（颜色取粒子色 × HDR 强度 2.0）→ `ParticleFXLightPass` 渲染图节点

**门禁证据**：`light pass initialized (max_lights=256)` + `[ParticleFXLightPass] first draw: 32 lights`；2562 帧 VALID 零 fault；mesh/ribbon/fluid 同场景共存。

**实现注记**：v1 为"发光 billboard"路径——粒子光以 HDR 加色进光照输出（tonemap 前可见光贡献）；注入 DeferredLighting 点光数组（`DeferredLightingInputs` 当前为单光源）与 DDGI 探头更新属 Lumen 核心改造，需独立设计文档（§8 风险）。

### 10.6.2b N6 soft particles 真实现 —— 已完成（2026-09-13，三次施工）

**交付物**：

| 文件 | 内容 |
|---|---|
| `Engine/Graphics/Metal/shaders/Particle.metal` | ①`ParticleUniforms` 尾部追加 `soft_params`（x=softness y=enabled）；②`ParticleVertexOut` 追加 `screen_uv`/`clip_depth`；③两个顶点函数显式计算 clip 并写出屏幕 uv 与深度（clip.z/w）；④两个片段函数新增 `depth2d` 场景深度采样，深度差淡出 `saturate((scene_d - clip_d)/softness)`，`enabled` 分支旁路保证未绑定时采样不执行 |
| `Engine/Graphics/Passes/ParticlePass.h/.cpp` | `ParticlePushConstants` 尾部追加 `soft_params[4]`（Vulkan UBO 布局兼容——旧字段偏移不变，预编译 SPIR-V 不受影响）；描述符布局加绑定 4（场景深度 CombinedImageSampler，仅软粒子激活时写入）；`set_soft_particles`/`set_scene_depth` API；激活时一次性日志 |
| `EngineTest/IntegrationTests/TestParticleSponza.cpp` | 深度纹理创建后 `set_scene_depth(depthTexture)` + `set_soft_particles(true, 0.002)` |

**门禁证据**（2026-09-13，Sponza 60s）：日志 `[ParticleFX] soft particles enabled (scene depth bound)` + `[ParticlePass] soft particles active (softness=0.002, depth bound)`（深度分支实际执行）；Metal 运行时 shader 编译零错误（depth2d 采样路径合法）；3109 帧 `pipeline=VALID` 零 fault；TestParticles/TestParticleFX 无回归。

**实现注记**：深度比较采用"同空间深度值 + 尺度偏置"（clip.z/w 与深度缓冲同为 Metal [0,1] 空间，单调且免线性化/免投影约定依赖）；材质层 `ParticleMaterial` 的 soft 标志→pass 的委托为后续接线（当前 pass 级 API 直控）。

### 10.7 阶段完成总览（2026-09-13）

| 阶段 | 状态 | 门禁 |
|---|---|---|
| N0 框架骨架 | ✅ 完成 | 7 项单测 + 生产 tick 接线 |
| N1 模块堆栈 + JSON | ✅ 完成 | 13 项单测 + ABI 8 导出 + C# 72 项 |
| N2 渲染器解耦 | ✅ 完成（Metal + Vulkan 双后端实跑） | 36 项单测 + 双后端 60s 门禁（Metal 3251 帧 / Vulkan 3107 帧 VALID，1000 实例首绘） |
| N3 事件 + 刚体消费者 | ✅ 完成 | 4 项含确定性哈希门禁 |
| N4 场系统 + POP 碰撞/平流 | ✅ 完成 | 5 项 + 死代码清理完成 |
| N5 GPU 图发射器 | ⏸ 延后（决议②） | 启动条件见 §3.6 |
| N6 预算/LOD + 渲染件 | ✅ 完成——预算/LOD/ribbon/soft/fluid/light/decal + emissive v1 聚合注入全部落地（六大渲染件 Sponza 同帧共存 2891 帧 VALID） |
| N7 可选项池 | 按需（不默认进） | — |

**回归证据**：TestParticleFX 34 项全绿；TestParticles / TestPhysicsSimulation / TestPhysicsM4 无回归；C# Native 72 项 + Cli 55/56（唯一失败为 dev 分支预置 schema 快照问题，与 PFX 无关）；Sponza 集成冒烟 90s 4888 帧 VALID 零 fault。


### 10.6.5 R2 schema 驱动动态表单 —— 已完成（2026-09-14）

**交付物**：PfxEditorWindowViewModel 从 JSON TextBox 升级为 schema 驱动结构化编辑——
ObservableCollection<PfxModuleRow> 按阶段分组、每模块 PfxParamField 集合可编辑、
SyncToJson 序列化回 JSON、300ms 防抖热预览推送、Undo/Redo 双栈按钮接通 AXAML。
EmitterEditorWindow.axaml 从 JSON TextBox 升级为三列结构化编辑
（阶段 ComboBox + 模块 ListBox + 参数 ItemsControl），x:DataType 编译绑定适配 Avalonia 12。

**门禁**：TestParticleFX 37 项 + C# Native 72 项全绿；dotnet build 0 errors。
