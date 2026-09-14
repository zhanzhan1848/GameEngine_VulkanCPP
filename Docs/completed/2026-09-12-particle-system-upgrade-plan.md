# 粒子系统升级规划 —— 从 sprite 发射器到事件驱动的多形态粒子框架

> **状态（2026-09-12 当日更新）**：本文的架构层结论已被 `Docs/2026-09-12-particle-framework-architecture.md`（PFX 框架）**取代**——固定 config 结构体的渐进扩展路线让位于"属性表 + 模块堆栈"的可编程框架。本文降级保留为：①存量系统现状盘点的依据（§0 仍准确）；②过渡路径参考；③其中 P0 的句柄/死亡通知设计与 P3 的刚体消费者设计已被 N0/N3 吸收，ADR1-7 大部分原文沿用。

- 日期：2026-09-12
- 前置调研：本文 §1 的差距结论来自对 `Engine/Particles/`、`Engine/Graphics/ParticleMaterial.*`、`EngineDLL/Particle*API.cpp` 的逐文件盘点（2026-09-12 完成）。
- 关联文档：`Docs/2026-09-11-physics-simulation-design.md`（Jolt M0–M4 里程碑，P3 阶段的上游依赖）、`Docs/2026-09-11-physics-simulation-survey.md`（§2.4 流体自研路线，P5/P7 的边界）。
- 本文档回答：现有粒子 API 的确切能力边界、目标架构与架构决策、P0–P7 分阶段任务级规划（每阶段含数据结构草案、涉及文件、ABI 增量、验收门禁、风险与工作量）。

---

## 0. 现状盘点

### 0.1 能力清单（以 `emitter_config` 为单一事实源）

| 类别 | 现有能力 | 位置 |
|---|---|---|
| 发射模式 | continuous / burst / ring_buffer；spawn_rate | `ParticleTypes.h:116-134` |
| 发射源 | 发射器单点（position + rotation），`emit_in_local_space` 只旋转初速度 | `ParticleEmitter.cpp:223-227` |
| 初值 | 速度三分量 min-max 矩形随机；颜色 start-end 插值；2D scale 随机 | `ParticleTypes.h:140-154` |
| 力模型 | gravity / wind / drag 三个静态全局量 | `ParticleTypes.h:157-159` |
| 生命期曲线 | scale / alpha / color_gradient / velocity / force / rotation 六条，归一化时间 [0,1] | `ParticleTypes.h:52-99`、`ParticleCurve.h` |
| 池 | 无锁 free-list，per-frame 双缓冲，峰值统计；**裸索引、无 generation、死亡静默** | `ParticlePool.h:20-45` |
| 排序 | CPU 相机距离排序，JobSystem 并行 | `ParticleSorter.h` |
| GPU 路径 | compute 更新/剔除/spawn，indirect draw，10 万上限，统计量回读（非逐粒子） | `GPUParticle*.h/.cpp` |
| 渲染 | billboard quad 实例化 / point sprite；图集动画；4 种混合模式 | `Particle.metal`、`ParticlePass.cpp` |
| 材质 | blend mode、纹理表动画、depth write、receive shadows 标志、**soft particles 标志（shader 未实现）** | `ParticleMaterial.h:18-36` |
| ECS | Particle 组件挂实体，位置跟随 + 偏移，runtime 改 rate/active/burst | `Engine/Components/Particle.h` |
| 预设 | 命名预设 + 分类 + 8 个内置默认 + 文件存取 | `ParticlePreset.h/.cpp` |
| ABI | 约 28 个 EDITOR_INTERFACE 导出（emitter 配置 blob 读写/预设/曲线/材质） | `EngineDLL/ParticleEmitterAPI.cpp:271-449`、`ParticleMaterialAPI.cpp` |
| 编辑器 | 组件只有 PresetName / SpawnRate / IsEnabled 三属性 + 曲线编辑器窗口 | `PrimalEditor_Avalonia/Components/ParticleEmitterComponent.cs` |

### 0.2 已知问题清单（升级中必须处置）

1. **`particle_pool` 无 generation 计数**：裸 u32 索引，`free(index)` 静默回收，外部无法安全持有粒子引用（ABA 复用风险）。这是 P0 的核心改造对象。
2. **`set_collision_handler` 是死代码**：`ParticleCPU.h:58` 声明的碰撞钩子全库零调用点、零使用者。P5 用正式边界系统替代后**删除**，不保留第二事实源。
3. **材质 soft particles 标志已导出 ABI 但 shader 未实现**：`Particle.metal:161` 只有注释。P6 落地或摘除标志，不允许长期"假开关"。
4. **C# 侧按字节偏移硬补配置**：`SceneRuntimeSynchronizer.cs:332` 在 `config[8]` 处写 SpawnRate，依赖 `emitter_config` 的字段布局。升级加字段必须保住这个偏移（见 §6.1）。
5. **`FluidRenderPass` 与粒子系统未接线**：`Engine/Graphics/Fluid/FluidRenderPass.h` 的 `FluidInputs` 已接受 `float4 (xyz=pos, w=radius)` 粒子缓冲，只差渲染管线侧传参。P5 顺路接线。
6. **曲线指针在 config blob 里**：`emitter_config` 含 `emitter_curves`（裸指针），blob 跨进程传输靠 `SanitizeConfig` 清指针 + 曲线单独走 `EmitterSet/GetCurveData`。所有新字段必须保持"blob 可安全 memcmp/memcpy"的纪律。

---

## 1. 差距分析结论（浓缩版）

### vs Unreal Niagara

单项功能缺口：发射形状、mesh/ribbon/贴花/光源渲染器、力场（curl noise/向量场/涡旋）、碰撞、事件管道、任意属性通道、LOD/scalability 七大块整体缺席。**架构差距大于功能差距**：Niagara 的一切建立在"任意命名属性通道 + 可编程模块堆栈 + 类型化事件管道"上，本系统是封闭配置结构体。结论：升级的首要工程不是逐个补功能，而是先把 P0 的"句柄 + payload + 事件"三件基础设施立起来，后续功能都在其上生长。

### vs Houdini

类别不同（离线 authoring + 全物理求解器 vs 运行时特效），不可直接对标。真正有意义的缺口两条：

1. **烘焙缓存消费通道缺失**：无 Alembic/VDB 回放路径，Houdini 产出进不来（P7）。
2. **逐粒子任意属性模型缺失**：Houdini 粒子即属性表；本系统 80 字节固定布局与之相反。P0 的旁挂 payload 通道向该模型靠拢，同一份基础设施同时服务刚体化（P3）与缓存回放（P7）。

---

## 2. 升级目标与非目标

### 目标

1. **事件驱动解耦**：粒子系统只发类型化事件（出生/死亡/碰撞），刚体、贴花、声音、光源等消费者各建各的模块，粒子核心零感知。
2. **粒子刚体化可用**：发射器可发射"带刚体属性的 mesh"，粒子活着则刚体持续模拟，粒子死亡按策略处置；变换权威移交物理。
3. **视觉能力对齐主流**：发射形状、mesh 粒子、ribbon 拖尾、噪声力场、碰撞边界、软粒子、LOD 预算逐阶段补齐。
4. **全程遵守仓库纪律**：ABI 只增不改 + static_assert 锁尺寸、命令面单一事实源、schema 快照随 PR、`DISABLE_PARTICLE_SYSTEM` 宏路径不破坏。

### 非目标

- 不做 Niagara 式可视化模块图表 authoring（编辑器仍走 config + 曲线 + 预设，图表化另立项）。
- 不做 GPU 刚体（物理在 CPU Jolt；刚体化粒子只走 CPU 模拟路径，见 ADR5）。
- 不做完整流体求解器（SPH/FLIP 归物理调研文档 §2.4 的后续立项，本计划只做边界碰撞与 FluidRenderPass 外观接线）。
- 不重写现有 CPU/GPU 双路径骨架——六条曲线、预设系统、排序、间接绘制全部保留复用。

---

## 3. 目标架构

```
┌─ 编辑器 (Avalonia)                        ┌─ 宿主（PipelineTick 语义沿用物理文档 §0）
│  ParticleEmitterComponent + Inspector     │
│  （新增字段经 UndoRedo + SceneRuntime 同步） │
└───────────┬───────────────────────────────┴──────────┬─────────────┘
            │ ABI（只增不改，config blob 追加式扩展）        │
┌───────────▼───────────────────────────────────────────▼─────────────┐
│ Engine/Particles/  粒子核心（纯模拟：点 + 寿命 + 曲线 + 力场 + 边界）      │
│   particle_pool(句柄化) · particle_emitter · gpu_particle_system      │
│   ★ 事件出口：birth/death/collision → 类型化 POD 事件队列（每帧消费一次）  │
│   ★ payload 旁挂缓冲：按句柄索引的固定步长自定义数据（刚体参数/mesh id…）   │
└───────────┬─────────────────────────────────────────────────────────┘
            │ 事件 + 句柄（粒子核心不知道消费者存在）
┌───────────▼─────────────────────────────────────────────────────────┐
│ Engine/Particles/Consumers/（新目录，消费者模块）                        │
│   RigidBodySpawner：birth → 建 Jolt body + mesh 实例；                  │
│                     death → destroy/freeze/orphan 策略；               │
│                     预算否决（上限/挤旧/距离 LOD）；变换权威归物理          │
│   （未来：DecalSpawner / SoundTrigger / LightFlicker 同管道接入）        │
└───────────┬──────────────────────────────┬──────────────────────────┘
            │ body_desc（纯引擎类型）         │ mesh handle
┌───────────▼──────────────┐   ┌───────────▼──────────────────────────┐
│ Engine/Simulation/ Jolt  │   │ 渲染：sprite 路径(现有) + mesh 粒子路径  │
│ （隔离纪律：头文件不出层）   │   │ (新) + ribbon (新) + FluidRenderPass   │
└──────────────────────────┘   └──────────────────────────────────────┘
```

### 架构决策记录（ADR）

**ADR1 粒子数据布局冻结。** `particle_data` 80 字节布局永不再动（GPU shader/MoltenVK/Vulkan 双后端已对齐）。一切每粒子扩展数据走 payload 旁挂缓冲。

**ADR2 稳定句柄 = (index, generation) 打包 u64。** 池为每槽位维护 u32 generation，`allocate` 时自增；`free` 时死亡事件携带句柄。外部一律持句柄不持裸索引，`pool.resolve(handle)` 失败即粒子已死。

**ADR3 变换权威单写。** 刚体化粒子出生瞬间，变换所有权移交 Jolt：渲染读 body transform（经 ParticleBodyMap，模式照抄 `Simulation/BodyEntityMap.h`），粒子槽位退化为"寿命记录 + 事件锚点"，**禁止**再回写粒子 position。此决策防止双积分漂移，是 P3 的正确性根基。

**ADR4 事件管道 = 类型化 POD + 环形缓冲 + 单帧消费。** 事件结构 POD、带 u64 句柄，per-emitter 环形缓冲，帧末由消费者拉取。消费者缺席时事件自然丢弃（零成本），不积压。

**ADR5 路径分工。** 刚体化、事件型、payload 型粒子**仅 CPU 路径**（数量受物理预算约束，CPU 足够且死亡通知天然精确）。GPU 路径保持纯视觉特效定位，P0 不给 GPU 路径加死亡回读（GPU 逐粒子死亡需 readback，收益不配成本；若未来需要，独立子项）。

**ADR6 预算否决优先于契约。** "粒子活着刚体就活着"的契约受物理预算门限约束：`max_bodies` 耗尽时按策略（拒新/挤最老，emitter 可配）降级，且被拒粒子在 payload 标记 `degraded` 供调试可视化。物理正确性 > 特效完整性。

**ADR7 死代码不留。** `set_collision_handler`（ParticleCPU）在 P5 边界系统合入同一 PR 内删除。

---

## 4. 阶段规划（P0–P7）

> 编号用 P 前缀，避免与物理文档 M0–M7 冲突。工作量为人日粗估（含测试与文档），按"单人熟悉仓库"口径。每阶段合入前跑 `./scripts/ci_local.sh` + `ctest -R "Unit"` + 粒子集成测试窗口冒烟。

### P0 基础设施：句柄 / 死亡通知 / payload / 事件队列（~6-8 人日）★ 一切的前置

**目标**：让粒子从"匿名渲染点"变成"可被外部系统引用与订阅的一等实体"。

**设计要点与数据结构草案**：

```cpp
// ParticleTypes.h 新增
using particle_handle = u64;                     // 低32位 index，高32位 generation
constexpr particle_handle invalid_particle_handle{ u64_invalid_id };

enum class event_type : u8 { birth = 0, death = 1, collision = 2 };  // collision P5 启用

struct particle_event {                          // POD, 24B
    particle_handle handle;
    emitter_id    source;
    event_type    type;
    f32           normalized_age;                // 死亡时的寿命进度
};

// ParticlePool.h 改造
//   - 新增 std::unique_ptr<u32[]> _generations（每槽位）
//   - allocate() 返回句柄并 ++generation；free(index) 由发射器在写完死亡事件后调用
//   - resolve(handle) -> particle_data*（generation 校验，失败返回 nullptr）
```

- **死亡通知**：CPU 发射器 update 循环里判定 `age >= lifetime` 的粒子，先写 death 事件再归还槽位（`ParticleEmitter.cpp:44` 附近的生命期判定处）。每发射器每帧事件数上限 = 该帧死亡数，环形缓冲容量按 max_particles 配 1/4，覆盖即丢（debug 下 PRIMAL_LOG 告警）。
- **payload 旁挂**：`emitter_config` 追加 `u32 payload_stride{0}`（尾部追加，见 §6.1）；stride>0 时发射器按容量分配等长侧缓冲，`set_payload(handle, void*, size)` / `get_payload(handle)`。粒子核心不解析内容。
- **事件出口**：`particle_emitter::drain_events(particle_event* out, u32 cap, u32* out_count)`；系统级聚合入口 `particles::drain_all_events()` 供消费者每帧调用一次。
- **测试**：句柄复用 ABA 用例（分配→死亡→再分配，旧句柄必须 resolve 失败）；death 事件顺序与数量门禁；payload 随生随灭不泄漏。

**ABI 增量**：暂不导出（事件/payload 是引擎内接口，消费者模块同进程）。若后续 C# 需要读事件，追加只读导出。

**风险**：`particle_pool` 是无锁 free-list，generation 改造需保住 CAS 语义——generation 数组单独管理、不在 CAS 路径内。GPU 路径（`gpu_particle_data = particle_data` 别名）确认零影响。

---

### P1 发射形状系统（~4-5 人日）

**目标**：发射源从单点升级为形状体积 + 方向分布，特效表现力第一梯队补齐。

**设计要点**：

```cpp
// emitter_config 尾部追加
enum class emission_shape : u8 { point = 0, sphere = 1, box = 2, cone = 3, cylinder = 4 };
emission_shape shape{ emission_shape::point };
math::v3       shape_params{ 1, 0, 0 };        // sphere: x=半径；box: 半尺寸；cone: x=角度 y=高度
enum class emission_direction : u8 { velocity_range = 0, shape_normal = 1, uniform_sphere = 2 };
emission_direction direction_mode{ emission_direction::velocity_range };
f32            inherit_source_velocity{ 0.f };  // 0-1，继承实体速度（P3 刚体喷射的关键参数）
```

- 采样实现集中在 `particle_emitter::initialize_particle`（`ParticleEmitter.cpp:223`），形状采样 + 方向采样两个纯函数独立成文件 `ParticleEmissionShape.h/.cpp`（可单测）。
- `emit_in_local_space` 语义升级：形状采样在发射器局部空间进行，再经 rotation 变换到世界。
- GPU 路径本轮**不做**形状（`gpu_emitter_config` 256B uniform 需动 shader 布局，留 P6 评估）；GPU 发射器忽略 shape 字段继续点发射，config 布局兼容由字段尾部追加保证。
- 曲线系统不动。`ParticleExamples.h` 补 sphere/cone 示例预设。

**验收门禁**：`ctest -R TestParticles` 新增形状分布统计用例（10 万粒子采样落球壳/锥体积的直方图断言）；`TestParticleSponza` 窗口冒烟 + 一张 sphere 形状渲染 PNG 产物。

**风险**：低。纯本地改动，不触碰布局冻结对象。

---

### P2 Mesh 粒子渲染器（~8-10 人日）

**目标**：每粒子可渲染一个静态 mesh 实例（instanced mesh），为 P3 的"刚体 mesh"提供渲染半边。

**设计要点**：

- **渲染数据旁挂**（不进 `particle_data`）：mesh 粒子发射器额外维护 per-instance 变换缓冲 `position(quat) scale` = 48B/实例。粒子槽位照旧积分 position；renderer 每帧从粒子 position + 粒子朝向（新增**自旋速率**曲线槽，3 轴欧拉，写进 payload 或独立侧缓冲）组装实例缓冲。
- **emitter_config 尾部追加**：`u32 mesh_id{u32_invalid_id}`（复用现有 mesh/资产句柄体系）、`bool mesh_particles{false}`。`mesh_particles=true` 时走 mesh 渲染路径，billboard 路径休眠。
- **绘制路径**：Metal 侧 `Particle.metal` 新增 `particle_mesh_vertex`（读 mesh VB + 实例变换），indirect indexed instanced draw；Vulkan 侧对称实现。复用 `draw_indirect_command` 的实例计数（由剔除 pass 维护）。
- **排序**：首版仅支持 opaque/alpha-test 材质（不做逐粒子深度排序，物理 mesh 场景天然合理）；透明 mesh 粒子明确不支持，文档写死。
- **经验收可视化**：Sponza 场景内 1000 实例 cube mesh 粒子喷泉，双后端（Metal + DebugVulkan 树）截图对比。

**验收门禁**：`TestParticleSponza` 扩展 mesh 粒子场景，帧序列 + 亮度 CSV；双后端各跑 60s 无 fault；`TestParticleMaterialAPI` 不回归。

**风险**：RHI 双后端 indirect indexed draw 的能力差异（Metal 借 `drawIndexedPrimitives` indirect 变体）；Vulkan 树混链坑（AGENTS.md 已有纪律：切树先 `rm libEngine.a`）。

---

### P3 粒子刚体化：事件消费者 + Jolt 接线（~10-12 人日）★ 解耦方案的落地

**上游依赖**：物理文档 M0（box/sphere body 闭环）已合入；convex_hull 形状按物理文档节奏（M1/VHACD）——P3 首版允许用 **box/sphere proxy** 发射，convex_hull 就绪后自动升级。

**目标**：发射器可声明"我发射的是刚体 mesh"；粒子出生 → 建 body + mesh 实例，粒子活着 → Jolt 持续模拟，粒子死亡 → 按策略处置。

**设计要点**：

```cpp
// Engine/Particles/Consumers/RigidBodySpawner.h（新，Engine/Simulation 之外、Particles 之内）
struct rigid_spawn_policy {                     // POD，随 emitter_config 尾部追加（序列化侧）
    u32  mesh_id{ u32_invalid_id };             // 渲染 mesh
    u8   body_shape{ /* box/sphere/convex_hull */ };
    f32  mass{ 1.f };  f32 restitution{ 0.2f };  f32 friction{ 0.5f };
    u8   on_particle_death{ 0 /*destroy*/ | 1 /*freeze*/ | 2 /*orphan: 转独立实体常驻*/ };
    u32  max_live_bodies{ 256 };                // ADR6 预算
    u8   overflow_policy{ 0 /*reject_newest*/ | 1 /*retire_oldest*/ };
    f32  cull_distance{ 0.f };                  // >0：相机超距不建 body，粒子纯视觉
};

class rigid_body_spawner {                      // 每个 emitter 可选挂一个
    void tick(f32 dt);   // drain_events → birth: 建 body(ShapeFactory) + ParticleBodyMap 登记
                         //            death: 按 on_particle_death 处置 + 摘 map
                         //            每帧把 body transform 写入 P2 的 mesh 实例缓冲（ADR3 单写权威）
};
```

- **变换权威**（ADR3）：body 建立后渲染实例缓冲由 spawner 从 Jolt 读出写入，粒子槽位的 position 字段对 mesh 粒子**只读不用**。调试可视化：`SimulationDebugDraw` 线框 + 粒子 sprite 叠加，肉眼校验无双写抖动。
- **接线点**：宿主 tick 顺序 = `particles::update → rigid_body_spawner::tick → simulation::world::step`（step 在前还是在后需与物理文档 §1.3 的 PipelineTick 顺序对齐——采用"物理先 step、spawner 后消费"序，避免一帧延迟）。
- **挤旧策略**（ADR6）：retire_oldest 触发时对最老 body 所属粒子强制提前死亡（发 death 事件），生命周期闭环不悬空。
- **Cull/LOD**：超距粒子继续走 billboard 微缩渲染或直接不画，body 不建；回距内重建 body 并从粒子 position 续接（freeze→thaw 语义）。

**验收门禁**：新增 `EngineTest/UnitTests/Particles/TestRigidBodySpawner.cpp`——确定性门禁沿用物理文档的哈希门禁思路（固定种子发射 N 个 box 刚体，step 300 帧，transform 哈希比对）；可视化：刚体瀑布堆叠 mp4 + 接触瞬间帧 PNG；`ctest -R "Unit"` 全绿；物理文档的单线程确定性模式（`--physics-threads=0`）下跑。

**风险**：中。跨 Particles/Simulation 两层的时序接线；宿主 tick ABI 若物理文档尚未合入 PipelineTick，则本阶段在测试可执行体内自驱 tick（与物理文档 §0 的测试驱动方式一致），宿主接线延后。

---

### P4 力场升级（~5-6 人日）

**目标**：力模型从三个静态全局量升级为可叠加力场列表。

**设计要点**：

```cpp
// emitter_config 尾部追加
enum class force_field_type : u8 { curl_noise = 0, vortex = 1, attractor = 2, point_gravity = 3 };
struct force_field_desc {                       // 32B POD，数组上限 8
    force_field_type type;
    math::v3 params0, params1;                  // 按 type 解释：频率/强度/半径/轴向…
    f32 strength;  u32 curve_slot{ u32_invalid_id };  // 强度可挂现有 float_curve
};
u32 force_field_count{ 0 };
force_field_desc force_fields[8];
```

- CPU 积分：`ParticleEmitter.cpp` update 循环内逐场求和，替换/叠加现有 gravity/wind/drag（三者保留为兼容隐式场）。
- curl noise 用 simplex 梯度噪声 CPU 实现（`ParticleEmissionShape.h` 同层新增 `ParticleNoise.h`，纯函数可单测）；GPU 侧留给 P6+（compute shader 加场数组）。
- 力场随预设序列化（`particle_preset` 扩展）。

**验收门禁**：curl noise 场的粒子云散布 PNG 黄金图对比（固定种子确定性）；`TestParticles` 力场求和纯函数用例。

**风险**：低。性能上 8 场 × 1 万粒子 CPU 可承受（每帧 8 万次噪声求值，JobSystem 可并行——复用 `ParticleSorter` 的 JobSystem 模式）。

---

### P5 碰撞与边界（~8-10 人日）

**目标**：粒子可被几何边界约束（反射/kill/停留），覆盖"流体模拟边界"诉求的碰撞半边（求解器归流体立项，见非目标）。

**设计要点**：

```cpp
// emitter_config 尾部追加
enum class boundary_type : u8 { none = 0, aabb = 1, sphere = 2, plane = 3, sdf_field = 4 };
enum class boundary_response : u8 { kill = 0, bounce = 1, slide = 2, clamp_inside = 3 };
struct boundary_desc {                          // 上限 4
    boundary_type type;  boundary_response response;
    math::v3 params0, params1;                  // AABB 两角 / 球心半径 / 平面点法
    f32 restitution{ 0.3f };
    u32 sdf_asset_id{ u32_invalid_id };         // 复用引擎 SDF 资产（ContentTools/SDF、OfflineSDFMerger 产物）
};
```

- **CPU 路径**：解析体（AABB/球/平面）在积分步内做连续碰撞检测（线段 vs 体）；SDF 边界采样资产体数据做梯度反弹（SDF 读回设施已有：`Engine/Graphics/PCG/PCGSDFReadbackManager.h`、`ContentTools/common/SDF.cpp`）。
- **GPU 路径**：屏幕空间深度缓冲碰撞（`ParticleCompute.metal` 更核读 scene depth，标准技法），参数进 `gpu_emitter_config` 追加段（shader 布局次同步）。
- **collision 事件启用**：P0 的 `event_type::collision` 在首次反弹时发出（带法线），节流（每粒子每 0.1s 最多一枚）。
- **死代码处置**（ADR7）：删除 `ParticleCPU.h/.cpp` 的 `set_collision_handler` 全套。
- **FluidRenderPass 接线**：渲染管线把 GPU 粒子位置缓冲喂给 `FluidInputs::particle_buffer`（`FluidRenderPass.h:20`），加一个"水柱"集成测试场景——这是对"粒子做流体"诉求的外观级交代，求解器另行立项。

**验收门禁**：AABB 反弹能量衰减曲线 CSV（ restitution=1 时守恒断言）；SDF 边界内粒子云不穿透 PNG；`TestParticleSponza` + 水柱 FluidRenderPass 帧序列。

**风险**：SDF 采样在 CPU 侧需要资产读回时机管理（懒加载 + 熔断）；GPU 深度碰撞在 MSAA/分辨率缩放下的一致性——先固定无 MSAA 路径。

---

### P6 Ribbon/Trail + 软粒子落地 + LOD 预算（~8 人日）

1. **Ribbon 渲染器**：per-emitter 历史位置环形缓冲（可配采样间隔），相机面向条带几何，宽度/颜色挂现有曲线。 适用拖尾特效；mesh 粒子互斥。
2. **软粒子落地**：`Particle.metal` 补场景深度淡出（soft particles 标志从"假开关"转真实现，或摘除——以实现为准，`ParticleMaterial.h:35` 的 ABI 不动）。
3. **LOD/预算**：发射器级 spawn rate 按屏幕占比缩放；全局 `particles::set_global_budget(…)`;
   重要性剔除（距离 × 屏幕占比）；debug HUD 显示各发射器预算占用（照物理文档可视化哲学）。

**验收门禁**：ribbon 视觉 mp4；软粒子近墙淡出 PNG 对比；预算门禁——预算砍半后帧时长线性下降的断言。

**风险**：ribbon 的历史缓冲与粒子死亡的事件一致性（拖尾端点必须随 death 事件截断）。

---

### P7 远期占位：Houdini 缓存回放（另立调研文档后排期）

- Alembic 位置缓存回放组件（渲染侧消费，不参与模拟）；payload 通道（P0）天然可承载缓存属性列。
- 前置：资产管线扩 Alembic importer（走 `ContentTools` 离线转换，遵循"离线烘焙优先"的仓库惯例）。
- 本文只占位不展开；启动前按仓库惯例先出 `*-survey.md`。

---

## 5. 测试与验收纪律（全阶段通用）

1. **纯函数单测**（无 GPU）：形状采样分布、力场求和、句柄 resolve、事件环形缓冲——进 `EngineTest/UnitTests/Particles/`，挂 `ctest -R Unit`。
2. **可视化孪生**：每个模拟特性至少一个带渲染产物的集成用例（PNG 帧 / 亮度 CSV / mp4），沿用物理文档"每条测试必须有可视化产物"铁律；载体复用 `TestParticleSponza` 窗口式集成测试（60-120s kill + 日志无 fault）。
3. **双后端**：渲染类验收 Metal（Darwin/Debug）与 Vulkan（Darwin/DebugVulkan）各跑一遍；注意切树先 `rm Darwin/Debug/libEngine.a`。
4. **确定性**：一切模拟类断言固定 RNG 种子（`particle_emitter` 的 `std::mt19937` 需暴露 seed 注入——P0 顺路补 `set_seed(u32)`）。
5. **门禁脚本**：`./scripts/ci_local.sh` 全绿 + 粒子相关 ctest 目标（`TestParticles`、`TestParticleMaterialAPI`、新增用例）显式列出。

## 6. ABI / 编辑器纪律与已知坑

### 6.1 `emitter_config` 演进纪律（P0 起生效）

- **新字段一律尾部追加**，永不插入中段、永不删除旧字段。现有 C# 侧按 `ParticleEmitterConfigSize()` 动态分配 blob（`SceneRuntimeSynchronizer.cs:322-324`），尾部追加天然兼容。
- **`static_assert(offsetof(emitter_config, spawn_rate) == 8)`** 进 P0：守护 C# 硬编码偏移（`SceneRuntimeSynchronizer.cs:332`），布局再演进时编译期报警。
- blob 内禁止新增裸指针字段（curves 指针是历史包袱，由 `SanitizeConfig` 兜底）；新复杂数据一律 `u32 asset_id` 间接引用。
- `ParticleEmitterConfigSize()` 导出无需改动（sizeof 自动反映），但 `Docs/` 本文档需随每次布局演进更新字段表。

### 6.2 编辑器通道

- 新增的可配置字段（形状/payload 策略/刚体策略/边界）进 `ParticleEmitterComponent.cs` 时走 Scene/GameEntity + UndoRedo 同路径（文档模型优先纪律），PresetName/SpawnRate/IsEnabled 三属性模式扩展为分组面板。
- 若新增 `particle.*` CLI 命令：description 写清单位/坐标系/前置条件/失败语义，`PRIMAL_UPDATE_SCHEMA=1` 重生成快照随 PR 提交。
- 曲线槽位新增（自旋、力场强度）走既有 `EmitterSet/GetCurveData` 槽位枚举扩展，不新增命令。

## 7. 里程碑总表

| 阶段 | 内容 | 估工 | 依赖 | 解锁能力 |
|---|---|---|---|---|
| P0 | 句柄/死亡通知/payload/事件队列 | 6-8 日 | 无（种子注入顺路） | 一切后续；事件消费者模式 |
| P1 | 发射形状 + 方向分布 | 4-5 日 | P0（inherit_source_velocity 用句柄语义无强依赖，可并行） | 特效表现力第一梯队 |
| P2 | Mesh 粒子渲染器 | 8-10 日 | 无硬依赖（建议 P0 后） | 刚体 mesh 的渲染半边 |
| P3 | 粒子刚体化消费者 | 10-12 日 | P0+P2 + 物理 M0（M1 升级 convex_hull） | 刚体模拟诉求闭环 |
| P4 | 力场升级 | 5-6 日 | P0 | 噪声/涡旋/吸引 |
| P5 | 碰撞边界 + Fluid 接线 + 死代码清理 | 8-10 日 | P0（事件）；物理 M0 可选（场景查询） | 流体边界诉求（碰撞半边） |
| P6 | Ribbon + 软粒子 + LOD 预算 | 8 日 | P0 | 表现力收尾 + 规模化 |
| P7 | Houdini 缓存回放 | 另立文档 | P0、资产管线 | 离线烘焙管线 |

关键路径：**P0 → P3**（约 20-25 人日达成用户核心诉求"粒子作为发射源、发射带刚体属性的 mesh、解耦不强绑定"）。P1/P2/P4/P5 相互独立，可按需穿插。

## 8. 与其他系统的对齐

- **物理（`Docs/2026-09-11-physics-simulation-design.md`）**：P3 的 body_desc/PipelineTick 序/确定性门禁/单线程模式全部沿用其约定；ShapeFactory 的 M1（convex_hull/VHACD）决定 P3 何时从 proxy 升级任意 mesh。
- **流体**：本计划只做边界碰撞与 FluidRenderPass 外观接线；SPH/FLIP/PBF 求解器按物理调研 §2.4 的结论另行立项。
- **GI/Nanite**：无交集；唯一注意项是粒子不进 Lumen 表面缓存（现状如此，维持）。
- **AGENTS.md 纪律**：ABI 只增不改、static_assert 锁尺寸、PRIMAL_LOG 走 stderr、命令面单一事实源、schema 快照——各阶段 PR 自查。
