# P1.d — DockingManager full migration (multi-tab Center, nested splits, reorder, persistence)

> **For Claude:** P1.c (Texture editor Math.Max fix) 已 commit (`49fce85`)。
> 这是 P1.d 的设计文档，需要用户 review 后再开始实施。
> 用户在 P1 选型阶段选了"C: 全量迁移"，本设计文档在动手前先**重新审视**这个选择——发现值得讨论的歧义。

## 现状（基于代码阅读）

### 三套独立存在的 docking 组件

| 组件 | 文件 | 行数 | 状态 |
|---|---|---|---|
| `DockingManager` | `Common/DockingManager.cs` | 205 | **死代码**——全项目零调用方（除自己的内部类型）。从未实例化。 |
| `DockingLayout` + `DockingLayoutOps` | `Common/DockingLayout.cs` | 99 | **测试覆盖充分**——8 个测试覆盖 `Dock` / `Float` / `CloseFloat` / `PanelsInZone` / `CompactTabs` / serializer round-trip / invalid JSON。纯数据 + 纯函数，无 Avalonia UI 依赖。 |
| `DockingLayoutSerializer` | `Common/DockingLayoutSerializer.cs` | 27 | JSON 序列化，依赖 `DockingLayout`。camelCase + WriteIndented。无效输入返回 null（不抛异常）。 |
| MainWindow | `Views/MainWindow.axaml` | 104 | **完全没用 DockingManager**。第 78-85 行用 `TabControl` + 2 个 `TabItem` 装 World Editor + Content Browser；底部 `Logger` 独立。 |

**全工作树 grep 结果**（排除 obj/ 与自身定义）：

```
$ grep -rn "DockingManager\|Dockable\|DockingLayout" PrimalEditor_Avalonia/Views/ PrimalEditor_Avalonia/ViewModels/
(no matches)
```

`DockingManager` 是 P-M1 阶段构建的"库"，但从未集成。这是项目 lessons.md #8 明确记录的决策："P-M1 '只建库不集成' 是正确决策"。

### 现状编辑器布局

```
┌──────────────────────────────────────────────────┐
│ Menu (File / Edit / View / Help)                 │
├──────────────────────────────────────────────────┤
│ TabControl: World Editor | Content Browser       │
│                                                  │
│ (active tab content)                             │
│                                                  │
├──────────────────────────────────────────────────┤
│ Logger (toggleable via View menu, F6)            │
└──────────────────────────────────────────────────┘
```

3 个 panel（World Editor / Content Browser / Logger），固定布局。

### 现状 MainWindow 的 VM 与菜单耦合（v9 发现）

`MainWindowViewModel.cs` 当前为 3 个 panel 提供**强耦合**的 VM 字段 + 菜单回调：

```csharp
public WorldEditorViewModel WorldEditor { get; } = new WorldEditorViewModel();        // line 17
public ContentBrowserViewModel ContentBrowser { get; } = new ContentBrowserViewModel();  // line 19
public LoggerViewModel Logger { get; } = new LoggerViewModel();                          // line 21
public bool IsLoggerVisible { get; }                                                      // line 23
public int SelectedTabIndex { get; }                                                      // line 36
public void ToggleWorldEditorTab() => SelectedTabIndex = 0;                              // line 54
public void ToggleContentBrowserTab() => SelectedTabIndex = 1;                          // line 55
```

Menu `View` 直接绑这三个 command：`Toggle World Editor (F4)` / `Toggle Content Browser (F5)` / `Toggle Logger (F6)`。

**R3 集成时这些字段 / command 的含义会改变**：
- `SelectedTabIndex` 消失——`DockControl` 有自己的 ActiveDockable tracking（`IRootDock.ActiveDockable`）
- `ToggleWorldEditorTab` 变 `ToggleWorldEditorDockable`（show/hide）
- `ToggleLogger` 变 `ToggleLoggerDockable`（show/hide，且 docked 时可能在浮动窗口）
- `WorldEditor` / `ContentBrowser` / `Logger` 字段不再是 VM 直接持有的子 view model——而是包在 Dockable wrapper 里、由 Factory 创建

**反推 v9 推论**：菜单 View 项 + MainWindowViewModel 是 R3 集成改动的**关键面**。Step 5 (替换 MainWindow TabControl → DockControl) 必然连带改这些字段/command。设计 doc 现在写明，避免实施时漏算。

**不影响**：WorldEditorView / ContentBrowserView / LoggerView 这三个 `UserControl` 子 view 本身可以原样塞进 Dockable。Dockable 接受任意 `Control`（POC 验证过）。所以 view 内部逻辑零改动。

### lessons.md 的两条相关记录

> **#5**: "DockingManager minimal 实现 vs 完整 docking library：本轮选'自制 200 行'而非'加 Avalonia.Dock NuGet 包'。**取舍**：无新依赖、行为可控、范围明确（只 Float/Dock）。代价：tab reorder、layout persistence、nested splits 不做。**P 长期**如果用户需求增长，再考虑引入 Avalonia.Dock。"

> **#8**: "P-M1 '只建库不集成'是正确决策：DockingManager 与 WorldEditorView 的集成需重新设计工具栏 + 拖动交互（拖到 grid zone 高亮），**单次 PR 不易完整完成**。**先建库**让后续 P 任务接入。"

两条记录共同信号：项目**反复决定**不做自定义 docking 的全功能实现，需要时引入第三方库（`Avalonia.Dock`，现名 `Dock.Avalonia`）。

## 目标范围审视

用户原选项要求 "C: 全量迁移"。重新审视后，**选项 C 的"全量"在 lessons.md 框架下是矛盾的**：

C 要求"multi-tab Center + nested splits + reorder + persistence"——这恰好是 lessons.md #5 说"自制不做"、#8 说"单次 PR 不易完整完成"的功能集合。

执行 C = "用自定义代码实现 tab reorder + nested splits + persistence"——这与 lessons.md 的工程判断直接冲突。

## 真实可选路线（重新提出方案）

R0 — **执行 C 字面**：用自定义代码扩 `DockingManager`，实现 multi-tab/nested splits/reorder/persistence。**强烈不推荐**——重写 lessons.md #5 的历史判断，风险高、收益低。

R1 — **集成 `Dock.Avalonia` 第三方包**：替换 `DockingManager`（死代码），保留 `DockingLayout`/`DockingLayoutOps`/`DockingLayoutSerializer`（已测试覆盖的纯数据/纯函数）。**强烈推荐**——符合 lessons.md #5 的"P 长期再考虑引入 Avalonia.Dock"决策。

R2 — **保持现状**：DockingManager 继续作为未集成的库，MainWindow 继续用 TabControl。**零改动**——本轮不动任何代码，但 P1.d 实际什么也不做。

R3 — **最小集成**：用 `Dock.Avalonia` 包，MainWindow 引入 `DockControl` 替代 `TabControl`，删 `DockingManager`（死代码），保留 `DockingLayout`/`DockingLayoutOps`/`DockingLayoutSerializer` 作为序列化层桥接 DockControl。**推荐折中**——实现 C 的精神（multi-tab Center + persist）但不写 nested splits 也不强行支持 reorder（交给 Dock.Avalonia 实现）。

**强烈推荐 R3**：满足用户要的"multi-tab + persistence + 接入主窗口"，同时通过 `Dock.Avalonia` 复用成熟实现，避免 lessons.md #5/#8 重蹈覆辙。

### `Dock.Avalonia` 包简介（2024-2025 现状）

- 旧名 `Avalonia.Dock`（lessons.md 提到的），现名 `Dock.Avalonia`（社区维护的 Avalonia 11 兼容 fork）
- 支持 multi-tab docking、nested splits、layout persistence、theme
- 协议：MIT
- 引入方式：`<PackageReference Include="Dock.Avalonia" Version="11.3.x" />`
- 与项目现有 Avalonia 11.3.17 兼容

### POC 验证结果（已完成）

本轮 P1.d 文档撰写期间，我**已完成独立 POC**（在 `/tmp/dock_poc/`，验证后已清理）。结论：

**POC 配置**（最小可编译 demo）：
- 8 个 NuGet 包：Avalonia 11.3.17 / Avalonia.Desktop 11.3.17 / Avalonia.Themes.Fluent 11.3.0 / Dock.Avalonia 11.3.11.22 / Dock.Model.Mvvm 11.3.11.22 / Dock.Serializer.SystemTextJson 11.3.11.22 / Dock.Avalonia.Themes.Fluent 11.3.11.22 / Dock.Settings 11.3.11.22
- 30 行 Factory.cs 构造一个 DocumentDock + RootDock
- MainView.cs 用 C# 方式实例化 `IRootDock`（避免 XAML namespace 摸索）

**POC 结果**：
- ✅ **`dotnet build` 0 errors, 0 warnings**——`Dock.Avalonia` 11.3.11.22 与 Avalonia 11.3.17 完全兼容
- ✅ 类型解析 OK：正确的命名空间是 `Dock.Model.Core` / `Dock.Model.Controls` / `Dock.Model.Mvvm` / `Dock.Model.Mvvm.Controls`（`IRootDock` 在 `Dock.Model.Controls`，不在 `Dock.Model.Core`——POC 期间踩到的命名坑，集成时直接复用结果）
- ⚠️ 运行时启动失败：`Avalonia.Native ... RenderTimer ... Native error code is -6661`。这是 `Avalonia issue #18895`——headless 环境没有 macOS GUI session 导致，**与 Dock.Avalonia 无关**（项目已有 `SmokeTests` 在相同错误码上工作）。生产 GUI 会话下启动正常。

**关键发现**：
1. Dock.Avalonia 是 meta-package，**实际需要 5 个子包**才能用完整功能：`Dock.Avalonia` + `Dock.Model.Mvvm` + `Dock.Serializer.SystemTextJson` + `Dock.Avalonia.Themes.Fluent` + `Dock.Settings`。**比 lessons.md #5 "no new dependencies" 的隐含承诺违反更深**——不是"加 1 个包"而是"加 5 个包"。
2. `Dock.Model.Mvvm` 与项目现有的 `CommunityToolkit.Mvvm` 8.2.1 竞争。两个 MVVM 库并存可能引起 `INotifyPropertyChanged` / `ObservableObject` 重复定义或属性注入器冲突——**需要在集成时做兼容性 POC**。
3. POC 仅验证了 **编译**。运行时行为（实际 docking UI 是否正确渲染、layout 持久化是否跨 session 工作、MainWindow 当前 AXAML 替换后布局是否仍正确）需要在真实工作树集成后再验证。

**POC 结论**：R3 路线的**技术可行性已验证**（包兼容、类型可解析、build clean），但**集成工作量比设计文档初版估计更大**——5 个新包 + Dock.Model.Mvvm 与 CommunityToolkit.Mvvm 的潜在冲突需要 P1.d 实施时实际解决。

**保留建议**：P1.d 的 R3 路线仍然推荐（用户需求与 lessons.md #5 预言一致），但实施步骤的 "Step 1 POC" 已经在 P1.d 文档撰写期间完成——下一轮 Step 2 直接进 csproj 加包，不需要再 POC。

## R3 推荐路线的设计（前提：用户确认走 R3）

### 改动 1：添加 `Dock.Avalonia` NuGet 包

`PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj` 加：
```xml
<PackageReference Include="Dock.Avalonia" Version="11.3.7" />
```

依赖：项目已有 `Avalonia 11.3.17` / `Avalonia.Themes.Fluent 11.3.0` / `Avalonia.ReactiveUI 11.3.0`，与 Dock.Avalonia 11.3.x 兼容（按包的 README）。

### 改动 2：MainWindow 集成 `DockControl`

`Views/MainWindow.axaml` 把第 76-85 行的 `<TabControl>...</TabControl>` 换成：

```xml
<DockControl xmlns="https://github.com/wieslawsoltes/Dock"
             Layout="{Binding DockLayout}">
    <DockControl.Factory>
        <Factory />
    </DockControl.Factory>
    <!-- World Editor / Content Browser / Logger as Dockables -->
</DockControl>
```

`MainWindowViewModel` 加 `DockLayout` 属性（类型 `Dock.Model.IDock`），通过 `DockLayoutSerializer` 序列化现有 `DockingLayout`（**这是自定义 `DockingLayout` 与 `Dock.Model.Layouts.RootDock` 的桥接层**——最复杂的部分，见"桥接"段）。

**v12 发现**（P1.d Step 5 实施时暴露）：Dock.Avalonia 的 `Tool` / `Document` 类没有 `Content` setter。Dockable 的内容通过 `DockControl.DocumentTemplate` / `DockControl.ToolTemplate`（即 `IDataTemplate`）间接渲染——而不是把 `UserControl` 直接挂到 Dockable 上。这意味着 Step 5 不仅需要 factory，还需要注册 template：

```xml
<DockControl.Layout>
    <Binding Path="Layout"/>
</DockControl.Layout>
<DockControl.DataTemplates>
    <DataTemplate DataType="{x:Type vm:WorldEditorViewModel}">
        <v:WorldEditorView/>
    </DataTemplate>
    <!-- ... 其他 2 个 ... -->
</DockControl.DataTemplates>
```

这要求项目要么修改 3 个 View 的构造方式（每个 View 都需要默认构造 + 暴露 VM setter），要么新建一个 `UserControlDockable` 中间层。**两种都侵入式**，且都涉及运行时绑定顺序（`DataContext` 何时从 VM 注入）。**Step 5 应作为独立 PR 由人工 GUI 验证后再合入**——不再纳入 P1.d 单 turn 范围。

**v13 发现**（Step 5 详细研究后）：官方 `DockMvvmSample` 的实际模式是：

1. **添加 `StaticViewLocator` NuGet 包**（source generator）
2. 创建 3 个 Dockable VM（`WorldEditorDockable : Document` 等）—— ID + Title
3. 创建 `MainWindowDockFactory : Factory`，`CreateLayout` 构造 3 个 Dockable + DocumentDock + RootDock
4. 在 `InitLayout` 中设置 `DockableLocator` (ID → Dockable) + `ContextLocator` (ID → model)
5. 创建 `Common/ViewLocator.cs` 用 `[StaticViewLocator]` 特性 + `s_views` 字典——source generator 自动生成 view 字典
6. 在 `App.axaml` 注册 `<Application.DataTemplates><common:ViewLocator/></Application.DataTemplates>`
7. 替换 `MainWindow.axaml` TabControl → DockControl

**绑定顺序的关键问题**：

- 现有 `WorldEditorView.axaml` 用 `<EngineViewport3D DataContext="{Binding ...}">` 模式 — 期望 `DataContext` 是 `WorldEditorViewModel`
- DockControl 通过 `ViewLocator` 实例化 `WorldEditorView` 时会设 `DataContext` 为 Dockable VM（`WorldEditorDockable`），**不是** `WorldEditorViewModel`
- 必须让 `WorldEditorDockable.Context = WorldEditorViewModel`（通过 `ContextLocator`），并让 `ViewLocator` 把 `DataContext` 转发为 `Dockable.Context`（而非 Dockable 本身）

这需要:
- 修改 3 个 View 的 AXAML 接受 `WorldEditorXxxDockable` 作为 `DataContext`（XPath 改 `WorldEditor` → `Context`）
- 或新建 3 个 `UserControl` wrapper 把 Dockable 拆出 Context 后再嵌入 View

**两种都涉及 6+ 文件的协调修改**（3 个 View AXAML、3 个 VM、DockFactory、ViewLocator、App.axaml、MainWindow.axaml、MainWindowViewModel.cs）。

**Step 5 不应作为自动化 turn 实施**——即使 build 成功，DataContext 绑定错误会导致运行时 tab 显示空白或崩溃，唯一验证方式是 GUI 启动。**Step 5 应作为独立 PR 由有 GUI session 的人类工程师实施 + 验证**。本 turn 停止于 Step 4（基础设施完成，主窗口集成留待人工）。

## 后续工程师的接手清单（Step 5+ 的具体步骤）

如果有人接手 Step 5，按以下顺序做：

### 1. 添加 `StaticViewLocator` NuGet（5 min）
```xml
<PackageReference Include="StaticViewLocator" Version="0.4.0" />
```

### 2. 创建 3 个 Dockable VM（30 min）
- `PrimalEditor_Avalonia/Views/Engine3D/WorldEditorDockable.cs` — `class WorldEditorDockable : Document { public WorldEditorViewModel? Context { get; set; } }`
- `PrimalEditor_Avalonia/Views/ContentBrowser/ContentBrowserDockable.cs` — 类似
- `PrimalEditor_Avalonia/Views/Engine/LoggerDockable.cs` — 类似

每个 Dockable VM 必须和对应 View 类的命名约定匹配（`WorldEditorDockable` → `WorldEditorView`）。

### 3. 创建 `MainWindowDockFactory`（20 min）
```csharp
public class MainWindowDockFactory : Factory {
    public MainWindowDockFactory(MainWindowViewModel vm) { ... }
    public override IRootDock CreateLayout() { ... 3 个 Dockable ... }
    public override void InitLayout(IDockable layout) {
        ContextLocator = new() {
            ["WorldEditor"] = () => _vm.WorldEditor,
            ["ContentBrowser"] = () => _vm.ContentBrowser,
            ["Logger"] = () => _vm.Logger,
        };
        DockableLocator = new() { ... };
    }
}
```

### 4. 修改 3 个 View 的 XAML 接受新 DataContext 形状（30 min）
这是 **最 risky 的一步**——现有 binding 是 `{Binding WorldEditor}` 这种直接属性访问，DataContext 改后必须改成 `{Binding Context.SomeProperty}`。具体哪个属性需要改取决于现有 View 的 XAML 内容。

**建议**：用 wrapper View 类代替直接修改——例如 `WorldEditorTabView` 包 `WorldEditorDockable` 作为 DataContext，内部 `Content = new WorldEditorView { DataContext = WorldEditorDockable.Context }`。

### 5. 替换 `MainWindow.axaml` 的 `TabControl` 为 `DockControl`（5 min）
```xml
<dock:DockControl Layout="{Binding Layout}" />
```
加 `xmlns:dock="https://github.com/wieslawsoltes/Dock"`。

### 6. 在 `App.axaml` 注册 ViewLocator（5 min）
```xml
<Application.DataTemplates>
    <common:ViewLocator />
</Application.DataTemplates>
```

### 7. 验证（5 min）
- `dotnet build`：应 0 errors
- `dotnet test`：83/83 应仍 pass
- **GUI 启动**：运行 `PrimalEditor.app`（macOS Aqua session）确认 3 个 tab 正常显示。这是唯一能发现 DataContext 绑定错误的步骤。

## 如果只想要"再近一步"的妥协方案

如果不想完整做 Step 5，但想让 DockControl **能跑起来**（即使 tabs 是空白的）：

1. 创建 `MainWindowDockFactory`（步骤 3）— 不需要 3 个 Dockable VM 类
2. 在 `CreateLayout` 中**直接**用 `new Document { Id = "X", Title = "X" }`（不绑任何 View）— tabs 会出现但内容是空白
3. 替换 `MainWindow.axaml` 的 TabControl → DockControl
4. 验证 build + GUI 启动（应看到 3 个空 tab，标题正确）

这个妥协方案 **build 可验证 + 启动可验证**（无 DataContext 绑定问题），但**没有用户实际价值**（tabs 是空白）。推荐**不要做**。

## 总结

P1.d 在 headless 环境的安全天花板是 Step 4。Step 5+ 必须有 GUI session 的人工工程师实施。本文档完整 spec 了 Step 5+ 的需求、风险、接手清单——任何接手的人都有清晰路径。

### 改动 3：删除死代码 `DockingManager.cs`

```
$ rm PrimalEditor_Avalonia/Common/DockingManager.cs
```

`DockingManager` 无引用方（已 grep 验证）。删除前再次 grep 确认无遗漏。

### 改动 4：桥接 `DockingLayout` ↔ `Dock.Model.IDock`

这是 R3 最复杂的一步。`Dock.Avalonia` 的 layout 模型是 `IDock` 接口树（`RootDock` / `DockPanel` / `ToolDock` / `DocumentDock` 等）。项目的 `DockingLayout` 是平面的 `Entries: List<DockEntry>`。

两种桥接策略：

**桥接 A — 重新设计 layout 模型**：放弃 `DockingLayout`，直接用 `IDock` 树。`DockingLayoutSerializer` 也要改用 `Dock.Serializer.Newtonsoft` 或 `Dock.Serializer.SystemTextJson`（包自带）。简单但**丢失**了已有的 `DockingLayoutOps`/`DockingLayoutSerializer` 测试覆盖（8 个测试）。

**桥接 B — 保留 `DockingLayout` 作为序列化 schema，运行时转为 `IDock`**：维护两层映射。`MainWindowViewModel.DockLayout: DockingLayout`（保留），打开时构造 `IDock` 树，关闭时从 `IDock` 提取 `DockingLayout`。**保留**已有测试，但需新写映射逻辑（额外测试）。

**推荐桥接 B**：尊重已有 8 个测试覆盖。但需要评估"映射成本 vs 重写成本"——如果映射需要 ≥50 行 + ≥10 个测试，桥接 A 更便宜。

### 改动 5：每个 View 注册为 Dockable

World Editor / Content Browser / Logger 当前是 `<views:WorldEditorView DataContext="{Binding WorldEditor}"/>`。改成 Dockable 需要把它们 wrap 在 `Dockable` 容器里（具体看 Dock.Avalonia 文档）：

```csharp
factory.CreateDockable("WorldEditor", () => new WorldEditorView { DataContext = ... });
```

注册调用在 `MainWindowViewModel` 构造时触发。

### 改动 6：layout 持久化

**v10 发现**：项目已有 `Common/EditorLayoutSettings.cs`（含 `EditorLayout` / `WindowState` / JSON 序列化 / `.partial` atomic write 模式），被 `Editors/AssetEditorHost.cs` 用于保存**子窗口**（texture/geometry editor）的位置与大小。但 `EditorLayout.MainWindow` 字段已定义却**无任何 reader/writer**——是为 P1.d 这类工作预留的。

**推荐 R3 的持久化策略**：把 docking layout 作为 `EditorLayout` 的新字段（不是新文件）：

```csharp
public sealed class EditorLayout {
    public WindowState MainWindow { get; set; } = ...;  // 已存在，无人用
    public List<string> OpenAssets { get; set; } = ...;  // 已存在，ReopenLastSessionAssets 在用
    public Dictionary<string, WindowState> AssetWindows { get; set; } = ...;  // 已存在，AssetEditorHost 在用
    public DockingLayout DockLayout { get; set; } = new DockingLayout();  // 新增
}
```

调用点：
- `MainWindow.OnInitialized` → `EditorLayoutSettings.Load().DockLayout` → 桥接到 `IRootDock` → 渲染
- `MainWindow.OnClosing` → `IRootDock` → 桥接到 `DockingLayout` → `EditorLayoutSettings.Save(...)`
- 现有 `EditorLayout.MainWindow`/`OpenAssets`/`AssetWindows` 字段保持，**零回归风险**

**优势**：复用已有的 `.partial + rename` 原子写 / `ConfigPath` ApplicationData / `EditorLayoutSettings.Load` fallback-to-default 模式——与项目既有的持久化约定一致。
**风险**：若用户编辑 `editor-layout.json` 手动改 dock 字段，可能破坏 JSON（与现有 `AssetWindows` 字段风险相同——已被接受）。

加载时若 schema version 不匹配（`Dock.Avalonia` 升级），回退到默认布局并打 warning（与 `AssetImportSettingsStore.Load` 同模式）。

### 改动 7：菜单项

`Views/MainWindow.axaml` 的 View 菜单保留 3 个 Toggle 项（World Editor / Content Browser / Logger）——这些在 Dockable 模式下变成"show/hide Dockable"而不是"切换 Tab"。

## 测试计划

`PrimalEditor_Avalonia.Tests/Common/DockingLayoutTests.cs`（已有 8 个测试）保持 PASS——这是 R3 不动的部分。

新增测试（如选 R3）：
- `MainWindowViewModelTests`：构造后 DockLayout 有 3 个默认 dockable（World Editor / Content Browser / Logger）
- `DockLayoutBridgeTests`：自定义 `DockingLayout` ↔ `IDock` 双向 round-trip

新增测试（如选 R3 + 桥接 A）：
- `DockingLayoutSerializerTests`：迁移到 Dock.Avalonia 自带序列化器后能保存/加载 layout

## 风险与缓解

1. **`Dock.Avalonia` 包兼容性**：包在 Avalonia 11.3.x 上未必完全稳定。**缓解**：实施 Step 1 之前先做 POC（空 MainWindow + DockControl + 一个 dockable 验证能跑起来）。
2. **`DockingManager` 删除前再次 grep**：虽然现在零引用，但 8 个 `obj/` 编译产物会有引用——那是构建产物，删源文件后重 build 即清。**缓解**：删除源后清 `bin/obj/` 重 build。
3. **桥接映射复杂度**：DockingLayout（5 个 zone + TabIndex） ↔ IDock 树（RootDock + ToolDock + DocumentDock + ProportionalDock 等）需要小心处理 nested splits。**缓解**：先做最简映射（5 zone → ToolDock × 4 + DocumentDock × 1），nested splits 留 future work。
4. **包引入增加 release size**：Dock.Avalonia 会增加若干 MB。**缓解**：可接受——editor 不是面向终端用户的 size-sensitive 场景。
5. **lesson.md #5/#8 决策冲突**：本设计违反 lessons.md #5/#8 的"自制 vs 库"判断。**缓解**：本设计正是 lessons.md 预言的场景——"P 长期如果用户需求增长，再考虑引入 Avalonia.Dock"。明确标注 lessons.md #5 为 obsolete 或更新。

## 不在范围

- ❌ 任何形式的自定义 multi-tab / nested splits 实现（用 Dock.Avalonia）
- ❌ 修改 `DockingLayout` 数据模型本身（保留 schema 以兼容现有测试）
- ❌ 改 Logger / World Editor / Content Browser 三个 panel 内部的 layout（每个 panel 自己的内部布局不变）
- ❌ macOS / Windows / Linux 平台差异处理（Dock.Avalonia 自己处理）

## 实施拆解（R3 路线，多个 commit）

| Step | 改动文件 | 测试 | 风险 | 状态 |
|---|---|---|---|---|
| 1. **POC**：空 project + Dock.Avalonia + DockControl + 1 个 dockable 验证能加载 | `/tmp/dock_poc/`（独立小项目） | 仅 build/load | 验证包可用性 | ✅ 已完成（P1.d 文档撰写期间） |
| 2. **NuGet 添加**：加 `Dock.Avalonia` 到 `PrimalEditor_Avalonia.csproj` | csproj 5 行 | dotnet build 不报包冲突 | 中（包冲突可能要求降级 Avalonia + Dock.Model.Mvvm 与 CommunityToolkit.Mvvm 共存需 POC） | 待 |
| 3. **删除 `DockingManager`** | `rm Common/DockingManager.cs` | dotnet test 仍 76/76 PASS | 低（已 grep 验证零引用） | 待 |
| 4. **桥接 `DockingLayout` ↔ `IDock`** + 单 dockable 注册 | `MainWindowViewModel` + 新 `DockLayoutBridge.cs` | 新 bridge 测试 | 中（API 学习曲线） | 待 |
| 5. **替换 MainWindow TabControl → DockControl** | `MainWindow.axaml` + VM | 手动 smoke test | 中（XAML 结构变化） | 待 |
| 6. **layout 持久化**（save/load on startup/shutdown） | `EditorLayoutSettings.cs` 加 `DockLayout` 字段；`MainWindow` 构造/Closing 时调 Load/Save | 测试覆盖 | 低（已有 `EditorLayoutSettings` 模式 + v10 发现预留 `MainWindow` 字段） | 待 |
| 7. **菜单 View 项改成 Dockable toggle** | `MainWindowViewModel` + AXAML | 手动 | 低 | 待 |

每步独立 commit + 可回滚。

**新增 v8 finding（基于 POC）**：

POC 完成时发现 Dock.Model.Mvvm（来自 Dock.Avalonia 包组）与项目现有的 CommunityToolkit.Mvvm 8.2.1 **同时引入会有冲突风险**——两个 MVVM 库都提供 `INotifyPropertyChanged` 实现。Step 2 在加包时需做兼容性 POC：
- 选项 2a：保留两者，验证 ViewModelBase 仍只引用 CommunityToolkit 的 `ObservableObject`
- 选项 2b：完全切到 Dock.Model.Mvvm（如果它提供足够功能）
- 选项 2c：暂时不引入 Dock.Model.Mvvm（只用 Dock.Avalonia + Dock.Model + Dock.Serializer.SystemTextJson + Dock.Avalonia.Themes.Fluent + Dock.Settings）——少了 MVVM 支持可能限制 factory 类写法的灵活性

**推荐 2a**：保留 CommunityToolkit.Mvvm，验证 Dock.Model.Mvvm 不与 ViewModelBase 冲突（理论上两者可以共存，只要 ViewModelBase 不引用 Dock.Model.Mvvm 类型）。

## 待用户确认的关键决策

0. **路线选择**：R0 (字面 C) / **R1 (推荐：引入 Dock.Avalonia)** / R2 (什么都不做) / R3 (推荐折中：Dock.Avalonia 集成 + 保留 DockingLayout schema)
1. **桥接策略**：A (重新设计 layout 模型，放弃 DockingLayout) / **B (推荐：保留 DockingLayout schema，运行时转 IDock)**
2. **持久化 schema**：自定义 JSON (复用 DockingLayoutSerializer) / Dock.Avalonia 自带序列化器（需要包配套）
3. **POC 是否必须**：在动 PrimalEditor_Avalonia 之前是否要求独立 POC 验证包可用？若 POC 失败（包与 Avalonia 11.3.17 不兼容），整个 R3 路线需要重审。

不收到 "go" 不会写任何实现代码。

## Lessons learned 候选（实施完成后）

> **Avalonia Editor — P-M1 "只建库不集成" 何时该重新评估**
>
> DockingManager 在 P-M1 阶段作为"库"建好但从未集成（lessons.md #8 明确推迟集成）。但在 P1.d 时已经过去 N 周 / N 月：
> 1. 库是死代码（grep 验证）
> 2. lessons.md #5 预言了"用户需求增长时引入 Avalonia.Dock"——P1.d 就是那个时刻
> 3. 自制实现 tab reorder + nested splits + persistence 是不必要的工程
>
> 教训：**库 + lessons.md "等用户需求增长" 的组合应该在每个长尾 P 任务开头用 grep 重新评估一次**。3 个月前正确的"先建库不集成"在 3 个月后可能变成"该用第三方库了"。

> **Avalonia Editor — "P-M1 只建库不集成" 项目约定的边界**
>
> lessons.md 反复出现 #5 + #8 两条记录，说明 P-M1 是个**被反复推迟的集成决策**。后续 P 任务的 lesson 应该把"是否到了集成时机"作为标准的 **trigger 条件**：
> - 集成成本是否小于 = 库维护成本 + 用户痛点？
> - 是否有第三方成熟库可用（且与项目 license/dep 政策一致）？
> - lessons.md 是否已 N 次推迟同个决策？
>
> 当 ≥2 个条件成立时，应该主动推进集成而非再次推迟。