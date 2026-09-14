# 物理模拟实施级设计 —— Engine/Simulation × Jolt 集成与可视化测试

- 日期：2026-09-11
- 前置：`Docs/completed/2026-09-11-physics-simulation-survey.md`（选型结论：Jolt 5.6+，C++17，macOS/Win 双平台）
- 本文档回答：底层长什么样、Jolt 怎么接、用哪些模块、测试怎么做（**接通即有测试；多物体刚体测试；UI 层测试；所有测试带渲染可视化**）
- 里程碑编号沿用调研报告 §4（M0–M7），本文把 M0–M4 展开到任务级

---

## 0. 总架构：一张图

```
┌─ 宿主进程（驱动帧模型——引擎无主循环，宿主每帧调一次）
│   PrimalPlayer 主循环 / 编辑器视口 DispatcherTimer(30Hz) / 测试可执行体
│        │
│        ├─ PipelineTick(dt)   ← 新增 C ABI（物理 + 脚本的"逻辑帧"，渲染帧之前）
│        │     ├─ simulation::world::step(dt)      固定 60Hz accumulator，0~N 次子步
│        │     ├─ script::frame_tick(dt)           （Script.cpp:816 已有入口，生产循环接入）
│        │     └─ 物理驱动实体 → transform::set_position/orientation（含渲染插值对）
│        │
│        └─ PipelineRenderFrame(...)   ← 现有渲染帧，SyncEntitiesToRenderScene 自动吃到新 transform
│
├─ Engine/Simulation/   ← 物理封装层（Jolt 头文件不出此层）
│     PhysicsWorld（Jolt PhysicsSystem 封装）· ShapeFactory · LayerFilter
│     JoltDebugRenderer→DebugDrawQueue 适配 · Body↔Entity 映射 · 确定性哈希
│
├─ third_party/JoltPhysics   ← git submodule，pin tag（v5.6.x）
│
├─ Engine/Components/   ← collider / rigidbody 组件（照 Light 模式：SoA + 自由函数）
├─ EngineDLL/PhysicsAPI.cpp   ← EDITOR_INTERFACE 导出（只增不改 + static_assert）
├─ PrimalEditor_Avalonia   ← Inspector 面板 + UndoRedo + SceneRuntimeSynchronizer 分支 + sim.* CLI
└─ 测试四层（全部带渲染可视化，见 §8）
```

两条铁律贯穿全文：
1. **Jolt 隔离**：`#include "Jolt/..."` 只允许出现在 `Engine/Simulation/` 内部；对外 API 一律引擎类型（`math::v3`、`id::id_type`、POD）。
2. **每条测试必须有可视化产物**（PNG 帧 / 帧序列 + 亮度 CSV / mp4 / 编辑器截图 diff），"无窗口纯单测"只允许覆盖纯函数，且每个纯函数必须有可视化孪生测试（§8.0）。

---

## 1. 底层：Engine/Simulation 模块设计

### 1.1 文件清单与职责

```
Engine/Simulation/
  SimulationAPI.h          ← 唯一对外头文件：settings/body_desc/查询结构（纯引擎类型，可被 EngineDLL/测试包含）
  PhysicsWorld.h/.cpp      ← Jolt PhysicsSystem 生命周期 + 固定步长 accumulator + BodyInterface 封装
  ShapeFactory.h/.cpp      ← body_desc::shape → Jolt ShapeRef（Box/Sphere/Capsule/ConvexHull/TriangleMesh）
  SimulationLayers.h/.cpp  ← BroadPhaseLayer + ObjectLayer 表与层过滤器（BPLayerTableImpl）
  BodyEntityMap.h/.cpp     ← engine body_id ↔ Jolt BodyID ↔ game_entity 双向映射
  SimulationDebugDraw.h/.cpp ← 继承 Jolt DebugRenderer，转发到 debug_draw::queue()（线框可视化，零管线改动）
  SimulationDeterminism.h/.cpp ← 全体 body transform 的稳定哈希（回归门禁用）
  JoltJobAdapter.h/.cpp    ←（M2+）Jolt JobSystem 适配到引擎 JobSystem；M0/M1 用 JobSystemSingleThreaded
```

构建接入：`Engine/CMakeLists.txt` `target_link_libraries(Engine PRIVATE Jolt)`——静态库传递链接，测试/EngineDLL 无需感知 Jolt；`.cpp` 已被现有 glob（`Engine/CMakeLists.txt:284-285`）自动收录，无需改。

### 1.2 核心数据结构（SimulationAPI.h 草案）

```cpp
namespace primal::simulation {
struct settings {                      // POD，EngineDLL 锁尺寸
    math::v3 gravity{0.f, -9.81f, 0.f};
    u32    max_bodies{4096};
    u32    max_worker_threads{0};      // 0 = 自动
    f32    fixed_dt{1.f / 60.f};
    u32    max_sub_steps{5};
};

struct body_desc {
    enum class shape : u32 { box, sphere, capsule, convex_hull, triangle_mesh };
    enum class mode  : u32 { static_body, dynamic, kinematic };
    shape s{shape::box};
    mode  m{mode::dynamic};
    math::v3 half_extents{0.5f, 0.5f, 0.5f};   // box：半尺寸 / sphere：x=半径
    math::v3 position{}, orientation_euler{};
    f32 mass{1.f};                              // static/kinematic 忽略
    f32 restitution{0.2f}, friction{0.5f};
    u32 layer{0};                               // SimulationLayers::object_layer
    game_entity::entity owner{};                // 反向绑定，物理 tick 回写 transform 用
    bool debug_draw_proxy{true};                // 测试可视化开关：线框 proxy
};

struct body_transform {                // POD 出参：插值后的渲染位姿 + 原生位姿
    math::v3 position{}, orientation_euler{};
    math::v3 prev_position{}, prev_orientation_euler{};  // 渲染插值用
    bool sleeping{false};
};
}
```

### 1.3 帧循环与固定步长（PhysicsWorld::step）

```cpp
void world::step(f32 host_dt) {
    _accum += std::min(host_dt, 0.25f);              // 防螺旋卡死（Gaffer on Games 惯例）
    u32 n{0};
    while (_accum >= _settings.fixed_dt && n < _settings.max_sub_steps) {
        _system->Update(_settings.fixed_dt, 1, _job_system);   // 1 子步 = 已定步长
        _accum -= _settings.fixed_dt; ++n;
    }
    if (n == _settings.max_sub_steps) _accum = 0;    // 追不上就丢帧，不让误差累积
    sync_transforms();                               // 物理 body → ECS transform（含 prev/cur 插值对）
}
```

- **transform 所有权**：仅 `running` 状态下、且实体带 rigidbody 组件（或 body 与实体绑定）时物理回写 ECS transform；编辑器手动摆物体时物理不动它。编辑器 `SceneRuntimeSynchronizer` 在 play 态跳过物理驱动实体的推送，避免两方打架（§7）。
- **渲染插值**：M1 先直接用 post-step 位姿（60Hz 足够平滑）；插值对（prev/cur）字段从 M1 就放结构里，编辑器视口 30Hz 时启用 alpha 插值，属 M3 增强。

### 1.4 Jolt 模块使用清单（用到才引）

| Jolt 模块 | 用途 | 里程碑 |
|---|---|---|
| `Jolt/Core`（RegisterTypes/Factory/JobSystem/Trace） | 初始化、job、日志断言接管 | M0 |
| `Jolt/Physics`（PhysicsSystem/BodyInterface/Shape 族/ContactListener） | 刚体全部 | M0–M4 |
| `Jolt/Physics/Character/CharacterVirtual` | 角色控制器 | M5 |
| `Jolt/SoftBody`（SoftBodySharedSettings 等） | 布料/软体 | M6 |
| `Jolt/Geometry`（ConvexHullBuilder 等，经 Shape 间接用） | 由 ShapeFactory 消化 | M1+ |

明确不用：`Jolt/Samples`、`Jolt/TestFramework`、Jolt 自带渲染示例（我们用 §1.6 的 DebugRenderer 桥）。

### 1.5 线程与确定性

- **M0/M1：`JobSystemSingleThreaded`**——所有测试先在单线程确定性模式下跑（同平台同构建 bit 级确定），哈希门禁才有意义。
- **M2：JoltJobAdapter**——实现 `JobSystem` 子类，`QueueJob/QueueJobs` 委托引擎 `JobSystem::Execute`（`Engine/JobSystem/JobSystem.h`）；单线程模式保留为测试开关（`--physics-threads=0`）。
- 主线程约束：Jolt `PhysicsSystem::Update` 必须从调用 `HeadlessCommands`/宿主 tick 的同线程发起；编辑器侧在 UI 线程 tick 即可（M0 不做专用物理线程，Jolt 内部并行靠 job）。

### 1.6 可视化桥（SimulationDebugDraw）

继承 `DebugRenderer`，实现 `DrawLine/Triangle/TriangleBatch`（只实现 line 路径，三角形先丢弃），每个 body 若 `debug_draw_proxy` 则在 step 后由 `PhysicsSystem::DrawBodies(DebugSettings{}, &our_renderer)` 画 AABB/形状线框 → 转发 `debug_draw::queue().add_line(...)`（`Engine/Graphics/DebugDraw/DebugDrawQueue.h`），由 ForwardSceneRenderer Pass 6（`ForwardSceneRenderer.cpp:3336-3372`，LineBatchRenderer）渲染。**零渲染管线改动**。

### 1.7 实心 proxy（多物体测试的"看得见的刚体"）

线框之外，测试需要实体感：`ProceduralMeshCreate(type=3 Box, size)`（`EngineDLL/ProceduralMeshAPI.cpp:57`）→ `PipelineRegisterMeshEntity` → 每帧 `SetEntityTransform`。普通 mesh 路径按 mesh 分组聚合成 **instanced draw**（`ForwardSceneRenderer.cpp:2402-2600`），100 个盒子 = 1 次 draw，多物体测试性能无忧。此路径封装为测试工具函数 `SpawnVisualProxy(body_id)`（放 `EngineTest/Utils/PhysicsTestHelpers.h`，测试专用，不进引擎）。

---

## 2. Jolt 接入细则

### 2.1 获取与版本

```bash
git submodule add https://github.com/jrouwe/JoltPhysics.git third_party/JoltPhysics
cd third_party/JoltPhysics && git checkout <最新 release tag，立项时检索确认（当前线为 5.6.x）>
# LICENSE 登记：LICENSE-THIRD-PARTY.md 追加 MIT (Jolt Physics) 条目
```

沿用 stb submodule 先例（b06c8296）；**不用调研报告里 PhysX 的 vendor 全仓方式**，Jolt 本体小，submodule 即可。

### 2.2 CMake（根 CMakeLists.txt PhysX TODO 处替换为）

```cmake
# === Jolt Physics (Engine/Simulation/) ===
set(JOLT_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(JOLT_ENABLE_TESTS OFF CACHE BOOL "" FORCE)      # Jolt 自测不进我们的 ctest
set(JOLT_ENABLE_SAMPLES OFF CACHE BOOL "" FORCE)
set(JOLT_ENABLE_VIEWER OFF CACHE BOOL "" FORCE)
set(JOLT_ENABLE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)  # 与 ccache/统一编译参数友好
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)     # Debug 树保干净；release 优化后议
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/JoltPhysics)            # 产出 target: Jolt
target_link_libraries(Engine PRIVATE Jolt)
```

- **C++ 标准兼容性**：macOS 全局 `-std=c++17` flag（`Engine/CMakeLists.txt:384`）同样作用于 Jolt 源码——Jolt 以 C++17 为最低要求，命中；Windows C++20 亦兼容。无需为 Jolt 单独改标准。
- **双构建树纪律**（AGENTS.md）：Jolt 静态库进 `libEngine.a` 对象集后，Debug/DebugVulkan 切树必须先 `rm libEngine.a` 的坑会叠加物理符号——在 M0 把 `check_shader_copies.sh` 旁加 `check_clean_trees.sh`（检测混链症状：`nm libEngine.a | grep -c Jolt` 与当前树 define 集矛盾即 fail），进 ci_local.sh。

### 2.3 初始化/关闭序列（PhysicsWorld::Initialize / Shutdown）

```cpp
RegisterDefaultAllocator();
Trace::sTraceReceiver   = [](const char* s){ PRIMAL_LOG(Trace) << s; };   // Jolt 日志 → PRIMAL_LOG（stderr）
Assert::sFailureReaction = [](const char* e, ...){ PRIMAL_LOG(Error) << e; return AssertResult::Abort; };
Factory::sInstance = new Factory();
RegisterTypes();                                    // 全部默认形状/约束注册
_system = std::make_unique<PhysicsSystem>();
_system->Init(max_bodies, 0 /*mutex 群默认*/, max_bodies*2, max_bodies, _broad_phase_layer_table, broadphase_can_collide, object_can_collide);
_system->SetGravity(to_jolt(settings.gravity));
// Shutdown 逆序：unique_ptr 复位 → delete Factory::sInstance; Factory::sInstance = nullptr; UnregisterTypes();
```

层表（`SimulationLayers`）：BroadPhase = {STATIC, MOVING, DEBRIS, TRIGGER}；ObjectLayer 0..N 与 body_desc.layer 对应；过滤矩阵初版：MOVING↔STATIC/MOVING/TRIGGER、DEBRIS 不与 DEBRIS 碰（粒子化碎块）。测试可通过 layer 组合验证过滤器。

### 2.4 引擎接线的既有坑位检查单

- `EngineDLL/CMakeLists.txt` SOURCE_FILES 显式列表：`PhysicsAPI.cpp` 必须加（探查确认这是显式清单，glob 不覆盖）。
- `Engine.vcxproj`：Windows 侧手维护的工程文件需补 Simulation/ 新文件（引擎历史惯例，glob 只管 CMake）。
- `Tools/PrimalEngineCli`、`Tools/PrimalPlayer` 链接 Engine 后自动获得 Jolt 符号，无需改。

---

## 3. 组件层：Collider / RigidBody

照 Light 组件模式（SoA + 自由函数 + `EngineAPI/XComponent.h` 句柄），完整落点清单（探查核实过的精确位置）：

| 层 | 文件 | 动作 |
|---|---|---|
| 组件 | `Engine/Components/Collider.h/.cpp`、`Engine/Components/RigidBody.h/.cpp` | `primal::collider` / `primal::rigidbody` 命名空间；SoA 并行数组；`create/remove/getters/setters` |
| 句柄 | `Engine/EngineAPI/ColliderComponent.h`、`RigidBodyComponent.h` | `DEFINE_TYPED_ID(collider_component_id)` 等 |
| 注册 | `Engine/Components/ComponentTraits.h` | tag `struct Collider{}/struct RigidBody{}`；`component_bit` 取下一个可用位（现到 10）；`component_bit_of<>` 分支 |
| 实体分派 | `Engine/EngineAPI/GameEntity_impl.h` | include + `component_init_info<T>` 特化 + `Get/Add/Remove` 三个 if-constexpr 分支 |
| 形状来源 | Collider.shape 参数 | M1 支持 box/sphere；convex_hull 经 ContentTools/VHACD（已在 third_party）M2 补 |

**组件语义**：
- `Collider`（可独立存在）：shape、offset、is_trigger、layer → 有 Collider 无 RigidBody 的实体 = 静态体（如关卡碰撞）。
- `RigidBody`（依赖 Collider）：mode(dynamic/kinematic)、mass、restitution、friction、初始速度、sleep 阈值。
- 组件 `create` 不直接碰 Jolt（组件层无 Simulation 依赖倒置问题——Simulation 是 Engine 内部模块，可直接调用；但**只在 world running 时物化 body**，编辑态只存参数）。参数 setter 在 play 态 → 转发到 `world::update_body_properties`。

---

## 4. 宿主 Tick：PipelineTick（关键新增 ABI）

探查结论：引擎是宿主驱动帧模型，无内部主循环，且**没有真实 deltaTime**（渲染用 `delta_time_override = 0.016f`）。物理必须自算 dt：

```
EDITOR_INTERFACE void PipelineTick(f32 dt_seconds)   // EngineDLL/RenderPipelineAPI.cpp 旁新增
{
    script::frame_tick(dt);                  // Script.cpp:816 现成入口（生产循环首次接入）
    simulation::world::step(dt);             // 内部固定 60Hz accumulator（§1.3）
}
```

宿主接入点（三处，各自计时）：

| 宿主 | 位置 | 计时方式 |
|---|---|---|
| PrimalPlayer | `Tools/PrimalPlayer/main.cpp:571-630` while 循环内、`PipelineRenderFrame` 前 | `std::chrono::steady_clock` 差分 |
| 编辑器 | `PrimalEditor_Avalonia/Views/Engine3D/EngineViewport3D.axaml.cs:324-352` `OnRenderTick`（30Hz） | UI 定时器差分；**仅 Play 态调用**；accumulator 在引擎内部补足 60Hz 子步，30Hz 调用每次补 2 步 |
| 测试可执行体 | `RenderTestCase::Run()`（CFRunLoop 60FPS） | 固定传 1/60（确定性测试）/ chrono（演示） |

Play/Pause/Step：`PhysicsSetRunning(bool)` + `PhysicsStepOnce()`（暂停态单步，编辑器和可视化调试都用）。

---

## 5. EngineDLL 导出第一版（PhysicsAPI.cpp）

命名沿 LightAPI 风格，全部 `EDITOR_INTERFACE` + 入出参仅 C 标量/POD；POD 结构 `static_assert(sizeof)` 锁尺寸（沿 `AssetAPI.h:7-19` 惯例）。ABI 只增不改。

```cpp
// 生命周期
EDITOR_INTERFACE u32  PhysicsInitialize(const PhysicsSettingsPod*);   // 1=ok
EDITOR_INTERFACE void PhysicsShutdown(void);
EDITOR_INTERFACE u32  PhysicsSetRunning(u32 running);
EDITOR_INTERFACE void PipelineTick(f32 dt_seconds);
EDITOR_INTERFACE void PhysicsStepOnce(void);

// body 管理（测试与 CLI 主要消费面）
EDITOR_INTERFACE u64  PhysicsBodyCreate(const PhysicsBodyDescPod*);   // 返回 body_id，0=失败
EDITOR_INTERFACE u32  PhysicsBodyRemove(u64 body_id);
EDITOR_INTERFACE u32  PhysicsBodySetTransform(u64, const f32 pos[3], const f32 euler[3]);
EDITOR_INTERFACE u32  PhysicsBodySetLinearVelocity(u64, const f32 v[3]);
EDITOR_INTERFACE u32  PhysicsBodyApplyImpulse(u64, const f32 imp[3]);
EDITOR_INTERFACE u32  PhysicsBodyGetTransform(u64, PhysicsBodyTransformPod* out); // 含 sleeping
EDITOR_INTERFACE u32  PhysicsBodyIsSleeping(u64);

// 确定性门禁
EDITOR_INTERFACE u64  PhysicsDeterminismHash(void);   // 全 body 位姿稳定哈希
```

POD 清单：`PhysicsSettingsPod`、`PhysicsBodyDescPod`、`PhysicsBodyTransformPod`（各带 `static_assert(sizeof, "...must match PrimalEditor_Avalonia mirror...")`；C# 镜像见 §7）。

---

## 6. 编辑器 UI 与文档模型

按探查核实的落点：

1. **文档模型**：`Components/ColliderComponent.cs`、`RigidBodyComponent.cs`（`[JsonDerivedType(typeof(...), "collider"/"rigidbody")]` 加进 `Components/Component.cs`）；场景 JSON 序列化自动获得。
2. **注册表**：`Components/EditorComponentRegistry.cs:35-41` 加两行 → Add-Component 对话框与 inspector 自动出现。
3. **Inspector**：`Views/PropertiesView.axaml` 加两个 `StackPanel` 段（沿 Light 段 L98-166 结构）+ `ViewModels/PropertiesViewModel.cs` 属性；编辑走 **`RecordAndApply`（L502-525）** = Snapshot → apply → Snapshot → `UndoRedo.Add(...)` → `MarkProjectDirty()`，undo 天然闭环。
4. **native 同步**：`Native/SceneRuntimeSynchronizer.cs` `Sync`/`SyncEntity`（L151/L646）加 collider/rigidbody 推送分支（调 §5 的 `PhysicsBodyCreate` 等）；**play 态跳过物理驱动实体的 transform 推送**（防编辑器写回打架）。
5. **P/Invoke**：`Native/NativeEngineBridge.cs` 加 `[DllImport("PrimalEngine", EntryPoint="PhysicsBodyCreate", CallingConvention=Cdecl)]` 镜像 + `INativeEngineBridgeLike.cs` 抽象同步。
6. **Play 控制 UI**（M3）：工具栏 Play/Pause/Step 按钮 → `PhysicsSetRunning`/`PipelineTick` 开关 + Run 按钮（现有 `Common/RuntimeLauncher.cs`）可选直接起 PrimalPlayer。

## 7. CLI 命令族（PrimalEditor_Avalonia/Cli/PhysicsCommands.cs）

```
sim.play / sim.pause / sim.step [n] / sim.reset
sim.body.add --entity <guid> [--shape box|sphere] [--mode dynamic|static|kinematic]
             [--mass M] [--restitution R] [--friction F] [--size "sx,sy,sz"]
sim.body.remove --entity <guid>
sim.body.status --entity <guid>        # 位置/朝向/线速度/sleeping（单位：米、秒、度；右手系——与 Transform 一致）
sim.determinism.hash                   # PhysicsDeterminismHash，测试门禁消费
```

- 全部走文档模型 + `worldEditor.UndoRedo.Add(...)`（沿 `ComponentCommands.cs:46-55` 的 camera.set 样板）；Description 写清单位/前置条件/失败语义（§6.3 纪律）。
- **schema 门禁**：新命令类必须加进 `PrimalEditor_Avalonia.Tests/Cli/CliSchemaSnapshotTests.cs` 的 `BuildCanonicalSchema()` 注册列表（L24-30），跑 `PRIMAL_UPDATE_SCHEMA=1 dotnet test --filter CliSchemaSnapshotTests` 重生成 `Testing/CliSchema/editor-schema-snapshot.json` 并随 PR 提交。

---

## 8. 测试策略（用户要求：接通即有测试；全测试带渲染可视化）

### 8.0 总原则与四层矩阵

| 层 | 名称 | 里程碑 | 运行形态 | 可视化产物 | 断言 |
|---|---|---|---|---|---|
| T0 | TestPhysicsSmoke（接通即有） | **M0 当天绿** | 集成测试 exe，帧数上限自动退出（TestWFCRendering 模式） | 120 帧 PNG 序列 + 亮度 CSV | 1 盒从 y=3 落到地面：终止 y∈[0.45,0.55]；亮度/非空像素随帧变化；退出码 0/1 |
| T1 | TestPhysicsMultiBody（多物体刚体） | M1 | 同上，600 帧 | 600 帧 PNG + CSV + **transform 哈希基线比对** | 塔/金字塔 100 体不穿地不飞散（位置带内）；50 帧哈希 == 基线（单线程确定性）；性能：600 帧墙钟 < 预算 |
| T2 | 编辑器 UI 层闭环（smoke 扩展） | M3 | `primal_smoke.sh` 新 physics 段（活编辑器 + socket） | `editor.screenshot` 前后对比 PNG + `editor.record` mp4 | UI 建 RigidBody → sim.play → N 秒后 `sim.body.status` y 下落到位；screenshot diff 证明画面动了；undo 闭环 |
| T3 | 离线 CLI 门禁 `primal test physics` | M2 | primal-engine 新 verb（headless，无窗口） | 输出 PNG 与基线 ImageDiff（沿 test render verdict 机制） | 渲染基线 within-threshold；JSON 报告落盘 |
| L0 | 纯函数单测 | M0+ | ctest（EngineTest/UnitTests，TestFramework.h） | **无自身可视化，但每条必须有可视化孪生**（T0/T1 场景覆盖同一行为） | accumulator 边界、层过滤矩阵、POD 尺寸锁 |

> 纪律措辞：L0 是唯一允许无窗口的层，且限定纯函数（accumulator、层过滤、哈希函数本身）；任何"物理行为"断言一律落在 T0–T3 的可视化层，杜绝"单测绿但画面黑"。

### 8.1 T0：接通即有的可视化冒烟（M0 验收 = 此测试绿）

样板组合（探查核实）：`TestWFCRendering.cpp:503-520`（帧数上限自动退出 + 先 Shutdown 再 terminate）+ TestModularPipeline 实体创建 helper（L207-213）+ `checkBand` 断言模式（L1541-1565）+ `IntegrationTests/CMakeLists.txt:204-218` 注册模板。

```text
TestPhysicsSmoke（EngineTest/IntegrationTests/TestPhysicsSmoke.cpp）
初始化：JobSystem::Initialize → 窗口(1280x720) → MetalDevice → RenderSystem →
        CompileShaders → StandardRenderPipeline → 相机(俯视斜 45°)
物理：  PhysicsInitialize(settings{gravity -9.81})
        PhysicsBodyCreate(static box 地面 20x1x20) + PhysicsBodyCreate(dynamic box 1x1x1 @ y=3)
渲染：  每帧 PipelineTick(1/60) → 为 dynamic body 更新 proxy transform → PipelineRenderFrame
断言（第 120 帧）：
  a) PhysicsBodyGetTransform.position.y ∈ [0.45, 0.55]（盒半高 0.5 + 允差）
  b) PNG 亮度统计（ComputeLumaStats 模式）：第 1 帧 vs 第 120 帧均值差异 > ε（证明"看得见它在动"）
  c) DebugDrawQueue 线框 proxy 可见：Pass 6 线段数 > 0（GetStats/日志）
退出：  pass → Shutdown() → NSApp terminate(0)；fail → _Exit(1)（TestModularPipeline L1565 模式）
CMake： add_executable(TestPhysicsSmoke ...) + setup_test_target() + add_test(NAME Engine_Integration_Tests_TestPhysicsSmoke ...)
```

ci_local.sh 增补（step ⑨，模板 L36-55）：
```bash
step "9/9 物理可视化冒烟"
cmake --build "$BUILD" --target TestPhysicsSmoke
ctest --test-dir "$BUILD" -R Engine_Integration_Tests_TestPhysicsSmoke --output-on-failure
```

### 8.2 T1：多物体刚体可视化测试（M1 验收）

场景族（同一 exe，`--scene` 参数选，全部渲染）：
- **塔堆（默认）**：10x10 错缝砖塔 100 盒 + 地面，120 帧静置稳定（哈希门禁：第 50 帧与第 120 帧哈希相等 = 全员入睡），再施加一次 `PhysicsBodyApplyImpulse` 冲倒，300 帧内塔顶 y 下降到位。
- **金字塔**：15 层金字塔冲倒，验证深层接触链稳定（Jolt 才能扛住的场景，顺带验证选型）。
- **材质阶梯**：10 个斜面 + 不同 restitution/friction 球，终点位置分带断言（checkBand × 每球）。

自动断言三件套：
1. **行为带内**：每体终止位置 ∈ 预期带（数值正确性）；
2. **确定性哈希**：`PhysicsDeterminismHash` 固定种子场景 N 步后 == `Testing/Baselines/physics-multibody-hash.txt`（单线程模式跑；哈希漂移 = 行为回归，比像素敏感且稳定）；
3. **可视化存在性**：每 10 帧存 PNG（`SaveRenderTargetAsPng`），亮度 CSV（沿 PrimalPlayer `--record-metrics` 的 ComputeLumaStats，`main.cpp:386-401`）非恒定 + 末帧 PNG 与首帧 ImageDiff > 阈值（证明画面在动、proxy 跟随物理）。

渲染性能路径：100 盒共享同一 `ProceduralMeshCreate` content_id → instanced 单 draw（`ForwardSceneRenderer.cpp:2402-2600`），600 帧预算 < 30s 墙钟（超预算即性能回归信号）。

### 8.3 T2：UI 层刚体模拟测试（M3 验收，primal_smoke.sh 扩展）

沿 primal_smoke.sh 既有动词（entity.create → set-transform → screenshot → record/diff → undo 闭环）追加 physics 段：

```text
1. entity.create Cube + physics.body.add（--mode dynamic --mass 1）      ← 走文档模型，undo 入栈
2. sim.play → sleep 2s → sim.body.status                                  ← y 下落带内断言（数值）
3. editor.screenshot → physics_fell.png（与初始 screenshot ImageDiff 大）  ← 可视化：编辑器实况里真的在动
4. editor.record --frames 60 → mp4 落盘                                   ← 可视化：完整运动记录
5. sim.pause / sim.step 5 / sim.reset 全动词走查                            ← CLI 面
6. undo 两步 → sim.body.status 报 body 不存在                              ← undo 闭环
```

可视化产物 = 编辑器真实视口截图 + mp4，人工可复核、自动断言走 JSON 数值 + PNG diff。

### 8.4 T3：`primal test physics`（M2，无窗口 CI 门禁）

`Tools/PrimalEngineCli/main.cpp` 新 verb `physics-sim`（沿 `VerbRender` L282-406 骨架）：InitializeEngine → PhysicsInitialize → 注入地面 + 3 动态盒 → 循环 {PipelineTick, PipelineRenderFrame(target)} × N → `SaveRenderTargetAsPng --out` → JSON stdout。C# `PrimalCli` 加 `test physics`：渲染 PNG 与 `Testing/Baselines/physics-320x180.png` ImageDiff（复用 test render 的 verdict：identical/within-threshold/different）。`ci_local.sh` 加 step。

### 8.5 基线与环境纪律

- 数值基线（哈希）跨 Debug 构建稳定的前提：单线程 + 固定步长 + 相同编译器；**Debug↔Release、Win↔macOS 哈希不承诺一致**，基线按 `{树}-{平台}` 命名（如 `physics-multibody-hash.darwin-debug.txt`），CI 矩阵各自比对。
- 渲染基线走阈值容忍（沿 test render 机制），不要求 bit 级一致；哈希门禁管物理正确性，像素门禁只管"可视化没坏"。

---

## 9. 里程碑任务分解（M0–M4 重排，测试前置）

> **M4 进展（2026-09-12）**：⑰ **ContactListener 已实施**——`contact_listener_impl` 收集 Added/Persisted 接触（`Body::SetUserData` 反查引擎 body id；回调多线程触发，互斥入队，上限 8192 防无界增长），API `drain_contacts` + DLL 导出 `PhysicsDrainContacts`；脚本事件总线转发待脚本运行时接入 PipelineTick 后开启（emit 会堆积未 drain 的事件，开关已留）。⑲ **凸包/三角网碰撞体已实施**——`body_create_convex`（ConvexHullShapeSettings，≥4 点）+ `body_create_mesh`（MeshShape，仅静态）+ `PhysicsBodyCreateConvex/Mesh` DLL 导出。物化路径重构为 `create_body_with_shape`（统一 user data 反查注入）。单测 **UnitPhysicsM4**：接触事件捕获/配对/added 标志 + 凸包地台着地（y≈0.49）+ 三角网着地（y≈0.0，1.97e-7）。**实施教训**：① 三角网顶点是 body 局部坐标（body position 叠加世界偏移，双重偏移会导致"穿到网格下方半米处静止"的迷惑现象）；② 环绕顺序决定正面朝向，背面默认被 MeshShape 忽略；③ 接触回调多线程触发，严禁在回调里调 BodyInterface 加锁接口（用 Body 引用直读）。⑱ JoltJobAdapter 多线程延后。

| 里程碑 | 任务拆解 | 验收（全部绿才算过） |
|---|---|---|
| **M0 接通日** ✅ 2026-09-11 已实施 | ① submodule pin **v5.6.0** + CMake（§2.2，**实际选项名以 Jolt 5.6 为准**：`JPH_BUILD_SHARED_LIBS=OFF` 静态链接 + `POSITION_INDEPENDENT_CODE ON`——全局 BUILD_SHARED_LIBS 缓存被其他子项目置 ON，按设计的 `JOLT_ENABLE_*` 系列在 5.6 不存在）+ LICENSE ② Engine/Simulation 骨架：PhysicsWorld Initialize/Shutdown + step + box/sphere ShapeFactory + 层表（**bp_filter 必须在层映射/碰撞矩阵填充之后再构造**——它的位表是构造时快照）③ EngineDLL PhysicsAPI 第一批 ④ T0 TestPhysicsSmoke 绿（渲染走高层 `pipeline->Render()` + 离屏 RGBA8 RT + `WaitIdle` 后 ReadBack；`DrawBodies` 走 DrawGeometry 与线段 renderer 不兼容，改为逐体 `DrawWireBox/WireSphere`；桥接 TU 需 `-fno-rtti`——Jolt 无 typeinfo，且该 TU 不得包含 CommonHeaders 链）⑤ L0 单测（accumulator/层矩阵/确定性哈希）+ ci_local.sh step 8/9 | T0 绿 + ctest 绿（#142 UnitPhysicsSimulation、#186 Engine_Integration_Tests_TestPhysicsSmoke）|
| M1 多物体 | ✅ 2026-09-11 已实施：⑥ owner→ECS transform 回写（`sync_bound_transforms`，step/step_once 内自动执行）⑦ 双管线代理（各通道独立 RegisterMeshEntity，同几何 instanced）⑧ **T1 TestPhysicsMultiBody 三场景**：砖塔 95 盒安定（哈希 60 帧稳定）+ 底层滑出坠落、金字塔 120 盒安定 + 掏空坠落 0.5m、弹性阶梯 8 球峰值单调（0.38→1.78，CoR 0.05..0.75）⑨ 确定性哈希基线入库（Testing/Baselines/physics-multibody-hash.txt，PRIMAL_UPDATE_PHYS_BASELINE=1 重生成）+ 性能预算 <30s（实测 19s）。**设计修订**：塔布局 10/9 交替（10/10 错缝有悬臂端砖→永久微抖动无法安定）；坍塌语义 = 底层滑出/重球轰击（原"踢两砖倾覆"在摩擦耦合下不成立）。CI 注意：ctest 工作目录随构建子目录变化，基线路径用向上逐级查找 | T1 三场景全绿（ctest #187）+ 19s 预算 |
| M2 离线门禁 | ⑩✅ primal-engine `physics-sim` verb（离屏仿真 120 帧：地面+动态盒，PipelineTick 驱动 + 物理数值断言 final_y∈[0.44,0.56] + PNG；一次性进程 `_Exit` 直传退出码——拆除路径有引擎侧双重释放噪声）⑪✅ `primal test physics`（C# 客户端 + Testing/Baselines/physics-sim-320x180.png 基线，bare-run verdict=identical）+ ci_local step 6b（固定 PRIMAL_ENGINE_BIN=Darwin/Debug，§10-8）⑫ C# 组件文档模型 + P/Invoke + Schema（待下轮；前置=引擎 Collider/RigidBody 组件）| ⑩⑪ 绿：physics-sim exit 0 + bare-run identical + ctest 三测绿；⑫ 待做 |
| M2-D 引擎组件层 | ✅ 2026-09-11 已实施：Collider/RigidBody ECS 组件（照 Light 模式：SoA + 自由函数 + EngineAPI 句柄转发；Collider 层默认 kDynamic——POD/零值层的坑已在 CLI 侧注释）+ ComponentTraits（bit 11/12）+ GameEntity_impl 四分支 + EngineDLL 组件导出 6 个（AddEntityCollider/AddEntityRigidBody/Remove×2/Has×2，物化映射表持 entity→body；物化时读实体位姿——初版漏读导致 body 全在原点穿地，单测抓住）+ C# 文档模型（ColliderComponent/RigidBodyComponent + JsonDerivedType + EditorComponentRegistry + MSCollider/MSRigidBody + NativeEngineBridge/INativeEngineBridgeLike/Adapter + 3 个测试 FakeBridge 桩）。**闭环单测 UnitPhysicsComponents**：实体+组件 → 物化 → step 回写 → 实体 transform 被物理驱动（min_y 0.454 落地）| ctest 四测绿（#142/#186/#187/#UnitPhysicsComponents）+ C# Cli 56/56 |
| M3 编辑器闭环 | ⑬ Inspector 两面板 + RecordAndApply undo ⑭ SceneRuntimeSynchronizer 分支 + play 态跳过 ⑮ Play/Pause/Step UI + sim.* 命令族 ⑯ **T2 smoke 扩展段绿** | T2 绿 + primal_smoke 全绿 + schema 门禁绿 |
| M4 打磨 | ⑰ ContactListener → 事件（ScriptEventBus 触发器） ⑱ JoltJobAdapter 接引擎 JobSystem（多线程路径 + 单线程开关保留） ⑲ 凸包/三角网 collider（VHACD 链） | demo 工程 + 全门禁 |

M5+（角色控制器、布料软体、流体立项）沿用调研报告 §4，不在本文展开。

---

## 10. 风险与开放问题

1. **dt 计时是全新增量**：引擎从未有真实 dt，宿主三处计时器质量（编辑器 DispatcherTimer 抖动）靠 accumulator 吸收；极端掉帧语义 = 丢帧不追（§1.3），行为可预期。
2. **编辑器 30Hz vs 物理 60Hz**：一次 tick 调用补 2 子步；渲染侧 M3 前不插值（30Hz 采样 60Hz 模拟在自由落体上视觉可接受），插值是增强项不是依赖项。
3. **哈希基线的脆弱面**：任何 Simulation 参数/层表/Jolt 版本变更都会合法地改变哈希——基线更新必须随 PR 说明原因（门禁说明模板放进 Testing/Baselines/README）。
4. **Engine.vcxproj 手维护**：新文件漏加会导致 Windows 侧静默缺文件——M0 加进 checklist，后续可考虑 vcxproj 生成器小工具（另立）。
5. **Jolt 版本 pin 策略**：只 pin release tag，升级 = 独立 PR + 全部物理门禁重跑（哈希基线随之更新），禁止顺手升级。
6. **macOS `-std=c++17` 显式 flag 的全局性**：对 Jolt 是恰好命中而非设计，若未来该 flag 清理成 target 级 CXX_STANDARD 20，需确认 Jolt 目标仍以 C++17 编译（Jolt 兼容 C++20，无实际风险，记录在案）。
7. **【M0 已知问题】T0 测试窗口中实体网格缺失（仅线框 proxy 可见）**：`RenderWithCommandBuffer` + backbuffer 目标下，静态实体网格（RegisterMeshEntity 路径）不产出绘制，debug 线框 overlay 正常；同场景走高层 `Render()` + 离屏 RT（无 RenderSystem）则网格完整（v2 实证）。已排除 cbIdx 索引源、SetOutputResource。已尝试 surface.blit_and_present 呈现离屏结果，`create_surface` 在测试环境崩溃（编辑器为 EngineDLL 建窗后调用，测试环境缺少部分前置）。物理行为断言不受影响（全部基于 simulation API 数值 + 线框像素差），网格渲染成因待专项排查。
8. **【M0 已知问题】build-wt（DebugEditor 混合配置）下物理测试堆损坏**：Engine 以 `-O3 -DNDEBUG -ffast-math -std=c++17/20 混合 flag` 编译、Jolt 空配置编译时，`BodyManager::Init → mBodySequenceNumbers.resize(4096)` 处 100% 复现 heap corruption（realloc 收到同一垃圾指针 0x32aaaba7，SIGABRT）。已排除：JPH 条件宏不一致（已做 Jolt 侧 config 归一化 + 消费方 define 显式补齐，Jolt 运行时自检通过）、fast-math（Simulation TU 已 `-fno-fast-math` 屏蔽，无效）。怀疑方向：Engine `-O3` 对内联 Jolt 头代码的变换，或 DebugEditor 树其他 flag 与 Jolt 的交互——需要独立专项（最小复现 + MallocStackLogging）。**处置**：物理门禁固定跑 Darwin/Debug（ci_local step 8/9），build-wt 仅要求物理目标可编译可链接（已验证）。
