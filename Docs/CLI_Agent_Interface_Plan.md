# 引擎 CLI / Agent Interface 综合规划

> 状态：设计定稿（待评审）
> 分支基线：`feat/avalonia-migration`
> 日期：2026-08-31

---

## 1. 目标与定位

本规划要解决的不是"给引擎加一个命令行程序"，而是构建引擎的**统一 Agent Interface**：

| 消费者 | 需求 | 入口 |
|---|---|---|
| 人（编辑者） | 可视化操作 | Avalonia GUI（现状保留） |
| 脚本 / CI/CD | 离线批处理、流水线、无 GUI 环境 | one-shot CLI |
| LLM Agent | **遥控运行中的编辑器**：编辑场景、人物动作、场景状态，改完即可视化 | 会话式 CLI / MCP |
| 自动化测试 | 确定性验证、截图对比、退出码 | 同上一条复用 |

一句话定位：**GUI 面向人操作，CLI 面向系统与 Agent 操作，两者共享同一套命令与状态。**

核心诉求场景（最高优先级）：用户打开编辑器 UI，LLM 通过命令通道对活编辑器做编辑
（场景实体、变换、灯光、动画、材质参数），每步可截图验证，构成
**看 → 想 → 改 → 验证** 的自动编辑闭环；同时人看到的界面与 LLM 改的是同一份状态。

---

## 2. 现状盘点（已有基础 vs 缺口）

### 2.1 已有基础（可直接复用）

| 资产 | 位置 | 意义 |
|---|---|---|
| C ABI 门面，约 311 个导出（生命周期/资产/渲染/烘焙/截图） | `EngineDLL/*.cpp`，20 个 API 文件 | 一切命令的 C++ 执行底座，无需改引擎内部 |
| headless 渲染整链已验证 | feat 分支 `Program.cs` `--headless-render` + `EngineDLL/HeadlessRender.cpp`（`SaveRenderTargetAsPng`/`FlushGPU`） | 离线 `render` verb 蓝本；`SaveRenderTargetAsPng` 是 Agent 的"眼睛" |
| 独立 CLI 先例 ×2 | `ContentTools/main.cpp`（--pipeline/--validate）、`ContentTools/vtbake/main.cpp` | verb 设计与退出码样板 |
| 资产批处理脚本 | `scripts/vt/bake_sponza.sh`（vtbake 批量烘 75 张） | pipeline 编排可行性已证 |
| C# 文档模型 | feat 分支 `GameProject/Documents/SceneDocument|EntityDocument|ProjectDocument`、`Serializer` | **在线编辑命令的正确落点**（UI 绑定的真实数据源） |
| P/Invoke 桥 | `Native/NativeEngineBridge.cs`（97 个声明）+ `NativeLibrary.Load` 多名回退 | C# → 引擎通道现成 |
| 测试基建 | ctest + `run_all_tests` + CI workflow | `test` verb 薄包装即可 |
| ABI 纪律范例 | `EngineDLL/AssetAPI.h` 的 `static_assert(sizeof==792)` | 扩展导出时的正确姿势 |

### 2.2 缺口（本规划要补的）

1. **无任何进程间通信设施**：LLM 无法触达运行中的编辑器（引擎状态活在编辑器进程内）。
2. **无统一命令框架**：现有 CLI 手写解析；`ai_asset_pipeline_params` POD 已有三份手工镜像。
3. **场景烘焙断链**：编辑器 JSON → 引擎二进制的转换逻辑只在 C# 侧（`BuildPipeline`/`BuildManifestGenerator`）。
4. **平台容错**：Windows MSVC `CreateRenderSurface(nullptr)` 是 assert（clang 分支已容错）；Linux 无 surface。
5. **输出契约缺失**：无 `--json`、退出码约定、schema 自省。
6. **环境耦合**：shader blob 按 cwd 相对解析（headless 必须 cwd 锚定）。

---

## 3. 总体架构

### 3.1 双通道形态（最重要的架构判断）

CLI 不是单一形态，必须按状态归属拆成两条通道：

```
┌─ Avalonia 编辑器进程（在线通道，核心场景）──────────────────┐
│  Avalonia UI ══ SceneDocument / EntityDocument              │
│        ║（UI 与 CLI 走同一套 handler）                       │
│  CommandServer（[CliCommand] Registry，监听本地 socket）     │
│        └── handler → NativeEngineBridge ──▶ EngineDLL ──▶ Engine │
└──────────────┬───────────────────────────────────────────────┘
               │ Unix domain socket / named pipe（JSON 消息，传输无关设计）
       ┌───────┴────────┐
       │ primal 客户端   │◀── LLM / 脚本 / CI 冒烟
       │ primal-mcp 桥  │◀── MCP harness（ZCode / Claude 等）
       └────────────────┘

EngineCore (C++) ── EngineDLL ──▶ 离线通道（one-shot 子进程）
      cook / bake-vt / bake-gi / build-shaders / render --headless
      无状态、落盘产物、CI/沙箱/容器友好
```

- **离线动词**（产物落盘，如 cook/bake）→ 子进程即可，天然可沙箱；
- **在线动词**（操作活编辑器，如 entity/scene/material/animation）→ **必须**走 socket
  进编辑器进程。`subprocess.run` 模式在这里只会启动一个没加载任何场景的新引擎。

类比：`docker` CLI 无状态，但一切有状态操作发给常驻 dockerd——本设计中
**编辑器进程就是 dockerd**。

### 3.2 逻辑命令命名空间（跨进程聚合）

统一 Registry 的理想图横跨两个进程（离线实现在 C++、在线实现在 C#），物理统一不现实。
替代方案：**一个逻辑命名空间、两个执行器、一张聚合 schema**。

- `primal schema` 输出合并命令表，每条标注 `channel: offline | editor`；
- CLI 客户端与 MCP server 按标注路由到子进程或 socket；
- CI 门禁对聚合 schema 做快照 diff，接口变更在 PR 中强制可见；
- LLM 看到的是统一接口面，无需感知背后进程结构。

### 3.3 分层总览

```
EngineCore (C++) ── EngineDLL (C ABI，已有)
   │
   ├── 离线执行器（C++ 侧，one-shot）
   └── Avalonia 编辑器进程
         └── CommandServer（[CliCommand] Registry + socket）
               ├── Avalonia UI（人，同一套 handler）
               ├── primal CLI 客户端（脚本 / 冒烟）
               ├── primal-mcp 桥（agent，双通道路由）
               └── workflow.run（确定性流水线）

知识层：SKILL.md / AGENTS.md / docs.search /（后置）embedding RAG
```

**明确砍掉的层**：REST / gRPC / Web Dashboard——无当前需求，属过度设计。
替代约束：socket 消息模型做成**传输无关**（JSON-RPC 风格），未来加 Web 只需
HTTP→socket 适配器，今日成本为零。

---

## 4. 命令框架设计（C# Attribute 封装）

C# 的 Attribute + 反射就是"装饰器"惯用法，实现零手工注册的 Registry。

### 4.1 声明形态

```csharp
public class EntityCommands
{
    [CliCommand("entity.set-transform",
        Description = "设置实体世界变换。单位米，右手系，+Y 向上。实体必须已存在，" +
                      "否则返回 NOT_FOUND。修改后视口立即刷新。",
        Example = """{"entityGuid":"...","position":[1,0,3]}""")]
    public TransformResult SetTransform(
        Guid entityGuid,                     // 自动 JSON 绑定，缺参报结构化错误
        Vec3 position,
        Vec3? rotationEuler = null)          // 可选参数 → schema non-required
    {
        var doc = _scene.RequireEntity(entityGuid);   // 抛 CliError("NOT_FOUND", "先 scene.tree 查询")
        doc.Transform = ...;
        _engine.SyncTransform(entityGuid, ...);       // P/Invoke
        return new TransformResult(doc.Transform);    // 自动序列化为响应 data
    }
}
```

### 4.2 框架层统一提供（handler 只写业务）

| 能力 | 实现方式 |
|---|---|
| 命令发现 | 启动时一次反射扫描，装配进 Registry |
| 参数绑定 | `System.Text.Json` 按签名反序列化；类型不匹配/缺参 → 结构化错误 |
| JSON Schema 生成 | 从 `ParameterInfo`/返回类型反射生成 → `primal schema` |
| MCP tool 定义 | 同一份 schema 转 MCP `inputSchema` + description |
| 主线程编组 | Dispatcher 统一外包，handler 不感知线程（引擎侧配合 `TickMainThreadJobs`） |
| 错误包装 | 业务抛 `CliError(code, hint)`；未预期异常统一捕获为 `INTERNAL` + 日志引用 |
| 自定义类型 | `Vec3`/`Guid` 等注册一次转换器（接受 `[1,2,3]` 与 `"1,2,3"`，对 LLM 宽进严出） |

### 4.3 硬约束

1. **fail-fast 校验**：启动时扫描命令签名，发现无 schema 规则的自定义类型立即失败——
   否则漂移会以静默方式回来。
2. **程序集拆分**：attribute + 装配器分属 `Primal.CliFramework` 与宿主两个程序集。
   将来若引入 AOT/trimming，运行时反射换编译期 source generator，attribute 与
   handler 代码零改动。
3. **白名单即安全**：只有注册过的 verb 才可被调用，LLM 天然无法触达未暴露的能力；
   只绑本地 socket，无需鉴权。

---

## 5. 命令面设计

### 5.1 命令命名空间（初版清单）

**编辑器通道（editor，最高优先级）**

| 类别 | 命令 | 底层 |
|---|---|---|
| 元 | `editor.ping` `editor.schema` `editor.undo` | Registry / Document 快照 |
| 查询 | `scene.tree` `entity.get` `asset.list` | 文档模型序列化 |
| 观察 | `editor.screenshot --out f.png [--w --h]` | `SaveRenderTargetAsPng`（已有） |
| 帧捕获 | `editor.record --frames N --fps F --out dir/ [--scale 0.5]` | 离屏 RT 异步回读环形缓冲（见 5.4） |
| 帧捕获 | `editor.metrics --frames N` | 每帧标量遥测（均亮度/方差）时间序列 JSON |
| 帧捕获 | `editor.diff a.png b.png --out heatmap.png` | 帧间/基线对比，自动测试共用 |
| 场景 | `scene.load` `scene.save` `scene.new` | `SceneDocument`/`Serializer` |
| 实体 | `entity.create/delete/duplicate` `entity.set-transform` `entity.set-name` | 文档模型 + P/Invoke 同步 |
| 组件 | `light.set` `camera.set` `animation.play/pause/seek` `material.set-param` | 对应 EngineDLL 导出 |
| 执行 | `engine.tick <N>` `engine.flush` | `PipelineRenderFrame` / `FlushGPU` |

**离线通道（offline）**

| 命令 | 底层 |
|---|---|
| `engine info` | `InitializeEngine` 健康检查 + 设备信息 |
| `render` headless 渲染出 PNG | 复刻 `--headless-render` 序列（离屏 RT + surface_id=~0u） |
| `asset import-texture / import-mesh / cook / validate` | AssetAPI + ContentToolsCLI 能力聚合 |
| `bake vt / bake gi` | vtbake / `GIBakeStaticProbes` |
| `build-shaders` | ShaderCompilerAPI |
| `workflow run <yaml>` | 见 5.3 |
| `schema`（聚合输出两张表） | CI 门禁用 |

### 5.2 命令面设计三原则

1. **每个动作动词配一个查询动词**：只写不读的 CLI 无法构成"改 → 看 → 再改"的闭环。
2. **查询输出必须可解析**：一律 JSON，禁散文。
3. **UI 按钮 = 对自家命令的内部调用**（长期形态）：CLI 面与 UI 面永不漂移；短期至少
   要求新功能注册时两处都挂。

### 5.3 帧捕获：截图是帧流的退化特例

一帧截图只能验证静态构图；仓库实际调试过的问题（VSM blur 不生效、SC 图集过曝、
indirect args 撕裂挂死）全是**多帧才能显现**的。所以接口按帧流设计，拆三层：

```
第 1 层  帧序列捕获（record）  editor.record --frames 120 --fps 30 --out out/
第 2 层  视频编码             第 1 层帧序列的下游编码（mp4/gif/关键帧）
第 3 层  标量遥测（metrics）   每帧均亮度等标量，成本最低
```

设计要点：

1. **第 1 层是根本**：扩展已有离屏 RT + `SaveRenderTargetAsPng` 为连续 tick + 逐帧
   异步回读写盘。配合 `engine.tick` 确定性推进，`record --frames 120` **可复现**——
   对 bug 报告和 CI 价值极大。
2. **视频（第 2 层）服务于"人可视化"目标**；对 agent 的产物不是 mp4 而是
   **采样关键帧 + 相邻帧 diff 热图**（把"哪里在变"直接可视化）。编码初期不引重依赖：
   帧序列写盘后调外部 ffmpeg（可选依赖，缺则退化为 PNG 序列/GIF）；macOS 后续可换
   VideoToolbox。编码是 `editor.record` 的输出选项，不是独立能力。
3. **遥测（第 3 层）是 agent 排查多帧问题的首选入口**：每帧一个标量的时间序列即可
   抓住闪烁/过曝/渐黑（过曝问题 mean 179 当时就靠回读统计定位），成本比图像回读低
   几个量级；看不出异常再上帧序列。
4. **socket 传指针，不传像素**：帧数据走文件系统（`--out` 路径），JSON 消息只带
   元数据（帧数/尺寸/路径/遥测数组）。实时直播流（MJPEG/WebSocket）若将来需要，
   是独立的新 verb 和新通道，不混入命令消息协议。
5. **回读不卡渲染主循环**：异步回读环形缓冲（读 N-2 帧时引擎继续跑 N 帧），允许
   降采样（诊断 10–15 fps/半分辨率；回归测试全分辨率无损）。

### 5.4 Workflow（YAML 流水线）

定位：**确定性流水线的打包**（import → optimize → meshlet → bake → package，步骤固定），
不是 agent 交互回路的替代——探索性编辑（布光、调动画）必须逐步看截图，YAML 化会丢闭环。

- `workflow.run` 是 Registry 里的**普通命令**：同一执行器、每步发结构化事件、
  失败带步骤级错误与已完成部分的状态（stop-on-error 语义）；
- agent 可自由选择"单命令交互"或"生成 YAML 一次跑"。

---

## 6. 协议与输出契约

### 6.1 消息协议（传输无关）

```json
→ {"id":"42","verb":"entity.set-transform","params":{...}}
← {"id":"42","ok":true,"data":{...}}
← {"id":"42","ok":false,"error":{"code":"NOT_FOUND","message":"...","hint":"先用 scene.tree 查询有效 guid"}}
```

- 帧格式：长度前缀；socket 文件 `~/.primal/editor-<pid>.sock`（或项目目录），编辑器退出清理。
- 响应附队列时延信息（UI 阻塞时命令排队，对 agent 判断有用）。

### 6.2 输出纪律（Unix 可组合的前提）

- `--json` 机器输出走 **stdout**，日志一律走 **stderr**；
- 退出码分类表：`0` 成功 / `2` 参数错 / `3` 资源不存在 / `4` 引擎内部错 / `5` 编辑器未运行；
- `primal schema` 输出带 `$version`（Registry 程序集版本或内容 hash），MCP/CI 缓存一致性检查；
- 错误必须含 `code + message + hint（下一步建议）`——LLM 靠错误文本自我修正，
  "NOT_FOUND: 先用 scene.tree 查询"这类写法能省一轮试错。

### 6.3 描述写作规范（attribute 元数据标准）

每个 `[CliCommand]` 的 description 必须写清：单位/坐标系/前置条件/失败语义/副作用；
每个 verb 至少一个 example。**内容质量是决定 LLM 好不好用的关键变量，封装解决机制，
这部分是人工投入，也是验收对象**（见 9.4 实测法）。

---

## 7. LLM 接入层

### 7.1 三层产物，单一事实源

```
[CliCommand] Registry（唯一事实源）
   ├─▶ primal schema          自省命令，任何 harness 运行时拉取
   ├─▶ MCP tools              primal-mcp 桥自动生成 tool 定义
   └─▶ SKILL.md 命令参考      可生成初稿，人工补工作流
```

原则：给 LLM 的说明**绝不手写第二份**，全部从 Registry 生成，接口演化时自动同步。

### 7.2 MCP server（primal-mcp）

- 形态：独立薄进程，stdio（MCP 传输）↔ socket（编辑器命令）双向转发；
- tool 定义从 `editor.schema` 拉取生成，带 `$version` 缓存校验；
- **双通道路由**：按 schema 里的 `channel` 标注，offline verb 走子进程、
  editor verb 转发 socket；编辑器未运行时 tool 返回明确错误（exit code 5 语义）；
- 任何支持 MCP 的 harness（ZCode/Claude/Cursor 等）接入零适配。

### 7.3 Skills / AGENTS.md

工具 schema 只告诉 LLM"有什么"，SKILL.md 教它"怎么打"。首批三个 playbook：

1. **场景布光**：`scene.tree` 摸底 → 逐灯 `light.set` → `engine.tick 30` →
   `editor.screenshot` → 迭代；
2. **资产导入摆放**：`asset import` → `entity.create` → `set-transform` → 截图验证；
3. **回归验证**：改后截图与基线对比 → 退出码约定。

另在仓库根放 **AGENTS.md**：引擎怎么启动、CLI 在哪、怎么验证改动——对任何进入
工程的编码 agent 直接生效，成本远低于 RAG。

### 7.4 RAG：分层引入，不单独建通道

| 层 | 成本 | 触发条件 |
|---|---|---|
| 结构化 schema 即检索 | 零 | 命令面 ≤ 几十 verb 时无需向量检索 |
| `docs.search` 命令（ripgrep 级全文检索 Docs/、MEMORY.md、源码注释） | 低 | Phase 3，作为普通 verb 注册 |
| embedding 检索（`asset.semantic-search`：.meta 标签/描述语义搜索） | 高 | 资产规模真正需要时再立项 |

**RAG 是命令面上的一个 verb，不是独立 LLM 通道。**

---

## 8. 自动测试搭在上面

自动测试 = 普通脚本：

```
加载场景 → engine.tick N → editor.screenshot → 与基线图 diff（复用
scripts/visual_parity_report.sh 机制）→ 不符退出码非零
```

与 LLM 自动编辑共用同一命令面与"眼睛"（截图），即"可视化、自动测试、自动编辑"
三目标同一套设施。CI 冒烟：编辑器以最小场景启动 → `editor.ping` + `scene.tree` +
`screenshot` → 退出码判定。

---

## 9. 关键工程点与风险

| # | 问题 | 对策 |
|---|---|---|
| 1 | **线程编组**（最易踩坑）：socket 收命令在后台线程，引擎/UI 只能动主线程 | Dispatcher.UIThread.InvokeAsync 投递主线程执行再回填；框架统一外包 |
| 2 | **ABI 镜像扩散**：POD 已三份手工镜像 | 按 `AssetAPI.h` static_assert 惯例抽公共头；禁止加第四份镜像 |
| 3 | **shader blob cwd 耦合** | CLI/MCP 启动即 cwd 锚定可执行目录（复刻 `AnchorWorkingDirectoryToExecutable`），或提供 `--engine-root` |
| 4 | **描述质量无法静态验收** | 实测法：让真实 LLM 只靠 schema + SKILL.md 操作编辑器，哪里卡住改哪里 |
| 5 | **Windows MSVC null-surface assert** | 离线 `render` verb 前对齐 clang 分支容错 |
| 6 | **Linux 无 surface** | CI matrix 按平台禁用 render 类 verb，资产类全平台可用 |
| 7 | **AOT/trimming 反射裁剪** | 现状普通 WinExe 直接可用；CliFramework 独立程序集预留 source-gen 迁移 |
| 8 | **编辑器未运行** | 客户端明确报错（exit 5），MCP tool 同语义 |
| 9 | **GPU 回读同步 stalls**：`RenderTexture::ReadBack` 是否有异步路径待核实 | 帧捕获用异步回读环形缓冲；Phase 2 开工前先核实，无则补引擎侧能力 |

---

## 10. 分期路线

### Phase 0 —— 最小闭环（框架即地基）
- `Primal.CliFramework`（attribute + 反射装配 + schema 生成 + socket 分发）
- **日志 L0（前置依赖，见第 11 节）**：LogDispatch + Console/RingBuffer sink +
  EngineDLL `InstallLogCallback` 等三件套——CLI 的"日志走 stderr"纪律从第一天就是真的
- `CommandServer` 进编辑器 + `primal` 客户端
- 种子命令：`editor.ping` / `scene.tree` / `editor.screenshot` / `editor.schema`
- **交付判据：LLM 已能"看见"运行中的编辑器**（截图 + 场景查询）

### Phase 1 —— 编辑 verbs（核心价值）
- 实体增删改、变换、灯光、动画 play/seek、材质参数
- 全部走 `SceneDocument` 层（UI 同步刷新）
- undo（Document 快照）+ 命令原子性（校验全过才落）

### Phase 2 —— 离线通道 + 自动测试 + 帧捕获
- C++ 侧 `primal-engine` one-shot CLI：render / asset / bake / build-shaders
- `primal schema` 聚合两张表 + CI 快照 diff 门禁
- `test.run`：截图基线对比 + 退出码约定，接入 CI
- 帧捕获三层（时域回归测试直接依赖，与自动测试同期交付）：
  `editor.record`（先核实异步回读路径）/ `editor.metrics` / `editor.diff`；
  视频编码（ffmpeg 可选依赖）作为 record 的输出选项

### Phase 2.5 —— Agent 接入（原 Phase 3 前移）
- `primal-mcp` 桥（双通道路由）
- 首份 SKILL.md（三个 playbook）+ 仓库 AGENTS.md
- 在自有 harness（ZCode）中实测 LLM 驱动闭环，修订描述质量

### Phase 3 —— 打磨
- `workflow.run` YAML 流水线
- `docs.search`；`--json`/补全/版本收尾
- 场景烘焙断链评估：先 C# dotnet tool 打通，再评估下沉 C++
- embedding RAG（按资产规模立项）

### 优先级原则
若只做一件事，做 Phase 0——在线命令通道对核心目标（LLM 遥控 + 可视化闭环）
价值最大；离线工具面可与之并行、互不阻塞。

---

## 11. 日志与诊断专项（L 系列）

CLI/Agent 面严重依赖日志纪律（stderr 分离、LLM 靠错误 hint 自我修正、CI 日志可收集），
而现状调查结论：**引擎没有跨层统一日志系统**——约 6 套互不相通的机制，全部只在 debug
构建生效，无一能到达宿主。

### 11.1 现状要点（证据）

| 缺口 | 证据 |
|---|---|
| 统一设施形同虚设 | `RHIDebug.h` 宏全 RHI 仅 5 处使用；Engine 下 119 文件约 **1610 处**裸 `cout/cerr` |
| 级别/目标是编译期常量 | `RHIDebug.h:44-54`，默认 target=0 全静默；Release 下 `DEBUG_OP` 为空，**ERROR 都无输出** |
| EngineDLL 零日志导出 | 328 个导出无任何 `InstallLogCallback`/`SetLogLevel`；`InitializeEngine` 失败只返回 0 + stderr 一行 |
| GPU fault 到不了调用者 | Metal fault 仅 `MetalCommandBuffer.cpp:248` 临时 cerr；device lost 无错误码 |
| C++ 与 C# 日志不互通 | WPF/Avalonia Logger 只显示 C# 自己的消息，引擎 stderr 进不了 UI 面板 |
| 无统一落盘/崩溃处理 | 仅 `rhi_debug.log` 硬编码且默认不写；无 crash handler/minidump |

现成挂点：`RHIDebug.cpp:76 OutputDebugMessage`（单点汇聚，带 level/file/line 全参数）、
ContentTools `progress_callback`（函数指针先例）、Vulkan/Dawn validation 回调（单点函数
重定向即可）、Avalonia `ILogger`/`UiLogger.EntryAdded`（C# 侧回调式雏形已就绪）。

### 11.2 设计：核心只发事件，去向由宿主定

**依赖倒置是本专项的地基**（决定"要不要改核心层"的答案）：

```
宿主（编辑器/CLI/脚本）
   │  传 primal_engine_events_v1（第 12 节）
   ▼
EngineDLL（门面层，全部新代码）── 事件表存储 + LogDispatch + 四个 sink 都住这里
   │  只通过一个内部发射函数反向依赖核心
   ▼
Engine 核心 ── 只需知道 primal::diag::Emit(level, category, ...) 这个函数
              （核心完全不知道宿主/事件表/sink 的存在）
```

事件表是 **EngineDLL 的 ABI，不是核心层的**；核心层唯一要认的是一个无主的发射函数
`Emit`——没人装回调时它就是空操作。宿主换台、加 sink、脚本接管，核心零感知；
事件表 v1→v2 演进只动 EngineDLL 侧，核心的 `Emit` 签名可十年不变。

```
各层 → PRIMAL_LOG(Error, "RHI", "shader compile failed: {}", path)   // 唯一入口宏
   ▼ LogDispatch（lock-free 队列，后台线程 flush，线程安全）
   ├── ConsoleSink    → stderr（CLI/headless 默认）
   ├── FileSink       → 统一日志文件 + 轮转
   ├── CallbackSink   → C ABI 回调 → C# ILogger → UI 面板（三层首次打通）
   └── RingBufferSink → 常驻黑匣子（最近 N 千条，Release 也开）
         └── 崩溃/断言时自动 dump 最后一屏 → 附在错误响应与 crash log
```

关键决策：

- **黑匣子常开（最重要）**：Release 下 ERROR 以上仍走 RingBuffer + stderr。历次 Metal
  取证（fence 超时、AtlasInit 全零、indirect args 撕裂）若有"挂死前最后 500 条自动落盘"
  每次都能省一轮插桩重建。
- **结构化字段是给 LLM 的**：category 与命令命名空间对齐（`RHI`/`Asset`/`Scene`/`Render`…），
  事件附帧号/资产 guid；`editor.logs --tail 200 --level warn --category RHI` 比翻全文有效。
- **迁移不人肉**：宏提供 ostream 兼容形态（`primal::log(Error) << ...`），脚本批量替换
  1610 处裸流（行为零变化）；结构化字段化只对关键路径（EngineDLL 错误边界、device lost、
  导入失败）手工做。`RHIDebug` 宏与遗留 `MESSAGE` 宏废弃到兼容层。

### 11.3 三轨道实施（按"是否动核心层"拆分，互不阻塞）

| 轨道 | 动核心层？ | 规模 | 内容 |
|---|---|---|---|
| **A** | 否（EngineDLL/Diagnostics 新代码） | 新文件为主 | 事件表进 `InitializeEngine`、LogDispatch、Console/RingBuffer sink、`InstallLogCallback` 三件套；事件从 EngineDLL 已包住的点发出（初始化失败细节、导入 progress、进程级 crash handler） |
| **B** | 是，但极小 | ~6 文件 | 核心**现成的单点汇聚点**重定向到 `Emit`：`OutputDebugMessage`（RHIDebug.cpp:76）、Vulkan debug messenger（VulkanDevice.cpp:1039）、Dawn logging callback（DawnDevice.cpp:246）、Metal shader/CB 错误两处 cerr。一个小 PR 量级 |
| **C** | 是，但机械 | 1610 处，异步 | 裸 `cout/cerr` 批量替换为 ostream 兼容宏，脚本进行、行为零变化、按模块分批提交；**不阻塞 A/B**（替换前裸流保持 stderr 直出，ConsoleSink 兜进程 stderr） |

### 11.4 L 系列落地节奏（= 三轨道 × P 系列各期）

| 期 | 内容 | 轨道 | 对应 P 期 |
|---|---|---|---|
| **L0** | LogDispatch + ConsoleSink + RingBufferSink + EngineDLL 三件套导出（`InstallLogCallback`/`SetLogLevel`/`SetCategoryFilter`） | A | **Phase 0 同期（前置依赖）** |
| L1 | CallbackSink 接通 C# ILogger → 日志面板；单点回调重定向；device lost 结构化事件 | B + A 收尾 | Phase 1 同期 |
| L2 | 批量迁移裸流（轨道 C）；统一文件落盘 + 轮转；崩溃黑匣子 dump 挂钩 | C | Phase 2 同期 |
| L3 | `editor.logs` / `log-stream` agent verbs；CI 日志收集归档 | 消费端 | Phase 3 |

---

## 12. 宿主事件表（Host Events vtable）

### 12.1 设计：一张版本化的事件表 + 一套命令面

统一回调结构的业界成熟形态是**宿主接口表**（Dawn/Godot GDExtension/Handmade 平台层
同构）：初始化时传入带版本号的 C 结构体，核心只依赖这张表。C 结构体 + 函数指针是唯一
能保证 ABI 长期稳定的方案。精确化为一句架构：

> **事件表住在 EngineDLL（门面 ABI），核心层只依赖 `primal::diag::Emit` 发射函数
> （见 11.2 依赖倒置）；一张版本化的事件表（引擎→宿主，只观察），一套命令面
> （宿主→引擎，只修改），两者互不越界。**

> **一张版本化的事件表（引擎→宿主，只观察），一套命令面（宿主→引擎，只修改），两者互不越界。**

```c
typedef struct primal_engine_events_v1 {
    uint32_t version;      // PRIMAL_EVENTS_V1
    size_t   size;         // 宿主 sizeof，引擎按 size 截断兼容
    void*    userdata;     // 原样传回每个回调
    void (*on_log)(void* ud, uint32_t level, const char* category,
                   const char* msg, const primal_log_fields* fields);
    void (*on_assert)(void* ud, const char* file, int line, const char* cond);
    void (*on_crash)(void* ud, const primal_crash_info* info);      // 黑匣子出口
    void (*on_progress)(void* ud, const char* op, int done, int total);
    void (*on_device_lost)(void* ud, const char* backend, const char* detail);
} primal_engine_events_v1;

PRIMAL_API bool InitializeEngine(primal_rhi_platform p, bool debug,
                                 const primal_engine_events_v1* events);
```

C# 侧 delegate 结构镜像布局（static_assert 锁尺寸，沿 `AssetAPI.h` 纪律）。
编辑器、CLI、脚本宿主（Lua/C#/WASM 薄绑定）**实现同一张表即平级消费者**——
"核心处理脚本化"由此免费获得，无需新通道。

### 12.2 四类需求的边界纪律（此设计成败的关键）

| 需求类 | 处置 | 理由 |
|---|---|---|
| 事件观察（log/assert/crash/progress/遥测） | **进 vtable** | 纯观察无副作用，随时可加 |
| 修改数据的"锚点" | **不进 vtable，走命令面** | 裸改锚点绕过 Document 校验/UI 同步，成为第二事实源 → 状态漂移与跨帧竞争。修改永远走 EngineDLL 导出 + CommandRegistry |
| 插入外部处理（管线钩子） | **逐子系统显式设计**（如 `RegisterRenderPassHook`），作为导出而非事件表成员 | 通用钩子 = 邀请在任意位置插入任意代码，重入与顺序使核心不可推理；等具体需求出现再单独设计 |
| 脚本化 | **脚本是表的消费者，不是新机制** | 不为脚本发明第二条核心出口 |

### 12.3 ABI 纪律

- `version + size` 双字段：加回调只追加字段，旧宿主按 size 截断兼容；
- 字段只追加、永不重排，废弃置 null 保留占位；
- 每个回调文档化**线程契约**：调用线程、可否阻塞、可否重入。日志类 = 任意线程 +
  lock-free 入队；crash 类 = async-signal-safe。

---

## 13. 验收清单（Definition of Done）

- [ ] 用户打开编辑器 UI，LLM 通过 CLI/MCP 完成一次"查询场景 → 修改实体 → 截图验证"闭环
- [ ] 所有命令说明从 Registry 自动生成，`primal schema` 快照进 CI 门禁
- [ ] 支持任意 MCP harness 零适配接入（tool 列表 + 调用 + 结构化结果）
- [ ] 离线 verbs 在无 GUI 的 CI 环境全绿（macOS/Linux/Windows 各自可用子集）
- [ ] 截图基线自动测试进 CI，失败退出码非零
- [ ] `editor.record` 连续 120 帧重跑逐帧一致（确定性验证），`editor.metrics` 能暴露注入的闪烁缺陷
- [ ] 新增一条命令的边际成本 = 一个方法 + 一个 attribute
- [ ] 日志 L0 落地：Release headless 下 ERROR 可达 stderr 与黑匣子；`InstallLogCallback` 可在 C# 收到引擎日志（面板三层打通为 L1 判据）
- [ ] SKILL.md 三 playbook 在真实 LLM 会话中一次跑通
