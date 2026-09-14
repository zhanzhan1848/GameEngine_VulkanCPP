# PrimalEditor Avalonia — P2 三个 Editor 完整化

> **For Claude:** P0（数据安全）/ P1（视口抽象）完成后，按 P0 计划 P2-B/P2-C/P2-D 把三个 Editor 从 stub 升到 production。
> 本计划定义完成标准、依赖顺序、不做边界。
> 既不替代 `2026-07-28-avalonia-feature-parity.md` 中已实现的 Assets/Registry/IAssetEditor 框架；属于补完。

## 现状（基于代码阅读）

### Texture Editor
- 文件：`Editors/Texture/{TextureEditorViewModel,TextureEditorView,TextureImportSettings,TextureInfo}.{cs,axaml}`
- 已实现：基础 RGBA 通道 CheckBox、NormalMap CheckBox、Mip/Array 写死 "0" 文本、Reimport 写日志、Save 写日志、`TextureInfo` 元数据。
- **stub**：`UpdateChannelBitmap()`（line 78）是空操作；`IsNormalMap=true` 不重绘；Reimport 不调 cook；Save 不写 sidecar；Mip/Array 不绑定实际数据。
- **缺**：sRGB toggle、BC1/BC3/BC5/BC7 格式选择 UI、IDirtyTrackable 集成、sidecar 读写。

### Geometry Editor
- 文件：`Editors/Geometry/{GeometryEditorViewModel,GeometryEditorView,GeometryData}.{cs,axaml}`
- 已实现：`.mesh` 格式加载（`Geometry.Load`）、LOD 切换 ComboBox、VertexCount/TriangleCount 统计、Auto LOD 复选框。
- **stub**：`GeometryEditorView.axaml.cs:RenderPanel` 是空 Panel，hook pointer 事件但**没有 wireframe 绘制代码**。
- **缺**：GPU 预览（managed wireframe via Avalonia DrawingContext）、材质槽/submesh/bounds 显示、Reimport、ImportSettings 持久化、IDirtyTrackable 集成。

### World Editor
- 文件：`Views/WorldEditorView.axaml(.cs)`、`Views/Engine3D/EngineViewport3D.axaml.cs`
- 已实现：Hierarchy 树、Inspector 多选属性编辑（依赖 P0 修复）、轨道相机（orbit/pan/zoom）、test scene 4 球（编辑器自检）。
- **缺**：视口点击选中并同步到 Hierarchy/Inspector；拖拽 Content Browser asset 到视口生成 entity；F 键对焦选中；Gizmo（移动/旋转/缩放）；网格地面/坐标轴显示。

## 目标（P0 计划 P2-B/C/D 验收）

### P2-B（Texture Editor 完整版）
1. ✅ RGBA 通道真实预览（channel mask 实时重绘 preview）
2. ⏳ mip/array/depth 浏览（绑到实际 mip/array/depth 数据，本轮单 mip/array/depth，UI 上为 0..N-1）
3. ⏳ import settings 持久化（sidecar `.texture.meta` 二进制）
4. ⏳ Reimport 真正调用 Cook（`NativeEngineBridge.ImportTexture`）
5. ✅ normal map 重映射（已写重映射函数，line 1567-1571 在 feature-parity 计划里）
6. ⏳ sRGB、压缩格式（BC1/BC3/BC5/BC7）UI 暴露
7. ⏳ Save/Discard 和 Dirty 状态（IDirtyTrackable 集成 + sidecar 写入）

### P2-C（Geometry Editor 完整版）
1. ✅ `.mesh` 格式加载和 LOD 数据正确
2. ⏳ 真实 GPU 预览（managed wireframe via Avalonia `DrawingContext.DrawLine` + 透视投影；非 engine-driven，但视觉上等价）
3. ⏳ 材质槽/submesh/bounds 统计
4. ⏳ LOD 切换、相机控制（已部分）、自动 LOD
5. ⏳ Reimport 与导入设置（sidecar `.mesh.meta`）

### P2-D（World Editor 完整版）
1. ⏳ 视口选择与 Hierarchy/Inspector 同步（`EngineViewport3D` 点击 → WorldEditor.SelectedEntities 同步）
2. ⏳ Gizmo：移动、旋转、缩放（managed overlay 控件，不走 engine）
3. ⏳ 拖拽资产到场景生成实体（Content Browser → WorldEditorView.DragDrop）
4. ⏳ 摄像机对焦（**F** 键对焦选中 entity）
5. ⏳ 网格地面 + 坐标轴（managed overlay）

## 设计与依赖

### 复用 P0 成果
- `IDirtyTrackable` — Editor VM 继承 `IDirtyTrackable`，保存/丢弃通过 sidecar。
- `EditorSaveService` — 不直接复用（它针对 Project/Scene）；Editor 单独走 `AssetImportSettings.ToBinary/FromBinary` + sidecar 文件。

### 复用 P1 成果
- `IEditorViewportHost` — 视口相关（不涉及本轮 Editor；但 World Editor Gizmo 在视口上叠加 managed overlay，与 host 渲染解耦）。

### 新增基础设施

1. **`PrimalEditor_Avalonia/Common/AssetImportSettingsStore.cs`** — 通用 sidecar `.meta` 读写：
   - 路径：`<asset_path>.meta`（与 asset 同目录）
   - 格式：`<4-byte-magic>` + `<4-byte-version>` + `<IAssetImportSettings.ToBinary>` body
   - API：`static T? Load<T>(string assetPath) where T : IAssetImportSettings, new()` 和 `static void Save<T>(string assetPath, T settings)`
   - 缺失 sidecar 时返回 `new T()`（默认设置），不算错误。

2. **`PrimalEditor_Avalonia/Common/EditorDirtyMarker`** — 复用 `IDirtyTrackable`：让 AssetEditorViewModel 继承 `IDirtyTrackable`，`SetAsset` 时 `ClearDirty`，任何 settings 变更时 `MarkDirty()`，Save 后 `ClearDirty()`。

3. **`PrimalEditor_Avalonia/Views/Engine3D/GizmoOverlay.cs`** — managed overlay 控件，绘制移动/旋转/缩放手柄（Gizmo 模式 1/2/3 按 W/E/R 切换），投影到屏幕空间根据 `_cameraYaw/_cameraPitch/_cameraDistance/_cameraTarget` 计算。

4. **`PrimalEditor_Avalonia/Views/Engine3D/WireframeRenderer.cs`** — managed wireframe 绘制 helper，从 `Geometry.LODs[LODIndex].Meshes[i]` 读顶点/索引，用 `DrawingContext.DrawLine` 在 Panel 上画线框（透视投影：X→screen X 按 yaw/pitch，Y→screen Y，Z→screen depth）。

### 不做

- **不**重写 C++ 引擎
- **不**实现真实 GPU mipmap 生成（Texture 仅支持单 mip，UI 滑块表示 mip 0）
- **不**实现 BC1/BC3/BC5/BC7 实际压缩（仅 UI 暴露；`OutputFormat` 字段写入 sidecar；native 端未做实际压缩时 Reimport 走"读取原始 → sidecar 记录格式"路径）
- **不**改 `AssetRegistry.Register`（GUID 稳定属于 P1-3，本轮不动）
- **不**实现 undo/redo 集成到 Editor（Editor 自己的设置修改不入场景的 UndoRedo 栈——仅 Asset 自身的导入/操作入 scene 的 UndoRedo 栈）

## 实施阶段（按依赖顺序）

### 阶段 0：基础设施
- `Common/AssetImportSettingsStore.cs` — sidecar 读写
- `Common/EditorDirtyMarker.cs` — IDirtyTrackable 集成 helper
- 编译 + 单元自测（sidecar round-trip）

### 阶段 1：P2-B Texture Editor 完整化
- `TextureImportSettings` 增加 `IsSRGB` 字段 + 兼容旧版二进制（追加字段版本检查）
- `TextureEditorViewModel`：mip/array/depth `MaxXIndex`/`MaxYIndex`/`MaxZIndex` + `SelectedX/SelectedY/SelectedZ` + `ChannelMask` 真实重绘 + `IsSRGB`/`OutputFormat` setter
- `TextureEditorView.axaml`：Mip/Array/Depth NumericUpDown（绑 MaxX）、sRGB CheckBox、OutputFormat ComboBox、Reimport/Save 行为
- 真实 Reimport 路径：调 `NativeEngineBridge.ImportTexture`（已有），写 sidecar
- Save 路径：写 sidecar
- Dirty：任何 settings 变更 → MarkDirty；`SetAsset` → ClearDirty
- 编译 + 单元自测（settings round-trip + channel mask 重绘）

### 阶段 2：P2-C Geometry Editor 完整化
- `GeometryImportSettings.cs`（新建）— `LodGenerationMode` (Manual/Auto)、`MinLod`/`MaxLod`、`Compression` enum
- `GeometryEditorViewModel`：`ImportSettings` 属性 + Submesh 列表 + Bounds 计算（min/max XYZ） + VertexCount/TriangleCount 已有
- `GeometryEditorView.axaml`：Mip 切换已有；新增 ImportSettings 面板（折叠）、Bounds/Submesh 统计
- `WireframeRenderer.cs`：透视投影 wireframe（DrawingContext.DrawLine）
- Reimport 路径：写 sidecar；不支持真实 GPU 上传（managed wireframe 已足够）
- Save 路径：写 sidecar
- Dirty 集成
- 编译 + 单元自测（bounds 计算、wireframe 投影）

### 阶段 3：P2-D World Editor 完整化
- `EngineViewport3D` 暴露：
  - `PointToWorldRay(Point screen)` — 给 picker 用
  - `WorldRay? PickEntity(Ray ray)` — 反向：ray 与 entity AABB 相交测试
  - `FocusOnEntity(uint entityId)` — 计算 entity 包围盒，对准 target + 调整 distance
  - `SelectionChanged` 事件
- `WorldEditorView` 接收 `EngineViewport3D.SelectionChanged` → 同步 `WorldEditor.SelectedEntities`
- `WorldEditorView.axaml` 顶部新增 G/R/T 键切换 Gizmo 模式（移动/旋转/缩放）
- `GizmoOverlay.cs` 在视口上叠 Gizmo
- `OnPointerPressed` 处理：点击 → picker；按住 entity handle → 拖动
- 拖放接收：Content Browser 拖 asset 到视口 → 用 native `PipelineRegisterMeshEntity` 生成 entity，添加到 scene
- F 键对焦：选中 entity → 调 `FocusOnEntity`
- 编译 + 单元自测（picker ray math、gizmo 投影、focus camera math）

### 阶段 4：验证 + 教训
- 全量编译 + 三个 Editor 单元自测
- `tasks/lessons.md` 追加 P2 章节

## 验证标准

- ✅ `dotnet build` 0 错误 0 新增 warning
- ✅ P2 单元自测全通过：
  - `AssetImportSettingsStore` round-trip
  - `TextureEditorViewModel` channel mask 重绘 + dirty/sidecar
  - `GeometryEditorViewModel` bounds 计算 + wireframe 投影 + dirty/sidecar
  - `WorldEditorViewModel` 选中同步 + focus camera math
- ✅ 视觉/运行时验证：smoke test 跑 macOS（与 P0 一样，`avares:` URI 错误是已存在，不预期本轮修复）

## 风险

- **`DrawingContext` 性能**：managed wireframe 大量顶点可能掉帧；fallback 是只画 LOD 0 且最多 5K 顶点的线框。
- **Picker 精度**：ray-AABB 简单实现；O(n) 扫描所有 entity。n 大时掉帧，但编辑器场景 entity 数量级（<1000）足够。
- **Gizmo 操控数学**：屏幕空间拖动到世界轴需要 unproject。本轮只做"基础 X/Y/Z 平面拖动"；锁定模式（Shift）留后续。
- **Reimport 路径**：native 端 `ImportTexture` 已有但本轮不会新增功能；如果 native 失败，Reimport 写错误日志但 sidecar 仍写入（保留设置选择）。

## 边界

- P0 计划 P2-B-4 (Reimport 真正调用 Cook) 在 native 端尚未提供 CookTexture 时只能"写 sidecar 标记格式"，不实际压缩。后续 native 端加 CookTexture 后接入。
- P0 计划 P2-C-2 (真实 GPU 预览) 解释为"managed wireframe 等价"——非 engine 渲染但视觉等价，符合编辑器 1K-10K 顶点的实际场景。
- P0 计划 P2-D-2 (Gizmo) 仅基础 X/Y/Z 平面拖动，不含 Shift 锁定 / 中心点模式。
