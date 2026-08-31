# PrimalEditor Avalonia — P1 短期核心补全

> **For Claude:** P0/P1/P2/P2-补全/P3 全部完成后，用户选 P1（短期高优先级 3 项）。
> 不替代 `2026-07-28-p3-editor-completion.md` / `2026-07-28-p2-editor-completion.md` 等既有计划。
> 集中解决"未真正闭合的 stub 路径"。

## 现状（基于代码阅读）

### A. Gizmo 真实拖动
- `Views/Engine3D/GizmoOverlay.cs` 已建：3 模式（Translate/Rotate/Scale）+ X/Y/Z 屏幕 delta → `OnDelta(axis, delta)` event
- `EngineViewport3D.GizmoDrag` event 暴露但 **未订阅**
- `WorldEditorViewModel.SetEntityTransform` 通过 `NativeEngineBridge.SetEntityTransform` 已可用
- **缺**：WorldEditorView 订阅 `GizmoDrag` → 转世界坐标 → `SetEntityTransform`

### B. 启动重开 windows
- `EditorLayoutSettings.OpenAssets` 列表已存
- `MainWindow` 启动时**未读** `OpenAssets` 自动重开
- **缺**：MainWindow.OnInitialized 调 `EditorLayoutSettings.Load` + 遍历 `OpenAssets` → `AssetEditorRegistry.Open`

### C. Build/Run/错误跳转
- `BuildService.cs` 已实现：异步 `Process.Start(bash, [script, config])` + stdout 重定向
- `WorldEditorView` 工具栏 Build/Stop 按钮
- **缺**：
  - Run 按钮（spawn `PrimalEditor.app` / `PrimalEditor.exe`）
  - Rebuild（clean + build）
  - 错误跳转（解析 build stdout 的 `error: file:line:col: ...` → click 跳转）

## 目标

1. **A. Gizmo 真实拖动** — X/Y/Z translate 屏幕 delta 投影到 world axis delta；WorldEditorView 订阅 → 调 `SetEntityTransform` 修改 selected entity
2. **B. 启动重开 windows** — 启动时根据 `EditorLayoutSettings.OpenAssets` 列表自动重开所有 asset editor（带 .meta sidecar 复用）
3. **C. Build/Run 完整**
   - Run 按钮（macOS `open` .app，Windows `start` .exe，Linux 调 dylib 路径）
   - Rebuild 按钮（先 clean → build）
   - 错误跳转 — 点击 build log 文本，识别 `file:line:col:` 模式 → 解析路径 → 触发 `NativeEngineBridge.OpenInEditor(path, line, col)`（native 暴露）

## 设计

### A. Gizmo unproject 拖动

数学：屏幕 delta（像素）→ 世界 axis delta。

- 已转 world space 中心 G = (target.x, target.y, target.z)
- camera basis 已用：`forward = (-sin(yaw)cos(pitch), -sin(pitch), -cos(yaw)cos(pitch))`; `right = normalize(forward × up)`; `up = right × forward`
- 屏幕 X drag delta → `axis_delta_screen_x_pixels`
- 转换：**世界 X-axis delta = `axis_delta_screen_x_pixels / focal * distance`**（透视投影的标量）
- **世界 Y-axis delta = `-axis_delta_screen_y_pixels / focal * distance`**（Y 翻转：屏幕 Y 向下 = world Y 向上）
- **世界 Z-axis delta**：与 X 同理但 axis 是 `right` 而非世界 X

简化：GizmoOverlay.OnPointerMoved 输出 **normalized delta**（已经按 `1/length` 缩放），WorldEditorView 接收后乘以 `distance` 转 world 单位。

**Translate Gizmo**：
- X 手柄：onDelta(axis=X, delta) → world dx = delta * distance
- Y 手柄：onDelta(axis=Y, delta) → world dy = -delta * distance (Y 翻转)
- Z 手柄：onDelta(axis=Z, delta) → world dz = delta * distance

**Rotate Gizmo**（本轮不做 — 留后续）：
- onDelta(axis, delta) → 角度 = delta（已是弧度）
- 用 `SetEntityRotation` (native 端 Euler 旋转) — **不实现**

**Scale Gizmo**（本轮不做）：
- onDelta(axis, ratio) → scale *= ratio — **不实现**

**本轮只完成 Translate (GizmoMode.Translate)**，Rotate/Scale 留 P+1。GizmoOverlay.Mode 切 W/E/R 键已实现，但消费端只接 Translate。

### B. 启动重开 windows

```csharp
// MainWindow.OnInitialized (or similar)
var layout = EditorLayoutSettings.Load();
foreach (var path in layout.OpenAssets)
{
    if (!File.Exists(path)) continue;
    var info = AssetRegistry.Get(path) ?? new AssetInfo { FullPath = path, Type = ContentHelper.GetAssetType(path) };
    AssetEditorRegistry.Open(info);
}
```

风险：启动时若多个 asset 集中打开，window 叠加难管理。**简化**：保留现有行为（每个独立 Window），但让 P3-14 的 `WindowState` 复原坐标（已在 `Open` 时通过 `WindowState?` 参数）— 但当前 `AssetEditorRegistry.Open` 不接 `WindowState` 参数。**修**：`AssetEditorRegistry.Open(info, windowState = null)` — 查 `EditorLayoutSettings.AssetWindows[info.FullPath]` 自动用。

### C. Build/Run/错误跳转

```csharp
// BuildService — add Run方法
public static bool StartApp(string config = "DebugEditor") {
    var path = LocateAppBundle(config);
    if (path == null) return false;
    Process.Start(new ProcessStartInfo { FileName = path, UseShellExecute = true });
    return true;
}
```

错误跳转：build log 文本中匹配正则 `(?<filename>.+?):(\d+):(\d+):\s*error:` → 调 `OpenInEditor(filename, line, col)`（native 暴露）。

但 **当前 native 未暴露 OpenInEditor**。**简化**：
- log 行有 `error:` 前缀 → 用户双击 log → 弹 dialog 显示 "file:line:col" + "Open in editor" 按钮（但 OpenInEditor **不实现** — 留 TODO）
- 本轮只做：双击 log 行的解析 + 复制到剪贴板 + log 弹窗

**实际方案**（最小）：
- TextBox `MouseDoubleClick` 事件 → 解析 selection 文本 → 弹 MessageBox-like dialog 显示 file/line/col

## 文件清单

### 新建
- （无，P1 在既有基础设施上扩展）

### 修改
- `PrimalEditor_Avalonia/Common/BuildService.cs` — 加 `StartApp(config)` + `StartRebuild(config)`
- `PrimalEditor_Avalonia/build/build_engine.sh` — 加 `rebuild` 子命令（clean + build）
- `PrimalEditor_Avalonia/build/build_run.sh`（新）— platform-aware app launcher
- `PrimalEditor_Avalonia/Editors/AssetEditorRegistry.cs` — `Open(info, windowState?)` 自动查 `EditorLayoutSettings.AssetWindows`
- `PrimalEditor_Avalonia/Views/MainWindow.axaml.cs` — 启动重开（订阅 `DataContext.Project` setter 或 OnLoaded）
- `PrimalEditor_Avalonia/Views/WorldEditorView.axaml` — Run + Rebuild 按钮 + log textarea `MouseDoubleClick` handler
- `PrimalEditor_Avalonia/Views/WorldEditorView.axaml.cs` — Run/Rebuild handlers + log double-click parser

## 验证

- `dotnet build` 0 错误 0 警告
- 单元自测：
  - Gizmo unproject math: 给定 `(yaw=0, pitch=0, distance=10)`, X 拖 5 像素 → world dx = 5/300*10 = 0.167
  - AssetEditorRegistry 重开: 列表中有 3 个路径，启动后有 3 个 OpenHost
  - Run: 不实际启动 .app（环境无 GUI），只测 BuildService.StartApp 返回路径正确

## 边界

- **不**实现 Rotate/Scale Gizmo 真实拖动（仅 Translate）
- **不**实现 Build 错误 native 跳转（仅 log 解析 + 显示）
- **不**实现 Rebuild 增量检测（每次 Rebuild 强制 clean + full build）
- **不**改 native 端（既不暴露 OpenInEditor 也不加新 API）

## 实施顺序

1. P1-A: Gizmo unproject + WorldEditorView 接线
2. P1-B: MainWindow 启动重开 + AssetEditorRegistry 传 WindowState
3. P1-C: Build/Run/Rebuild + log 错误跳转（占位）

每步完成后增量 build（不应破坏 0 错误 0 警告 baseline）。
