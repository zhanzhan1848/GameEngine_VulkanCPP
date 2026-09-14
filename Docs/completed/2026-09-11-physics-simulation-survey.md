# 模拟（物理）模块调研报告 —— 刚体/软体/布料第三方库选型

- 日期：2026-09-11
- 状态：调研结论稿（作为正式立项依据）
- 范围：刚体物理 + 碰撞检测 + 软体/布料（第三方库）、流体（自研，本报告仅给路线预览）、其他模拟子系统（优先级最后，不在本报告展开）
- 前置决策（已确认）：
  1. 在现有框架之上开发（组件模型、EngineDLL、编辑器文档模型、CLI 全部复用）；
  2. 刚体/软体/布料用第三方库，流体自研；
  3. **现阶段必须保持 C++17**；
  4. 原倾向 PhysX，接受"更现代、更接近 AI"的替代方案。

---

## 1. 仓库现状盘点（选型约束来自哪里）

### 1.1 C++ 标准的实际情况

| 构建 | 声明 | 实际生效 |
|---|---|---|
| Windows | `CMAKE_CXX_STANDARD 20`（`Engine/CMakeLists.txt:9`） | C++20 |
| macOS/Apple | 同上声明，但被显式 flag 覆盖 | **C++17**（`Engine/CMakeLists.txt:384` `-stdlib=libc++ -std=c++17`） |

跨平台交集 = **C++17**。这就是"现阶段必须保持 C++17"的工程依据：任何第三方库的核心库必须在 C++17 下可编译。等 macOS 侧切回 C++20 后该约束自然解除，但选型不应赌这个时间点。

### 1.2 已有资产

- **`third_party/PhysX` = PhysX 4.1.2**（官方 depot 完整 vendor，含 Kapla demo；`physx/version.txt` = 4.1.2.29882248）。**未接入 CMake**——根 `CMakeLists.txt:164-169` 是 TODO 注释（"使用 git submodule 集成"）。git 历史（b06c8296）显示曾有预编译 PhysX 库、后被清理。
- **`third_party/` 依赖惯例**：vendor 目录 + `LICENSE-THIRD-PARTY.md` 登记；stb 已走 git submodule 先例。
- **组件模型**（`Engine/Components/`）：按类型的组件模块（transform/geometry/light/script…），`id::id_type` 句柄 + `init_info` 初始化 + 引用式资源所有权（见 `primal::cluster` 注释），经 `EngineAPI/GameEntity.h` 门面访问。**物理组件应沿用同一模式**。
- **引擎侧可复用设施**：`Engine/JobSystem/`（work-stealing 调度器）、`Engine/TaskScheduler/`——Jolt 的 job 系统可直接对接。
- **内容侧可复用**：ContentTools cook 管线（mesh 数据、VHACD 已在 `third_party/VHACD`）——convex decomposition / triangle mesh 数据源现成。
- **下游接线纪律**（AGENTS.md）：EngineDLL 导出只增不改 + POD `static_assert` 锁尺寸；编辑器修改类命令走 Scene/GameEntity + UndoRedo 文档模型；CLI 命令 = `[CliCommand]` + schema 快照门禁。
- **`Engine/Simulation/`**：空目录（仅 .DS_Store），无任何历史代码。

---

## 2. 候选库对比

### 2.1 总表（2026-09 检索核实）

| 维度 | **Jolt 5.6** | **PhysX 5.x（≤5.10）** | **PhysX 4.1.2（已 vendor）** | **Bullet 3.x** | **MuJoCo 3.x** |
|---|---|---|---|---|---|
| 许可 | MIT | BSD-3 | BSD-3 | zlib | Apache-2.0 |
| C++ 标准 | **C++17**（无 RTTI、无异常、仅 STL） | C++17 可用 | C++14 | C++17（接口偏旧） | C 库 |
| macOS（Apple Silicon） | **官方支持**（CI 含 macos.arm64；PS5/Switch 同为 ARM64 内核） | 无官方支持（官方平台仅 Win/Linux；CPU 移植属社区行为） | 官方 macOS readme 为 Intel 时代（2021），**ARM64 无官方支持** | 社区可用 | 官方支持（CPU） |
| GPU 加速 | 无（纯 CPU，多线程 scaling 好） | 有，但 **CUDA-only**（Win/Linux；本引擎 Metal/MoltenVK 用不上） | GPU 线程已废弃方向 | 无 | 无（MJX/JAX 属另一生态） |
| 软体/布料 | XPBD 软体已可用（布料 demo、软体 contact listener、5.6 加 hair）；集成侧仍标注 experimental，适合简单布料 | 最强：PBD 布料、FEM 软体、粒子、Flow（但 GPU 侧 CUDA-only，CPU 侧软体能力弱） | 无（4.1 无软体路线） | 经典 soft body，老化 | flex/绳索，机器人向 |
| 维护活跃度 | 非常活跃（作者 Jorrit Rouwe，Guerrilla；2026 初发 5.6） | 活跃（NVIDIA Omniverse 仓库） | **停更**（2021 后冻结，Unreal 也已转向 Chaos） | 停滞 | 活跃（机器人向） |
| 游戏业界采用 | Godot 4.4 内置、**Godot 4.6 起为新项目默认**；Horizon 系列 | UE（旧）、Unity（旧）、Isaac Sim | UE4 旧版 | 老项目 | 非游戏向 |

### 2.2 逐项评估

**Jolt Physics（推荐）**
- 硬约束全部命中：C++17、macOS ARM64 官方支持、MIT 与 `LICENSE-THIRD-PARTY.md` 惯例兼容、无 RTTI/异常（与引擎 ABI 边界干净）。
- 刚体/碰撞质量是当前开源第一梯队（PS5《Horizon Forbidden West》出品方自用并开源）；确定性模式（deterministic cross-platform）可选，利于回归测试。
- job 系统可接自定义 scheduler → 可对接本引擎 JobSystem，避免线程池 doubling。
- 软体/布料：核心已落地（XPBD），适合"简单布料、绳网、果冻体"；复杂布料（多层碰撞、自接触）仍属 experimental——放后期里程碑并控制预期。
- 风险：无 GPU 加速（对本引擎当前阶段无影响：CPU 管线先行）；无 iOS/web 官方目标（Emscripten 社区有，非本次范围）。

**PhysX 5.x（不推荐作为引擎内核）**
- 三个硬伤叠加：①macOS 无官方支持（本仓库日常开发在 Apple Silicon macOS）；②GPU 加速 = CUDA-only，与 Metal/Vulkan(MoltenVK) 后端体系完全错位；③2025 年开源的 GPU 源码（500+ CUDA kernel）不解决前两条。
- CPU-only 路径理论可行但需自行移植 + 维护非官方构建，长期成本高。

**PhysX 4.1.2（已 vendor，不推荐在其上新建）**
- 死分支：2021 年后冻结，无软体/现代 API 路线；Apple Silicon 无官方支持。
- 处置建议：**从 `third_party/` 移除或归档**（约数百 MB），README 层面保留决议记录；若未来需要与 Isaac/旧 UE 资产互操作再议。

**Bullet 3.x / MuJoCo（否决）**
- Bullet：维护停滞，性能与 API 现代化落后一代。
- MuJoCo：接触动力学与可微性优秀，但定位机器人/RL（few-body、精确接触），游戏场景的大量动态刚体 + 场景查询非其目标；仅作为 RL 训练侧备选记录。

### 2.3 "更接近 AI"的方向怎么落

核实结论（2026-09）：
- **Newton 1.0**（NVIDIA，与 DeepMind/Disney Research 合作，已入 Linux Foundation）：GPU + 可微 + sim-to-real，但构建于 **Warp（Python GPU kernel 框架）**之上。
- **Warp 在 macOS 仅有 CPU 执行，无 Metal 后端**（官方文档明示），且整体是 Python 生态。
- Genesis 等同类均为 Python-first。

→ 结论：**AI 邻近方案当前无法作为 C++17 游戏引擎的进程内内核**。可行落点是**离线/训练侧互操作**：
1. 场景导出为 USD（Newton/Isaac Sim 生态），引擎做"编辑器 + 运行时"，训练在 Newton 侧做——两个世界数据互通；
2. 引擎内流体自研时参考可微/物理校准方法论（不影响引擎内保持传统数值方法）；
3. 长期若 Warp 出现 Metal 后端或 C API，再评估进程内集成的可能性（记录为开放问题，不阻塞立项）。

### 2.4 流体自研路线预览（后续里程碑，本次不展开）

- 算法候选：FLIP/PIC（液体，质量守恒好）> SPH（实现直观但调参敏感）> Position-Based Fluids（游戏化、稳定、可上 GPU）。
- 执行载体：走自家 RHI compute（Vulkan/Metal 双后端已有 compute 管线经验，Lumen SPGI/DDGI 即先例），与渲染深度耦合恰恰是自研的理由。
- 建议：刚体闭环（M0-M4）完成后再立项，先出独立设计文档。

---

## 3. 结论与推荐

> **推荐：Jolt Physics 5.6+ 作为 Engine/Simulation 的物理内核**，覆盖刚体、碰撞、关节、载具、简单布料/软体；PhysX 4.1 vendor 归档移除；AI 侧以 USD 互操作衔接 Newton/Isaac 生态；流体后续自研立项。

决策理由浓缩为一句话：在"C++17 + macOS(ARM64)/Windows 双平台 + 引擎内嵌 + 软体布料要能用"的四重约束下，Jolt 是唯一全部命中的活跃选项，且它同时是 Godot 4.6 的默认 3D 物理引擎，生态验证充分。

**备选路线（如未来约束变化）**：
- 若放弃 macOS 官方支持约束 → PhysX 5 CPU-only 可重新进入候选（移植成本另评）；
- 若引擎整体升 C++20 且出现 Metal 后端的 GPU 物理库 → 重开 GPU 物理评估。

---

## 4. 集成架构草案（Jolt 假设下）

分层与既有纪律一一对应：

```
third_party/JoltPhysics        ← git submodule（沿 stb 惯例）+ LICENSE 登记
Engine/Simulation/             ← Jolt 封装层（编译隔离，Jolt 头文件不出此层）
  PhysicsWorld / PhysicsScene  ← 场景、固定步长 accumulator（60Hz）+ 渲染插值
  ColliderShapes               ← Box/Sphere/Capsule/ConvexHull(triangle mesh from ContentTools/VHACD)
Engine/Components/             ← Transform 联动 + Collider/RigidBody 组件（沿 primal::cluster 模式：id + init_info + 引用式资源）
EngineDLL/                     ← 新增导出（只增不改；POD static_assert 锁尺寸，沿 AssetAPI.h 惯例）
PrimalEditor_Avalonia          ← 组件 UI 走 Scene/GameEntity 文档模型 + UndoRedo（与 UI 同路径）
Primal.CliFramework / Tools    ← sim.* 命令（[CliCommand]，description 写清单位/坐标系/失败语义）+ PRIMAL_UPDATE_SCHEMA=1 重生成快照
EngineTest/                    ← ctest 单测（积分/碰撞/确定性，无 GPU 依赖）
```

要点：
1. **步长**：固定 60Hz 物理 + accumulator；渲染 transform 插值（编辑器暂停/单步与 CLI 采样天然友好）。
2. **线程**：Jolt JobSystem 适配到本引擎 JobSystem；单线程模式作为调试基线。
3. **确定性**：启用 Jolt 跨平台确定性模式 + 固定步长 → 可写回放回归测试（对门禁文化是加分项）。
4. **坐标/单位**：与引擎现有约定对齐并在 CLI description 中显式声明（米、右手系——以引擎 Transform 既有定义为准）。

### 里程碑草案

| 里程碑 | 内容 | 验收 |
|---|---|---|
| M0 | submodule 接入 + CMake 双树构建（Debug/DebugVulkan 共享 libEngine.a，注意切树清 lib 的既有坑） | ctest Simulation 单测绿 |
| M1 | 刚体内核：PhysicsWorld 固定步长、静态/动态 Box/Sphere、Transform 联动 | TestModularPipeline 加物理场景日志回归 |
| M2 | 组件层：Collider/RigidBody 组件 + EngineDLL 导出 + static_assert | C# schema 快照门禁 + 单测 |
| M3 | 编辑器：组件 UI + UndoRedo + gizmo 联动 | primal_smoke.sh 闭环 |
| M4 | CLI：sim 命令族 + demo 工程（堆叠/斜面/反弹） | primal test render 式门禁 |
| M5 | 关节/角色控制器（Jolt CharacterVirtual） | demo 工程 |
| M6 | 布料/软体（Jolt SoftBody，控制预期：简单布料） | demo + 性能基线 |
| M7 | 流体立项（独立设计文档） | 评审 |

---

## 5. 风险与开放问题

1. **Jolt 软体成熟度**：简单布料可用，复杂布料（自接触、多层）experimental——M6 范围要收窄，复杂布料可能需要后续专项（或届时评估 PhysX 5 专项接入）。
2. **macOS C++17 vs 引擎声明 C++20**：Jolt 双兼容，无阻塞；但 `Engine/CMakeLists.txt:384` 的显式 flag 覆盖本身值得清理（另立小任务，不属于物理模块）。
3. **32 位平台**：Jolt 无 32 位构建——本引擎现状无 32 位目标，无影响；若未来移动端 32 位需求出现需重新评估（记录在案）。
4. **双构建树混链坑**（AGENTS.md 已记录）：Jolt 加入 libEngine.a 对象集后，切树不清 lib 的症状会叠加物理符号，M0 起就把"切树清理"写进 CI 脚本。
5. **AI 互操作深度**：USD 导出粒度（刚体层级/材质参数映射）需要与 Newton/Isaac 侧实际验证后才能定——放 M5+ 的调研小项，不阻塞主线。
6. **PhysX 4.1 归档**：删除 `third_party/PhysX` 是不可逆动作，执行前需单独立项确认（本报告仅给建议，不执行）。

---

## 6. 参考资料

- Jolt Physics GitHub（C++17/无 RTTI/无异常/MIT）：<https://github.com/jrouwe/JoltPhysics>
- Jolt 构建文档：<https://jrouwe.github.io/JoltPhysics/md__build_2_r_e_a_d_m_e.html>
- Jolt soft body 跟踪 issue #390：<https://github.com/jrouwe/JoltPhysics/issues/390>
- Godot 4.5 Jolt 官方文档（4.4 内置、4.6 起新项目默认）：<https://docs.godotengine.org/en/4.5/tutorials/physics/using_jolt_physics.html>
- NVIDIA-Omniverse/PhysX（5.x，BSD-3，GPU=CUDA Win/Linux）：<https://github.com/NVIDIA-Omniverse/PhysX>
- Phoronix：PhysX 5.1 开源与 2025 GPU 源码开放：<https://www.phoronix.com/news/NVIDIA-PhysX-51-Open-Source>
- NVIDIA Newton（Warp 构建、Linux Foundation 治理、1.0）：<https://developer.nvidia.com/newton-physics> 、<https://github.com/newton-physics/newton>
- Warp 安装文档（macOS 仅 CPU、无 Metal 加速）：<https://nvidia.github.io/warp/stable/basics/installation.html>
- 本仓库物理暂缓决策的出处：`Docs/completed/2026-09-03-editor-content-creation-and-four-features-plan.md` §3/§4（"物理……按题材与需求再议"）
