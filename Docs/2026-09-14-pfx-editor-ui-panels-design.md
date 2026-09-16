# PFX 编辑器 UI 三面板设计 —— 发射器浏览器 / 发射器编辑器 / 场预览标签页

- 日期：2026-09-14
- 上游：`Docs/2026-09-12-particle-framework-architecture.md` §4/§6/§7-N1（编辑归属决议：文档在 UI、schema 反射在引擎、引擎不做结构性编辑导出）
- 基础设施：Avalonia 12.1 + Dock.Avalonia 布局 + 既有 `EmitterCurveEditor` 自绘曲线控件 + `PropertiesView.axaml` 嵌入式属性面板模式
- 本文档回答：三个专用 PFX 编辑器面板的 AXAML/ViewModel 架构、数据流协议、每面板的任务拆解与门禁

---

## 0. 一页结论

| 面板 | 功能 | 打开方式 | 数据流 |
|---|---|---|---|
| **Emitter Browser** | 项目内 `.pfx_emitter` 资产列表，新建/复制/删除/重命名 | Dock 可停靠面板（菜单 F8 切换） | 文件系统扫描 → ObservableCollection |
| **Emitter Editor** | 模块堆栈三列视图（spawn/init/update）+ 参数编辑区 + 曲线控件嵌入 | 双击资产或 Inspector "Edit…" 按钮 → 独立 Window | `PfxGetEmitterSchema` 反射 → 动态表单 → `PfxEmitterReloadFromMemory` 热预览 |
| **Field Preview** | 视口叠加：KillVolume 线框 / SDF 等值面切片 / 力场箭头 | Inspector "Show Fields" ToggleButton | `SimulationDebugDraw` 适配（走 debug_draw::queue 零管线改动） |

三面板共享一个 **PfxEditorService**（Singleton）：持有当前编辑中的 `emitter_program` 副本 + schema 缓存 + 热预览推送。

---

## 1. 数据流架构

```
┌─ third_party/Alembic ─────────────────────────────────────────────────────────┐
│                                                                               │
│  EngineDLL（C ABI）                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐   │
│  │ PfxGetEmitterSchema()       → JSON schema（模块/参数/提示）              │   │
│  │ PfxEmitterCreateFromAsset() → 打开归档编译                              │   │
│  │ PfxEmitterReloadFromMemory()→ 热预览（编辑器推 JSON 全文重编译）           │   │
│  │ PfxSetExposedParam()        → 运行期参数覆盖                            │   │
│  │ PfxGetEmitterStats()        → 活粒数/预算                              │   │
│  └───────────────────────────────────────────────────────────────────────┘   │
└───────────────────────────────────────────────────────────────────────────────┘
         │ schema JSON                 │ 文件读写                    │ 热预览推送
         ▼                             ▼                            ▼
┌─ PfxEditorService（C# Singleton）─────────────────────────────────────────────┐
│  SchemaCache: PfxSchema（反序列化的模块/参数元数据，启动时拉取一次）               │
│  CurrentProgram: PfxProgramViewModel（编辑中副本）                               │
│  AssetList: ObservableCollection<PfxAssetEntry>（文件系统扫描）                  │
│                                                                                │
│  方法：                                                                        │
│    LoadSchemaAsync()          → 调 PfxGetEmitterSchema → 反序列化 → 缓存        │
│    OpenAssetAsync(path)       → 读文件 → 构造 ViewModel 树 → CurrentProgram 赋值 │
│    SaveAssetAsync()           → 序列化 ViewModel 树 → 写 .pfx_emitter 文件       │
│    PushHotPreviewAsync()      → 读文件全文 → PfxEmitterReloadFromMemory          │
│    BrowseDirectory(path)      → 扫描 *.pfx_emitter → AssetList                  │
│                                                                                │
│  事件：                                                                        │
│    ProgramChanged    → 发射器编辑器刷新                                         │
│    AssetListChanged  → 浏览器刷新                                              │
│    HotPreviewPushed  → 视口触发 InvalidateVisual()                              │
└───────────────────────────────────────────────────────────────────────────────┘
         │ INotifyPropertyChanged            │ 文件 I/O                │ P/Invoke
         ▼                                   ▼                         ▼
┌─ AXAML 视图层 ────────────────────────────────────────────────────────────────┐
│  EmitterBrowserPanel.axaml    （ListBox + ContextMenu）                        │
│  EmitterEditorWindow.axaml    （三列 Grid + ItemsControl + 曲线控件嵌入）        │
│  FieldPreviewOverlay.axaml    （Canvas 叠加层，绑定 debug_draw 队列）             │
└───────────────────────────────────────────────────────────────────────────────┘
```

**三权分立决议的 UI 落地**：

| 权 | 归属 | 本文档实现 |
|---|---|---|
| 文档（JSON 内容） | ViewModel（`PfxProgramViewModel`） | C# 类树 ↔ JSON 序列化/反序列化，UndoRedo 走文档模型 |
| schema（合法模块/参数定义） | 引擎（`PfxGetEmitterSchema`） | 启动时拉取一次，缓存到 `PfxSchema`；表单按 schema 动态生成 |
| 运行时状态（暴露参数值） | 组件属性导出 | `PfxSetExposedParam`（已实现） |

---

## 2. 面板一：Emitter Browser（发射器浏览器）

### 2.1 功能

- 列出项目目录下所有 `.pfx_emitter` 文件（含子目录）
- 双击 → 在 Emitter Editor 中打开
- 右键 ContextMenu：新建 / 复制 / 重命名 / 删除
- 搜索框过滤
- 底部状态栏：资产数 + 当前选中

### 2.2 AXAML 布局

```xml
<!-- EmitterBrowserPanel.axaml -->
<UserControl xmlns="https://github.com/avaloniaui">
  <DockPanel Margin="8">
    <!-- 顶部工具栏 -->
    <StackPanel DockPanel.Dock="Top" Orientation="Horizontal" Spacing="4">
      <Button Content="＋" Command="{Binding NewAssetCommand}" ToolTip.Tip="新建发射器"/>
      <Button Content="📋" Command="{Binding DuplicateCommand}" ToolTip.Tip="复制选中"/>
      <Button Content="🗑" Command="{Binding DeleteCommand}" ToolTip.Tip="删除选中"/>
      <TextBox Watermark="搜索…" Text="{Binding FilterText, Mode=TwoWay}" Width="120"/>
    </StackPanel>

    <!-- 资产列表 -->
    <ListBox ItemsSource="{Binding FilteredAssets}"
             SelectedItem="{Binding SelectedAsset, Mode=TwoWay}"
             DoubleTapped="OnAssetDoubleTapped">
      <ListBox.ItemTemplate>
        <DataTemplate>
          <TextBlock Text="{Binding DisplayName}" ToolTip.Tip="{Binding FullPath}"/>
        </DataTemplate>
      </ListBox.ItemTemplate>
    </ListBox>
  </DockPanel>
</UserControl>
```

### 2.3 ViewModel

```csharp
// PfxBrowserPanelViewModel.cs
public sealed class PfxBrowserPanelViewModel : ViewModelBase
{
    public ObservableCollection<PfxAssetEntry> FilteredAssets { get; }
    public PfxAssetEntry? SelectedAsset { get; set; }
    public string FilterText { get; set; }

    public ICommand NewAssetCommand { get; }       // → 模板 JSON → SaveFileDialog
    public ICommand DuplicateCommand { get; }      // → File.Copy + 刷新列表
    public ICommand DeleteCommand { get; }         // → File.Delete + 刷新列表
    public event Action<string>? AssetOpenRequested; // → 双击 → 通知主窗口打开编辑器
}
```

### 2.4 接入方式

- Dock.Avalonia 注册为可停靠面板（`IDockable`），菜单 F8 切换显示/隐藏
- `AssetOpenRequested` 事件 → `PfxEditorService.OpenInEditor(path)` → 打开 Emitter Editor Window

---

## 3. 面板二：Emitter Editor（发射器编辑器）—— 核心面板

### 3.1 功能

三列 Grid 布局（左：阶段树，中：模块列表，右：参数编辑区）+ 底部曲线控件嵌入。

```
┌──────────────────────────────────────────────────────────────────────┐
│  ToolBar: [Save] [Hot Preview ✓] [Verify] [Export…] [Asset Path]     │
├────────────┬─────────────────────┬───────────────────────────────────┤
│  Stage     │  Module List        │  Module Parameters                │
│            │                     │                                   │
│  ▼ Spawn   │  ┌─────────────────┐│  Module: SpawnRate               │
│    SpawnRate│ │  SpawnRate      ││                                   │
│    SpawnBurst│ │  SpawnBurst    ││  Rate: [====●====] 200.0         │
│            │  └─────────────────┘│                                   │
│  ▶ Init    │  [+ Add Module ▾]   │  (按 schema hints 动态生成)        │
│  ▶ Update  │                     │                                   │
│  ▶ Events  │                     │  ┌── Curve Editor ────────────┐  │
│            │                     │  │  EmitterCurveEditor 控件    │  │
│            │                     │  └────────────────────────────┘  │
├────────────┴─────────────────────┴───────────────────────────────────┤
│  Status: 3 modules, 10000 capacity, 500 alive                        │
└──────────────────────────────────────────────────────────────────────┘
```

### 3.2 模块列表（左+中列）

- `TreeView` 或三列 `ListBox`：按阶段分组
- 每个模块项显示：模块名 + 启用/禁用 Toggle
- 选中模块 → 右侧参数面板动态刷新
- 拖拽重排序（v2，v1 用上移/下移按钮）

### 3.3 参数编辑区（右列）

**动态表单生成规则**（schema 驱动，无硬编码模块知识）：

| param_hint.type | AXAML 控件 |
|---|---|
| `f32` | TextBox（数值校验） |
| `u32` | TextBox（整数校验） |
| `v3` | 三个 TextBox（X/Y/Z） |
| `v4` | 四个 TextBox（RGBA） |
| `enum` | ComboBox（schema 提供可选值列表） |
| `bool` | CheckBox |
| `curve` | 内嵌 `EmitterCurveEditor` 控件（复用） |
| `param_ref` | TextBox（绑定名）+ 可选数值（如果用常量覆盖） |
| `string` | TextBox |

**schema 反射数据结构**（C# 侧）：

```csharp
public class PfxModuleSchema
{
    public string Name { get; set; }
    public string Stage { get; set; }
    public List<PfxParamHint> Params { get; set; }
}

public class PfxParamHint
{
    public string Name { get; set; }
    public string Type { get; set; }   // "f32"|"u32"|"v3"|"curve"|"enum"|...
    public string Note { get; set; }
    public List<string>? EnumValues { get; set; }   // enum 类型时
}
```

由 `PfxGetEmitterSchema()` 返回的 JSON 反序列化。**C# 不硬编码任何模块名或参数名**。

### 3.4 曲线控件嵌入

- 当 `param_hint.type == "curve"` 时，右侧参数面板底部嵌入 `EmitterCurveEditor` 控件
- `EmitterCurveEditor` 是自绘 Control（已在 legacy 编辑器中使用），支持拖键帧/加键/删键/切柄
- 绑定到 `PfxCurveViewModel`（封装键帧列表 + 增删改事件）
- 编辑即写回模块参数 JSON（防抖 300ms → `PfxEmitterReloadFromMemory` 热预览）

### 3.5 热预览

```
用户编辑参数 → 300ms 防抖 → 序列化 JSON 全文 → PfxEmitterReloadFromMemory(id, json, len)
→ 引擎重编译 + 重启发射器 → PipelineTick 驱动 → 视口粒子立即更新
```

**撤销/重做**：纯 UI 侧文档模型——每次编辑快照 JSON 文本入 UndoStack，Ctrl+Z 恢复上一快照并重新推送。不跨 ABI。

### 3.6 保存

`Ctrl+S` / Save 按钮 → 序列化 `PfxProgramViewModel` → 写 `.pfx_emitter` 文件 → 标脏清除。

---

## 4. 面板三：Field Preview（场预览标签页）

### 4.1 功能

在视口叠加显示场系统边界/力场的可视化调试线，由 `SimulationDebugDraw` 的 debug_draw 队列驱动（零管线改动，线框写入 `debug_draw::queue` 即可）。

### 4.2 三种可视化

| 场类型 | 可视化 | 实现 |
|---|---|---|
| KillVolume（AABB/球） | 线框盒/球（`debug_draw::DrawWireBox` / `DrawWireSphere`） | emitter_program 的 KillVolume 模块参数直读 |
| SDF 场 | 等值面切片（网格面片沿轴向切片，着色=距离值） | `volume_grid` 的 active bbox 切片数据 → `debug_draw::DrawLine` 网格 |
| 力场 | 力场箭头（在采样网格上画方向箭头，长度∝强度） | `AdvectByField`/`CurlNoiseForce` 的场采样 → `DrawArrow` |

### 4.3 接入方式

- Inspector 面板加 "Show Fields" ToggleButton（绑定 `ParticleFXComponent.ShowFieldDebug`）
- 开启时每帧向 `debug_draw::queue` 写线段（用现有的 `SimulationDebugDraw` 适配器模式）
- 引擎渲染管线末尾自动消费 debug_draw 队列渲染线框（已有基础设施）

### 4.4 ViewModel

```csharp
public class FieldPreviewViewModel : ViewModelBase
{
    public bool ShowKillVolumes { get; set; }
    public bool ShowForceFields { get; set; }
    public bool ShowSdfSlice { get; set; }
    public float SdfSliceHeight { get; set; }   // SDF 切片的 Y 高度
    public float ArrowDensity { get; set; }     // 力场箭头采样密度
}
```

---

## 5. 文件清单与职责

```
PrimalEditor_Avalonia/
  Views/PFX/
    EmitterBrowserPanel.axaml(.cs)         — 浏览器面板（Dock 可停靠）
    EmitterEditorWindow.axaml(.cs)         — 发射器编辑器窗口
    FieldPreviewOverlay.axaml(.cs)         — 场预览叠加层
  ViewModels/PFX/
    PfxBrowserPanelViewModel.cs            — 浏览器面板 VM
    PfxEditorWindowViewModel.cs            — 编辑器窗口 VM
    PfxModuleViewModel.cs                  — 单模块行 VM（参数编辑状态）
    PfxFieldPreviewViewModel.cs            — 场预览 VM
  Models/PFX/
    PfxSchema.cs                           — schema JSON 反序列化模型
    PfxProgramModel.cs                     — 发射器程序 C# 镜像（编辑器持有）
    PfxAssetEntry.cs                       — 浏览器资产行
  Controls/
    PfxCurveEditor.cs                      — 曲线控件（复用 EmitterCurveEditor 或包装）
  Services/
    PfxEditorService.cs                    — Singleton：schema 缓存 + 资产列表 + 热预览推送
```

---

## 6. 里程碑

### R1：Emitter Browser + 基础编辑器框架（~5-6 人日）

- `PfxEditorService`（schema 拉取/缓存 + 资产扫描 + 热预览推送）
- Emitter Browser 面板（列表 + 新建/删除 + 双击打开）
- Emitter Editor 窗口骨架（三列布局 + 模块列表 + 动态表单框架）
- 新增/删除/保存 走文件 I/O

**门禁**：新建 → 编辑参数 → 保存 → 重新打开内容一致；`pfx_converter --verify` 通过。

### R2：动态表单 + 曲线控件嵌入（~6-8 人日）

- 按 schema `param_hint` 动态生成控件（f32/u32/v3/enum/bool/curve/param_ref）
- 曲线参数嵌入 `EmitterCurveEditor` 控件
- 热预览防抖 + Undo/Redo（JSON 快照栈）
- ColorOverLife 的 RGBA gradient 控件（色标条 + 拖控制点）

**门禁**：编辑 SpawnRate → 热预览粒子数变化；编辑曲线 → 视口观感变化；Undo/Redo 往返一致。

### R3：Field Preview + 完整集成（~5-7 人日）

- KillVolume 线框叠加（debug_draw）
- SDF 等值面切片
- 力场箭头
- 资产浏览器 → 编辑器 → 视口全链路闭环
- pfx_converter 接入 CLI（`primal-engine pfx-convert` verb）

**门禁**：Sponza 场景开启 Field Preview → KillVolume 线框与粒子碰撞边界视觉匹配；关闭后消失。

---

## 7. 总工作量估算

| 面板 | 人日 |
|---|---|
| Emitter Browser | 3-4 |
| Emitter Editor（核心） | 8-10 |
| Field Preview | 4-5 |
| 共享基础设施（Service/Schema/序列化） | 3-4 |
| **合计** | **18-23 人日** |

---

## 8. 风险与缓解

| 风险 | 缓解 |
|---|---|
| schema JSON 反序列化失败（引擎导出格式变化） | schema 结构版本号 + C# 侧容错解析（缺字段用默认值）+ 集成测试 |
| 热预览推送延迟导致视口卡顿 | 300ms 防抖 + 后台线程编译 + UI 线程仅提交结果 |
| 曲线控件复用兼容性 | `EmitterCurveEditor` 是独立 Control，只需适配数据模型转换层 |
| Dock.Avalonia 面板注册冲突 | 沿用 Sequencer Panel 的 F-key 切换模式，避免 Dock 内嵌 |

---

## 9. 非目标

- 节点图/图表编辑器（连接式模块图）
- 实时粒子预览（视口内拖动参数滑块看到即时变化——当前通过热预览实现，但非逐帧实时）
- GPU 模拟参数编辑（N5 延后）
- 多选发射器批量编辑
