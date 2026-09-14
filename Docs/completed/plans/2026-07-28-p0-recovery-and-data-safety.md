# PrimalEditor Avalonia — P0 数据安全与可用性修复计划

> **For Claude:** 本计划是 `2026-07-28-avalonia-feature-parity.md` 的 **P0 优先级**补充。
> 既不重复 T1-T10 已经完成的内容，也不替代之。
> 本计划聚焦数据安全基础：**布局正确性 → 死入口清理 → 异步 UI 死锁 → 事务式 Undo/Redo → Dirty/Save → Primitive Mesh 真实落盘**。
> 这些项目在 feature-parity 中**没有覆盖**，是 Avalonia 编辑器成为"可持续开发"基础的最小集。

## 背景与现状（实施前已确认）

- **既有未提交修改**（保留，不重做）：
  - `EngineDLL/AssetAPI.h` (新建) + `EngineDLL/AssetAPI.cpp` (改)：异步 Mesh 导入改为 `request_id + TryXxxResult` 轮询 + 清理。
  - `EngineDLL/CMakeLists.txt`：`AssetAPI.h` 加入 `HEADER_FILES`。
  - `PrimalEditor_Avalonia/Native/AssetImporter.cs`：使用 `WaitForMeshAsync` 轮询。
  - `PrimalEditor_Avalonia/Native/NativeEngineBridge.cs`：`TryGetMeshImportResult` + `TickMainThreadJobs` 包装。
  - `PrimalEditor_Avalonia/Views/MainWindow.axaml.cs`：`OnNewScriptClick` 占位 handler。
  - `tasks/lessons.md`：历史叙述（仅追加 P0 教训，不删改历史）。

- **本计划不依赖上述修改的功能完成度**——本计划只修复 P0 范围内的独立缺陷。如果异步 Mesh 导入修改未编译通过，由该修改的作者处理，与本计划无关。

## 已确认的 P0 缺陷（基于代码阅读）

| # | 缺陷 | 文件:行 |
|---|---|---|
| L1 | WorldEditor 工具栏与主区域重叠（同一 `Grid.Row="0"` 重复） | `PrimalEditor_Avalonia/Views/WorldEditorView.axaml:16,28,90` |
| L2 | F4 (Toggle World Editor) 是死入口（`InputGesture` 但无 `Click` 无 `Command`） | `PrimalEditor_Avalonia/Views/MainWindow.axaml:59` |
| L3 | F5 (Toggle Content Browser) 是死入口 | `PrimalEditor_Avalonia/Views/MainWindow.axaml:60` |
| L4 | AboutDialog 同步 `ShowDialog` 不 `await`（fire-and-forget） | `PrimalEditor_Avalonia/ViewModels/MainWindowViewModel.cs:196` |
| L5 | RenameEntityExecute `result.Wait()` 死锁风险 | `PrimalEditor_Avalonia/ViewModels/MainWindowViewModel.cs:288-289` |
| L6 | OnEntityDoubleClick `result.Wait()` 死锁风险 | `PrimalEditor_Avalonia/Views/WorldEditorView.axaml.cs:154-155` |
| U1 | TransformAction 旧/新值被覆盖为相同值（`RecordUndo` 双循环从已修改的 transform 抓两次） | `PrimalEditor_Avalonia/ViewModels/PropertiesViewModel.cs:88-108` |
| U2 | UndoRedo 不支持事务（一次拖拽 N 个 Action；一次输入 N 个 Action） | `PrimalEditor_Avalonia/Common/UndoRedo.cs:14-65` |
| U3 | AddEntity/DuplicateEntity/RemoveEntity 在批处理时生成 N 个独立 Action | `PrimalEditor_Avalonia/ViewModels/MainWindowViewModel.cs:172-186, 270-306` |
| S1 | 无 `Project.IsDirty` / `Scene.IsDirty` / `Asset.IsDirty` 跟踪 | 全局缺失 |
| S2 | 无统一 `IEditorSaveService`（`MainWindowViewModel` 直接调 `Serializer`） | `PrimalEditor_Avalonia/ViewModels/MainWindowViewModel.cs:234-249` |
| S3 | `AutoSaveService` 使用 `File.GetLastWriteTime` 作为隐式 dirty（脆弱） | `PrimalEditor_Avalonia/Common/AutoSaveService.cs:47-60` |
| S4 | 无关闭确认（关窗口不提示保存） | `MainWindow.axaml.cs` 无 `OnClosing` |
| M1 | Primitive Mesh `Create` 当 native 未加载时写 `# PrimalEngine placeholder mesh: ...` 文本文件 | `PrimalEditor_Avalonia/Editors/PrimitiveMesh/PrimitiveMeshViewModel.cs:97-110` |
| M2 | Primitive Mesh `Create` 当 native 加载时 `ProceduralMeshCreate` 后**没有写 `.mesh` 文件**，却 `AssetRegistry.Register` 不存在路径 | `PrimalEditor_Avalonia/Editors/PrimitiveMesh/PrimitiveMeshViewModel.cs:112-135` |
| M3 | 无 `Create` 状态机、失败清理、取消路径 | 同上 |

## 任务依赖图

```
L1 布局修复  ─┐
L2/L3 死入口 ─┼─→  P0-A: 基础修复
L4-L6 异步   ─┘
                 ↓
U1 旧/新覆盖 ─┐
U2 事务缺失  ─┼─→  P0-B: 事务式 Undo/Redo
U3 批处理    ─┘
                 ↓
S1 跟踪缺失  ─┐
S2 入口不统一┼─→  P0-C: Dirty/Save 服务
S3 脆弱检测  ─┤
S4 关闭确认  ─┘
                 ↓
M1 占位文件  ─┐
M2 不写盘   ─┼─→  P0-D: Primitive Mesh 真实落盘
M3 状态/清理 ─┘
                 ↓
              P0-E: 验证（编译 + smoke test）
```

## 实施边界

- **不**重新设计 `IAssetEditor`、`IAssetEditorRegistry`、`ContentWatcher`、Texture/Geometry Editor — 这些是 feature-parity T1-T9 范围，由前一会话实现。
- **不**触及 `EngineDLL/AssetAPI.*` 已有修改。
- **不**触及 C++ 引擎。
- **不**改 `PrimalEditor.csproj` 引入新 NuGet 包。
- **不**实现 Close-confirm autosave 窗口的 UI（仅接入 `OnClosing` 调用 Service，Service 内部决策）。
- **不**为 Primitive Mesh 引入并发/异步任务（与平台稳定性原则保持一致 — 同步完成所有 cook/write/register，超时由调用方 cancel）。

## 验收标准（与未达成对照）

### 布局
- [ ] WorldEditor 工具栏不与主区域重叠（视觉/Inspector/Hierarchy 与工具栏不交叉）。
- [ ] `DebugMessage` 状态条仍在底部，不遮挡视口。
- [ ] `MinHeight/MinWidth` 200 在工具栏下方仍生效。

### 死入口
- [ ] F4 / F5 任何一个有真实 command handler，或 `InputGesture` 与菜单项均不可见/禁用。
- [ ] 没有任何 `MenuItem` 的 `InputGesture` 没有 `Click` 或 `Command`。

### 异步
- [ ] 4 处 ShowDialog 全部 `await` 化。
- [ ] 没有 `.Wait()` / `.GetAwaiter().GetResult()` / `.Result` 用在 ShowDialog 返回值上（除 `Program.cs:56` 的已知 AwaitDisplayLink）。

### Undo/Redo
- [ ] `TransformAction.Undo()` 与 `Redo()` 之间可以实际还原旧/新值（不是相同）。
- [ ] `RecordUndo` 在同一次"属性编辑"中只产生 1 个 Action（不按字符/按键分裂）。
- [ ] `AddEntity` / `DuplicateEntity` 批量粘贴时只产生 1 个 `AddEntityAction`（包含所有 entity）。
- [ ] `BeginTransaction` / `CommitTransaction` / `AbortTransaction` API 存在并可嵌套。
- [ ] `Undoing`/`Redoing` 标志位抑制回弹 Action。

### Dirty/Save
- [ ] `Project.IsDirty`、`Scene.IsDirty` 存在。
- [ ] 编辑操作自动 `MarkDirty()`。
- [ ] `IEditorSaveService` 单一入口（Save / SaveAll / ConfirmIfDirty / SaveAs）。
- [ ] `OnClosing` 触发 dirty 检查。
- [ ] `AutoSaveService` 改用 `IsDirty` 替代 `File.GetLastWriteTime`。

### Primitive Mesh
- [ ] `Create` 流程真实写 `.mesh` 二进制文件（依赖 C++ 端 `CookPrimitiveMesh`，或 C# 端 fallback 写已知的内部格式）。
- [ ] `AssetRegistry.Register` 只在 `.mesh` 实际存在时执行。
- [ ] 失败/取消时删除已生成文件、清理半成品。
- [ ] 创建状态可在 UI 显示。
- [ ] GUID 稳定（重新扫描不改变 `AssetInfo.Guid`）。

## 每个修复的范围

### P0-A: 基础修复（独立，并行）

#### A1. WorldEditor 布局重叠
**文件**: `PrimalEditor_Avalonia/Views/WorldEditorView.axaml`

**根因**: 
- Row=0 (Auto) 同时被 Toolbar (line 16) 和主 Grid (line 28) 使用 — 两元素叠在 Auto 行。
- Row=1 (`*`) 被 DebugMessage (line 90) 占据。
- 视口（column 1 主 Grid 的子）实际渲染在 Row=0，被 Toolbar 覆盖。

**修复**: 重排为 `RowDefinitions="Auto,*,Auto"`，Toolbar 放 Row=0，主区域 Grid 放 Row=1，DebugMessage 放 Row=2。**单一** Grid 包含所有。

#### A2. F4 / F5 死入口
**文件**: `PrimalEditor_Avalonia/Views/MainWindow.axaml`

**修复**: 把 `InputGesture` 改为 `Command` 绑定到一个 `RelayCommand`，命令体切换 `MainWindowViewModel` 中相应 View 的 `IsVisible`。或者更稳妥：直接把 `MenuItem.IsEnabled = False` + `InputGesture` 移除，保留菜单项做说明。

**决策**: 接通 command。F4 = 切换 WorldEditor Tab 可见性，F5 = 切换 Content Browser Tab 可见性。涉及 `MainWindowViewModel` 增加 `IsWorldEditorVisible/IsContentBrowserVisible` 绑定。

#### A3. 同步等待异步对话框
**文件**: 
- `PrimalEditor_Avalonia/Views/WorldEditorView.axaml.cs:144-165` (OnEntityDoubleClick)
- `PrimalEditor_Avalonia/ViewModels/MainWindowViewModel.cs:280-297` (RenameEntityExecute)
- `PrimalEditor_Avalonia/ViewModels/MainWindowViewModel.cs:188-198` (AboutExecute)

**修复**:
- `OnEntityDoubleClick`/`OnEntityDoubleClick` 等 handler 改为 `async void`（Avalonia 事件允许）；`ShowDialog` 用 `await`。
- `AboutExecute` `dialog.ShowDialog(topLevel);` 加 `await`。
- `RenameEntityExecute` 改 `async Task`，`Execute` 入口用 `async void` 转发或拆出 `_RenameEntityAsync` 方法。

### P0-B: 事务式 Undo/Redo

#### B1. 修复 TransformAction 旧/新值
**文件**: `PrimalEditor_Avalonia/ViewModels/PropertiesViewModel.cs:88-108`

**根因**: `RecordUndo` 在 set 之后被调用，两次循环都从 transform 抓"当前"值，oldDataList 与 newDataList 完全相同。

**修复**: 引入"基线快照"模式。`BeginEdit()` 时记录 `_oldDataList`（从 transform 抓一次）。`EndEdit()` 时记录 `_newDataList`（再抓一次），构造 `MultiEntityTransformAction` 并提交。

#### B2. 事务 API
**文件**: `PrimalEditor_Avalonia/Common/UndoRedo.cs`

**API**:
```csharp
public IDisposable BeginTransaction(string name);  // returns IDisposable that commits on Dispose
```
实现：
- 嵌套计数 `_transactionDepth`
- 顶层 Begin 时建 `_pendingActions` 列表
- 顶层 Commit 时把所有 pending actions 包装成 `TransactionAction`（`IUndoRedoAction` 实现，内部是 List），一次性 Add 到 undo stack
- Abort/Dispose-with-flag 丢弃所有 pending

`RecordUndo` 改为：如果在事务中，加到 `_pendingActions`；否则立即 Add（保持向后兼容）。

**添加标志位**：
```csharp
public bool IsUndoingOrRedoing { get; }
```
Action.Undo/Redo 内部置 true，调用结束置 false。MSComponent/PropertiesViewModel 在此标志位下不触发新的 RecordUndo。

#### B3. 批量 Entity 操作合并
**文件**: `PrimalEditor_Avalonia/ViewModels/MainWindowViewModel.cs:172-186, 270-306`

**修复**: `DeleteEntityExecute`/`DuplicateEntityExecute` 包裹在 `using (UndoRedo.BeginTransaction("Delete N entities"))` 中，每条 entity 的 `Add/Remove` Action 收集在事务内。

### P0-C: Dirty/Save 服务

#### C1. IsDirty 跟踪
**文件**: 
- `PrimalEditor_Avalonia/GameProject/Project.cs` (加 `IsDirty`)
- `PrimalEditor_Avalonia/GameProject/Scene.cs` (加 `IsDirty`)

**API**:
```csharp
public bool IsDirty { get; private set; }
public void MarkDirty();
public void ClearDirty();
```
`Project` 暴露 `PropertyChanged` 通知 UI 标题显示 `*`。

#### C2. IEditorSaveService
**新建**: `PrimalEditor_Avalonia/Common/IEditorSaveService.cs`

```csharp
public interface IEditorSaveService
{
    bool SaveProject(Project project, bool saveAll = false);
    bool SaveScene(Project project, Scene scene);
    bool SaveAsset(Project project, AssetInfo asset);
    bool SaveAll(Project project);
    bool ConfirmIfDirty(IReadOnlyList<object> dirtySubjects);  // modal dialog
}
```

**实现**: `EditorSaveService`（单例静态类，包装 `Serializer` + 关闭确认）。

#### C3. 关闭确认
**文件**: `PrimalEditor_Avalonia/Views/MainWindow.axaml.cs`

**修复**: 重写 `OnClosing`（Avalonia 事件）调用 `EditorSaveService.ConfirmIfDirty`；`Cancel` 时 `e.Cancel = true`。

#### C4. AutoSave 改用 IsDirty
**文件**: `PrimalEditor_Avalonia/Common/AutoSaveService.cs`

**修复**: 改用 `_trackedProject.IsDirty` 替代 `File.GetLastWriteTime`。

### P0-D: Primitive Mesh 真实落盘

#### D1. 状态机
**文件**: `PrimalEditor_Avalonia/Editors/PrimitiveMesh/PrimitiveMeshViewModel.cs`

**状态**:
```csharp
public enum PrimitiveMeshCreationState
{
    Idle, Generating, Cooked, Persisted, Registered, Failed, Cancelled
}
```

**修复**:
- `Create(string projectContentPath)` 改为同步生成 mesh 数据 → 调 native 烘焙 → 真实写入 `.mesh` → `AssetRegistry.Register`。
- 任何一步失败：删除已生成文件、回滚到 `Failed`。
- native 缺失时返回失败（不写占位文件），引导用户先加载 native 引擎。

#### D2. GUID 稳定
**文件**: `PrimalEditor_Avalonia/Content/AssetInfo.cs` / `AssetRegistry.cs`

**修复**: `AssetRegistry.Register` 先按路径在 `_byPath` 查；命中则复用旧 GUID（不重新 `Guid.NewGuid()`）。已经在 `2026-07-28-avalonia-feature-parity.md` T1 Step 4 中描述，本次需验证已正确实现。

#### D3. 失败清理
**修复**: `Create` 用 try/catch 包住：
- cook 失败 → 删 `.mesh.partial`（如果存在）
- write 失败 → 删 `.mesh`、释放 meshId
- register 失败 → 删 `.mesh`、释放 meshId

## 验证（实施末尾执行）

1. `dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor` → 0 errors, warnings < 30
2. `cmake --build build-wt --target Engine` → exit 0
3. `cmake --build build-wt --target EngineDLL` → exit 0
4. 视觉自检（仅当有可能时）— 在现有 smoke test 中加入：
   - 启动后 WorldEditor 工具栏不重叠主区域
   - F4/F5 切换 Tab 可见性生效
   - 创建一个 Primitive Mesh → Content/{Name}.mesh 存在
5. `tasks/lessons.md` 追加 P0 教训（不要覆盖历史）。

## 已知限制（不解决）

- 跨平台视口抽象（`IEditorViewportHost`）属于 P1，本轮不做。
- `ComponentRegistry` / 多选 `MSComponent` 已存在（T3），本轮不重构。
- 真实 `EngineSync`（C++ 引擎事务同步）属于 P1。
- Content Browser CRUD 属于 P1。
- `AsyncResourceLoader::CookPrimitiveMesh` 仍依赖 C++ 端实现或 fallback；如果 C++ 端未实现，写入 fallback `.mesh`（含 BoundingBox/Vertices=0 的 metadata-only），但**仅在 native 实际报告成功后**才写，不写"占位文本"。

## 实施顺序（推荐）

1. P0-A 布局/死入口/异步（一组小改动，立即可编译）
2. P0-B Undo/Redo 事务（依赖现有 UndoRedo 框架）
3. P0-C Dirty/Save（依赖 Project/Scene 模型）
4. P0-D Primitive Mesh 真实落盘（独立，可在 P0-B 之前做）
5. P0-E 验证

每步完成后立即 `dotnet build` 验证（不超过 5-15 个文件增量）。
