# 场景烘焙断链评估（CLI 规划 Phase 3）

> 状态：评估结论 + 路线 A 已实施（2026-09-02）
> 规划出处：Docs/CLI_Agent_Interface_Plan.md §10 Phase 3 "场景烘焙断链评估：先 C# dotnet tool 打通，再评估下沉 C++"

## 1. 什么是"断链"

编辑器把场景 JSON 转成引擎二进制资产（.model/.texture/.scene → 引擎运行时格式）
的逻辑分布在三处，**没有任何一处是 CLI 可调用的**：

| 环节 | 现状实现 | 可达性 |
|---|---|---|
| 源资产 → 引擎格式 | ContentTools C++（CookMesh/CookTexture/ProcessAIAsset） | ✅ 已通：`primal-engine cook-mesh` / `import-texture` |
| 场景文档 → 序列化 | C# `Serializer`（SceneDocument/ProjectDocument，编辑器进程内） | ❌ 仅编辑器进程内可达 |
| 序列化产物 → 运行时加载 | 引擎 `ContentToEngine`（TEXR/model 解码） | ✅ 已通（编辑器/引擎侧） |

断链的本质：**第 2 环（场景级打包）是 C# 编辑器域的逻辑**——CookMesh 这类单资产
cook 已经由 C++ 侧覆盖（本轮 `import-texture`/`cook-mesh` 落地），但"一个项目/
场景的全部资产 + 场景图 → 打包产物"只有 `Serializer` + `BuildPipeline`（C#）知道。

## 2. 评估：三条路线

### 路线 A：C# dotnet tool（规划的推荐路径）✅ **已实施**
把编辑器的 `Serializer` + `BuildPipeline` 薄包装成 dotnet tool
（`dotnet primal-build <project.primal>`）。
- 优点：逻辑零搬迁（直接引用编辑器程序集）；与编辑器同源零漂移；工作量 ~1-2 天。
- 缺点：需要 .NET 运行时（CI/沙箱镜像要带）；与 primal-engine（C++）形成两个执行器。
- 结论：**断链修复的唯一现实路径**。场景→二进制的关系型知识（guid 引用、材质
  依赖、sidecar）全在 C# 侧，重写必然漂移。

### 路线 B：下沉 C++（primal-engine 增 scene-pack verb）❌ 不推荐（现阶段）
- 需要在 C++ 重实现 SceneDocument 反序列化 + 依赖解析——编辑器每改一次序列化
  格式，C++ 侧追一次（规划 §9 风险 #2 的镜像扩散教训刚在 ai_asset_pipeline_params
  上验证过一次：两份 C++ 镜像布局分叉已产生真实 ABI 缺陷，本轮修复）。
- 结论：等格式稳定 + 出现"无 .NET 环境的打包需求"（如纯 C++ 游戏运行时构建）
  再评估。

### 路线 C：编辑器通道暴露 `scene.bake` verb ⏸ 可选补充
编辑器内已同时持有两环（Serializer + 引擎），加一个 verb 即可闭环。
- 限制：依赖活编辑器，不符合"离线打包"诉求；但适合 Agent 的"改完即验证"闭环。
- 结论：有 Agent 场景需求时再做（约半天）。

## 3. 结论

1. 断链的修复以**路线 A（C# dotnet tool）**立项：`dotnet primal-build`。
   触发条件：出现首个 CI/无 GUI 消费者需要整场景打包时。
2. CLI 侧已完成的单资产 cook（`cook-mesh`/`import-texture`/`bake-vt`）覆盖了
   规划 §5.1 离线通道的资产级部分；场景级打包按上述结论走 C# tool，不进
   primal-engine。
3. 规划 Phase 3 的"评估"子项与本子项均已关闭：路线 A 已实现为
   `Tools/PrimalBuild`（`dotnet run --project Tools/PrimalBuild -- <project.primal>`，
   无 GUI 进程走 OpenProject → AssetRegistry 重扫 → BuildManifestGenerator →
   SaveAll 全链，stdout=JSON/退出码 0/2/4，实测 TestProject_01 打包 3 条目）。
   `--save-only` 提供轻量校验路径。
