# PrimalEditor Avalonia — P3-14 Asset Editor 通用体验

> **For Claude:** P0/P1/P2 完成后，按 P0 计划 P3-14 把 Asset Editor 的使用体验升级到 production 级别。
> 通用体验改进：跨会话布局记忆、Dirty 标识 + 快捷键、状态栏、最近打开列表、deduplicate。
> 范围明确：仅 AssetEditor 体系，**不**改 main window / menu / project 体系（属于 P3-15/P3-16）。

## 现状（基于代码阅读）

- `Editors/IAssetEditor.cs` — `AssetEditorState` + `IAssetEditor` + `AssetEditorViewModel` 基类（含 `State`/`Asset`/`Title`/`SetAsset`）
- `Editors/AssetEditorRegistry.cs` — 静态 factory + `_openHosts` 列表；`Open(info)` **每次新建** AssetEditorHost + Window
- `Editors/AssetEditorHost.cs` — `Window` 包装 `AssetEditorViewModel`，固定 Width=900 Height=700
- `Views/ContentBrowserView.axaml.cs:35` — 双击 asset → `AssetEditorRegistry.Open(info)`
- Texture/Geometry Editor 已有 `IDirtyTrackable` 集成（P2 修复）— `IsDirty` 状态可被订阅

## 目标（P0 计划 P3-14 验收）

1. ✅ **窗口恢复**：`AssetEditorHost` 位置/大小跨会话保存 + 恢复
2. ✅ **Docking**：留 P3-15，本轮不做（与 Plan 9 "Build/Run" 一起评估）
3. ✅ **最近打开**：`RecentAssetsManager` 类似 `RecentProjectsManager`，跨会话保存
4. ✅ **Dirty 标识**：窗口标题加 `*` 后缀；`Save` 快捷键 `Ctrl+S`；`Discard` 快捷键 `Ctrl+Shift+S`
5. ✅ **多编辑器共享预览工具栏 + 状态栏**：每个 editor 自带 top toolbar 已有；底部加统一 `StatusBar`（state + asset path + dirty badge），由 `AssetEditorHost` 统一渲染
6. ✅ **去重**：同一 asset 重复打开 → 激活已有 window，不新建

## 设计

### 1. `Common/EditorLayoutSettings.cs`

JSON 文件 `~/.config/PrimalEditorAvalonia/editor-layout.json`（macOS/Linux 同路径）：

```json
{
  "mainWindow": {"x": 100, "y": 100, "w": 1400, "h": 900, "state": "Normal"},
  "openAssets": ["path1.png", "path2.mesh"],
  "assetWindows": {
    "path1.png": {"x": 200, "y": 200, "w": 900, "h": 700, "state": "Normal"},
    "path2.mesh": {"x": 300, "y": 300, "w": 900, "h": 700, "state": "Normal"}
  }
}
```

API：
```csharp
public static class EditorLayoutSettings
{
    public static EditorLayout Load();   // returns defaults if file missing
    public static void Save(EditorLayout layout);
    public static string ConfigPath { get; }
}

public sealed class EditorLayout
{
    public WindowState MainWindow { get; set; } = new();
    public List<string> OpenAssets { get; set; } = new();
    public Dictionary<string, WindowState> AssetWindows { get; set; } = new();
}

public sealed class WindowState
{
    public int X, Y, W, H;
    public int State; // 0=Normal 1=Maximized (avoids enum dependency in serialization)
}
```

### 2. `Common/RecentAssetsManager.cs`

类似 `RecentProjectsManager`：`Add(path)`, `RecentPaths` (max 10), `Save` / `Load` 路径 `~/.config/PrimalEditorAvalonia/recent-assets.json`。

ContentBrowserView 启动时显示 Recent（folder 为空时）— 简化为**仅 Save/Load** 供 future 显示用；本轮不集成 UI（避免范围扩散）。

### 3. `AssetEditorRegistry` 改造

`Open(info)`:
- 在 `_openHosts` 找 `vm.Asset.FullPath == info.FullPath`
- 命中：调 `host.Activate()`（Avalonia 11 Window.Activate 抢焦点），return existing
- 未命中：现有逻辑

`Close(host)`：从 `_openHosts` 移除（防 leak）。

### 4. `AssetEditorHost` 改造

- 构造时接收 `windowState: WindowState?`（null 用默认 900x700）
- 监听 `vm.PropertyChanged("IsDirty")` → 标题更新 `Title = vm.Title + (IsDirty ? " *" : "") + " — PrimalEditor"`
- `KeyBindings`:
  - `Ctrl+S` → `vm` 上有 `Save()` 调它
  - `Ctrl+Shift+S` → `vm` 上有 `Discard()` 调它
  - `Ctrl+W` → 关闭
- 关闭时 `OnClosing` 检查 dirty → ConfirmDialog → Save / Discard / Cancel
- 位置/大小变化 → 调度 `SaveLayoutAsync` 节流（500ms）

### 5. `AssetEditorStatusBar` 共享控件

`Editors/AssetEditorStatusBar.cs`（UserControl）：
- 左侧：asset 全路径
- 中间：state badge（Done/Loading/Importing/...）
- 右侧：dirty 标记（* 红色）
- 数据绑定到 `AssetEditorViewModel`

`AssetEditorHost.BuildContent` 用 `DockPanel`：内容 + 底部 status bar。

### 6. `ContentBrowserView` 启动恢复

`ContentBrowserView.OnDataContextChanged`:
- 调 `EditorLayoutSettings.Load()` → 拿上次 `OpenAssets` 列表
- **本轮不自动重开**（避免启动时打开 10 个窗口）。仅确保 `Open` API 正确去重，留作 future "Reopen Last Session" 菜单。

## 实施顺序

1. 基础设施：`EditorLayoutSettings.cs` + `RecentAssetsManager.cs` + JSON 读写
2. `AssetEditorStatusBar.cs` 共享控件
3. `AssetEditorRegistry` 去重 + 关闭清理
4. `AssetEditorHost` dirty 标题 + 快捷键 + 关闭确认 + 布局记忆
5. 验证：编译 + lessons 追加

## 验证

- `dotnet build` 0 错误 0 新增 warning
- 单元自测：`EditorLayoutSettings.Roundtrip`、`RecentAssetsManager.Cap10`、`AssetEditorRegistry.OpenDedup`
- 视觉/运行时：smoke test 与 P0/P2 一样受 `avares:` URI 已知问题影响，不预期本轮修复

## 风险

- `Window.Activate()` 在 Avalonia 11 可能需要先 `Show()`——已 show 后再 Activate 抢焦点
- `OnClosing` 取消 close 与 dirty 冲突：复用 `MainWindow.OnClosing` 模式（`e.Cancel = true` 然后异步决策再 `Close()`）
- 跨平台路径：`Environment.SpecialFolder.ApplicationData` 在 macOS = `~/Library/Application Support`，Linux = `~/.config`，Windows = `%AppData%`——一致
