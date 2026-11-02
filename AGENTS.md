# AGENTS.md

给进入本仓库的编码 agent 的工作指南：怎么跑起来、CLI 在哪、怎么验证改动。

## 仓库速览

- **引擎核心**（C++20）：`Engine/`；**C ABI 门面**：`EngineDLL/`（约 320 个导出）。
- **编辑器**（Avalonia/C#）：`PrimalEditor_Avalonia/`；测试：`PrimalEditor_Avalonia.Tests/`。
- **CLI / Agent 接口**：规划见 `Docs/CLI_Agent_Interface_Plan.md`。
  - `Primal.CliFramework/` — 命令框架（attribute + 反射 Registry + socket 分发）
  - `PrimalCli/` — `primal` 客户端（stdout=JSON，stderr=日志，退出码 0/2/3/4/5）
  - `Tools/PrimalEngineCli/` — `primal-engine` 离线执行器（render/cook-mesh/schema）
  - `Tools/primal-mcp/` — MCP 桥（stdio，任意 MCP harness 零适配接入）
  - `Tools/PrimalBuild/` — `primal-build` 场景/项目离线打包（dotnet tool，断链路线 A）
  - `PrimalEditor_Avalonia/Cli/` — 编辑器内命令宿主（CliHost + 各命令类）

## 构建

```bash
# C++（Engine + EngineDLL + primal-engine）
cmake -S . -B Darwin/Debug -DCMAKE_BUILD_TYPE=Debug && cmake --build Darwin/Debug -j8
# Vulkan 后端（TestVulkanSponzaRenderGraph 等）需要独立目录：
cmake -S . -B Darwin/DebugVulkan -DCMAKE_BUILD_TYPE=Debug -DENABLE_VULKAN=ON
# 注意：两棵构建树共用 Darwin/Debug/libEngine.a，交叉构建会混入对方对象集
# （症状：GI 收敛变黑/链接符号缺失）。切换树后先 rm Darwin/Debug/libEngine.a 再构建。

# C#（CLI 框架 + 编辑器 + 测试；DebugEditor 是开发配置）
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor
```

注意：Xcode 15+ 的 SDK sysroot 不再默认搜 `/usr/local/include`，Vulkan（MoltenVK）
头文件路径已在 `EngineTest/IntegrationTests/CMakeLists.txt` 显式补上。

## 运行

```bash
# 编辑器（PRIMAL_OPEN_PROJECT 跳过项目浏览器，CI 冒烟必需）
PRIMAL_OPEN_PROJECT=$PWD/projects_test/TestProject_01/TestProject_01.primal \
  Darwin/PrimalEditor_Avalonia/DebugEditor/PrimalEditor_Avalonia &

# CLI 客户端（另开终端）
./scripts/primal.sh editor.ping        # 连通性；stdout JSON，退出码 0-5
./scripts/primal.sh scene.tree
```

## 怎么验证你的改动（不要跳过）

```bash
# 1. C# 单测（含 CLI schema 快照门禁——改了命令面必须 PRIMAL_UPDATE_SCHEMA=1 重生成快照并随 PR 提交）
dotnet test PrimalEditor_Avalonia.Tests -c DebugEditor --filter "FullyQualifiedName~Cli"

# 2. 客户端自测 + 编辑器闭环冒烟（需编辑器在跑）
Darwin/PrimalCli/DebugEditor/primal selftest
./scripts/primal_smoke.sh

# 3. 离线通道 + 基线门禁（无需编辑器）
dotnet run --project Tools/PrimalBuild -c DebugEditor -- <project.primal>  # 离线打包
Darwin/DebugEditor/primal-engine engine info
Darwin/PrimalCli/DebugEditor/primal test render

# 4. 本地全门禁（等价被裁掉的 macOS CI 作业）
./scripts/ci_local.sh

# 5. 引擎回归（改了 Engine/ 下的东西必做）
ctest --test-dir Darwin/Debug -R "Unit" --output-on-failure     # 单测（无 GPU 依赖）
# 集成测试是窗口式的：运行 60-120s 后 kill，检查日志无 fault/异常退出
Darwin/Debug/TestModularPipeline 2>&1 | tee /tmp/regress_metal.log
Darwin/DebugVulkan/TestVulkanSponzaRenderGraph 2>&1 | tee /tmp/regress_vulkan.log
```

## 纪律（改代码前读）

- **命令面单一事实源**：新增 CLI 命令 = 一个方法 + 一个 `[CliCommand]` attribute，
  description 必须写清单位/坐标系/前置条件/失败语义（§6.3），并同步重生成 schema 快照。
- **文档模型优先**：编辑器通道的修改类命令走 Scene/GameEntity + UndoRedo 栈
  （与 UI 同路径），绝不裸调 EngineDLL 制造第二事实源。
- **ABI 纪律**：EngineDLL 导出只增不改；POD 镜像用 `static_assert(sizeof)` 锁尺寸
  （沿 `AssetAPI.h` / `EngineEventsAPI.h` 惯例）。
- **日志纪律**：新 C++ 代码用 `PRIMAL_LOG`/`primal::diag::Emit`，日志走 stderr，
  stdout 留给机器输出。
- **渲染/RHI 必读**：动 `Engine/Graphics/`（RHI、渲染图、任一后端）前先查
  `Docs/RHI/GraphicsAPI_Parity_Matrix.md`（Vulkan/Metal/D3D12/Dawn 四 API 对等矩阵，
  注意 Dawn 端的降级边界）和 `Docs/rhi-barrier-semantics.md`（三端同步语义）。
- **文档与知识库**：`Docs/` 整体不进入 Git；本地正式文档索引仍为 `Docs/ARCHIVE.md`。
  引擎说明、架构、功能、规划、技术路线及正式结论保存到 [Gemini Notebook 引擎知识库](https://notebook.google.com/notebook/b30f044b-6a42-4f6b-bd0e-47ff3bda9864)。
  更新正式文档后同步知识库，保留原路径、日期与实现/候选/失败边界；同步后核验来源可读。
  会话交接、临时实验、逐轮诊断、原始证据、CI/审查流水报告不进入知识库；过程文件仅保留最近 72 小时。
  `Docs/reports/` 定义为中间过程目录。阶段收束先将设计、实现契约和长期结论提炼到 `Docs/` 根目录专题文档，
  维护源文件→主题的覆盖清单，再上传正式专题并读回核验；不直接上传 reports 原文。
  入口见 `Docs/Rendering_Knowledge_Index.md`。未完成知识提炼的 Markdown 不得仅按目录、文件名或年龄删除。
  清理按最后修改/状态变更时间中较晚者计算，保留近期或正在使用的输入及构建对象；删除前记录范围、SHA 与实际释放空间。
  正式文档不按三天期限删除。迁移清单和同步收据保留于本地 `Docs/.knowledge-base/`。
- **PFX/粒子排障速查**：PFX 相关"改了没作用/行为突变/穿地/无法保存"类问题，
  先查 `Docs/2026-09-16-pfx-troubleshooting-root-causes.md`（根因+现行规则）。
  高频结论：预览推送只进私有预览发射器，场景发射器只经 Save→Resync 更新；
  PhysicsCollision.radius 是"缩放 1 时的半径"（内置几何基准 0.05）且需 SIM ▶ Play；
  新增编辑器组件必须登记 Serializer.cs 三处，否则保存抛 Unknown component type；
  同 guid scene.load 不重建原生实体（强制重建需跨场景切换）。

## 渲染诊断中间数据的磁盘纪律

- 每轮开始记录本轮创建的临时目录；实验归档并逐 SHA 核验成功后，清理本轮已归档的 raw/bin 副本，保留 capture、日志、失败摘要及重建命令。不要等到 session 交接才清理。
- 跨阶段相同 SHA 的原始数据只保留一个物理副本；可用硬链接保留各归档逻辑路径。归档视为只读，不原地改写硬链接文件；需更新时写新文件再原子替换。
- 完整逐帧大窗口仅在证明跨帧来源所必需时捕获；不默认重复导出未变化的全 atlas／几何。减少诊断导出不应减少实际渲染射线、更新或改变验证契约。
- 不删除其他正在运行会话的近期数据、未归档输入、生产资产或构建对象。清理须保留范围清单、SHA对应和释放空间实测。

- 临时诊断 PNG 在持久截图或阶段归档中已有逐 SHA 相同副本后也应及时清理；保留 capture.json、重建网页所需 frame.ppm、日志和失败摘要，不盲删未归档或近期图片。
