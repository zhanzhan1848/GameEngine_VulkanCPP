# P1.c — 真实多 slice 纹理预览 (Texture editor multi-slice preview)

> **For Claude:** P1.b (Content Browser search + breadcrumb) 已 commit (`063f3ad`)。
> 这是 P1.c 的设计文档，需要用户 review 后再开始实施。
> 用户在 P1 选型阶段选了"C: Native ABI 扩展"，本设计文档回答当初提出的 5 个开放问题。

## 现状（基于代码阅读）

### 两条并存的 cooked texture 路径

仓库内 cooked texture 路径**不是一条**，而是两条独立的 pipeline，作用于不同 ABI：

| 路径 | 入口 | 输出格式 | 支持的元数据 |
|---|---|---|---|
| **Editor import path**（`libContentTools`） | `ContentTools::TextureImporter::Import(texture_data*)` | DirectXTex-flavored blob：每个 subresource 前缀 4×u32 (w, h, rowPitch, slicePitch) + slicePitch 像素字节 | 完整的 array_size / mip_levels / flags (cube/volume/premultiplied) |
| **Engine runtime path**（`libEngineDLL`） | `AsyncResourceLoader::LoadTexturesParallel` 用 `stbi_load` 解 PNG/JPG | 简单 blob：6×u32 header (w, h, array_size=1, flags=0, mip_levels=1, format) + RGBA 像素 | **永远** array_size=1, mip_levels=1 |

`EngineDLL/AssetAPI.cpp::ImportTexture` 是 C# 编辑器实际调用的入口（已通过 `PrimalEngine` dylib P/Invoke），属于第二条 runtime 路径。它返回的 `TextureImportResult` 只有 `width`, `height`, `success`, `error_message`，**没有 array/mip/depth 字段**。

**结论**：即便 `libContentTools` 的 heavy path 支持任意 slice 数，编辑器**永远看不到**多于 1 的 array/mip/depth，因为导入后的资源走的是 runtime 路径，`stbi_load` 只读 2D 单层 PNG/JPG。

### 现状编辑器

- `Editors/Texture/TextureEditorView.axaml` 第 14-19 行已有 `MipDisplay`, `ArrayDisplay`, `DepthDisplay` 三个 TextBlock，但只是显示当前 index（永远是 "0/0"），没有 selector
- `TextureEditorViewModel.cs:139-181` 有 `_mipIndex` / `_arrayIndex` / `_depthIndex` 字段 + setter，但 setter 改了不会触发预览更新（因为底层只持有 top-level slice 的 RGBA）
- `AvaloniaTextureSliceDecoder.TryDecode` (在 `Editors/Texture/ITextureSliceDecoder.cs:48-78`) 固定返回 `ArrayCount=1, MipCount=1, Depth=1`，永远只解码顶层
- `TextureSliceSet` 接受 `(arrayCount, mipCount)` 构造参数但只存一份 RGBA；注释已预告："callers that have real mip chains can construct alternative sets in the future"

## 目标（R3.a 路线）

让 Texture Editor 的 array/mip/depth 控件：

1. **反映用户在 import settings 中设置的值**（来自 `.meta` sidecar）
2. **保持与 PNG/JPG 源一致的预览**（只能看 index 0，PNG 本身不携带 mip/array/depth 信息）
3. **用户切换 slice index 时**：编辑器在状态栏显示 "switched to Mip 2/5 (preview limited to Mip 0 in v1)" 之类的诚实提示，而不是假装切换成功

本质：**让 UI 反映用户意图与 PNG 现实之间的差异**，而不是让 UI 假装一切都是 1×1×1。

## 目标范围审视

用户原选项要求 "C: Native ABI 扩展"。重新审视后，**精简的范围**才是合理的。

> **重要**：R1 和 R2 在 **R3.a 路线下均可省略**——它们的存在前提是"runtime 路径写回 array/mip/depth 给编辑器读"，但 R3.a 是"从 .meta sidecar 读用户设置的值"，根本不走 runtime 结果字段。完整定义保留供参考，**实施时只做 R3.a 实际需要的部分**。

### R1：扩展 `TextureImportResult` ABI 加 array/mip/depth/format（R3.a 路线下可省略）

`EngineDLL/AssetAPI.h::TextureImportResult` 目前 792 字节（实测编译器 sizeof 确认；现有 `static_assert(sizeof == 792)` 通过）。原结构：512 path + 8 handle + 4 width + 4 height + 4 success + 256 error_message = 788 字节，**外加 4 字节尾部填充使总尺寸对齐到 8 字节边界**（因为 u64 handle 把整个 struct 的 alignment 拉到 8）。

增加 4×u32 字段（array_size, mip_levels, depth, format）：
```cpp
struct TextureImportResult {
    char path[512];
    u64 handle;
    u32 width;
    u32 height;
    u32 array_size;
    u32 mip_levels;
    u32 depth;
    u32 format;
    u32 success;       // 保留在末尾以维持 ABI 兼容
    char error_message[256];
};
```
新尺寸计算（实测验证，下方）：512 + 8 + 4×7 + 256 = **808 字节**（其中前 7×4 = 28 字节为 width + height + array_size + mip_levels + depth + format + success）。编译器 sizeof 仍会把整个 struct 向上对齐到 8 字节边界，因为结构里有 u64 字段（alignment = 8）；808 本身已经是 8 的倍数，所以**无尾部填充**。更新 `static_assert` 注释：`(512 path + 8 handle + 28 metadata + 256 err = 808)`。

C# mirror `PrimalEditor_Avalonia/Native/NativeEngineBridge.cs:1217-1228` 同步加 4 个字段（`public uint ArraySize`, `MipLevels`, `Depth`, `Format`），保持 `[StructLayout(LayoutKind.Sequential)]` + `[MarshalAs(UnmanagedType.ByValTStr)]` 顺序不变——Sequential 布局保证字段顺序与 C++ 一致。

**R3.a 不做 R1**：因为 metadata 从 `.meta` sidecar 读，不从 runtime result 读。R1 只在 R3.b（runtime DDS/KTX）路线下需要。

### R2：runtime 路径为 single-slice 2D 返回真实值（R3.a 路线下可省略）

`AsyncResourceLoader.cpp:103-105` 写 blob 时硬编码 `array_size=1, mip_levels=1`。改为读真实 stbi metadata（`stbi_info` 已经在某些路径用过，确认是否暴露 array/mip/depth）：

实际上 **stb_image 不支持 array/mip/depth 概念**（它是单层 PNG/JPG 解码器）。所以：
- **如果是 stbi 加载**：保持 array_size=1, mip_levels=1, depth=1（这就是事实）
- **R2 实际意义**：让 `ImportTexture` 把 `(array_size=1, mip_levels=1, depth=1, format=29)` 写进 result，而当前的实现没有写这些字段。

这是**零工作量**的修复（编辑 AssetAPI.cpp 写 result 那几行），但需要同步 ABI。

**R3.a 不做 R2**：理由同 R1——metadata 从 sidecar 来。

### R3：**取消** —— 编辑器从未产生过 `.texture` 文件

> **重要更正**（实施前发现）：上一版本设计 R3 假设编辑器读 `libContentTools` 写的 cooked `.texture` 文件。**这个假设是错的**。

经过进一步代码勘察：

- `nm Darwin/.../libContentTools.dylib` 显示 `_Import(texture_data*)` 和 `_Decompress(texture_data*)` 都已 export
- 但 C# 侧没有这两个函数的 P/Invoke
- `ContentTools.cpp` 只 export `CookMesh` 和 `ShutDownContentTools`，没有 `CookTexture`
- `ContentTools/main.cpp` 是个独立的 CLI 工具，处理 FBX→mesh，**不处理 texture**
- `PrimalEditor_Avalonia/Native/AssetImporter.cs:88-130` 的 texture import 流程：
  1. `File.Copy(sourcePath, destPath)` — 保留原扩展名（PNG/JPG）
  2. `NativeEngineBridge.ImportTexture(destPath)` — 通过 `libEngineDLL` 的 runtime 路径加载（用 `stbi_load`）
  3. **不写任何 cooked 文件到磁盘** — 资产只存在 GPU handle 中
- 全工作树 `find -name '*.texture'` 返回 0 结果

**结论**：编辑器没有 cooked `.texture` 文件可读。`libContentTools::Import` 这条 heavy path 是给引擎 asset baking pipeline（offline cook tool）用的，**不**是编辑器导入流程用的。R3 设计的 `Editor_GetTextureSlice(path, ...)` 没有任何现存文件可作用。

**R3 修订方案**（需要用户决策，见末尾"待用户确认的关键决策"）：

**R3.a — 诚实 UI 模式**（推荐）：读取 source PNG/JPG + 同伴 `.meta` 文件（已经在 `AssetImportSettingsStore` 中保存）：
- `Bitmap` 只能解码顶层 slice（PNG/JPG 是单层格式），无法解码 mip/array/depth
- 把 `.meta` 里用户设置的 `ArraySize` / `MipLevels` / `Depth` 当作"声明的元数据"，UI 显示 "Array 1/6" 即使实际只有 1 张图
- slice selector 仍然可点（设置 `_arrayIndex` / `_mipIndex` / `_depthIndex`），但切换时预览保持不变（因为只有 index 0 有真实像素）
- **优势**：零 native 改动，纯 C# / XAML
- **劣势**：用户切换到 index > 0 看不到内容差异——但 UI **声明**了 slice 数与用户设置一致，是"honest"
- **典型用户场景**：导入了一张 PNG，editor 设置面板里勾选了 "Generate Mipmaps"，运行时引擎会生成 6 个 mip level。预览窗口显示 "Mip 0/5"，但 UI 不能真正显示 mip 1-5 的内容（PNG 文件本身不包含它们）。这是已知限制，**不是 bug**——运行时引擎会在 GPU 上生成。

**R3.b — 扩展 runtime 路径支持 DDS/KTX**：让 `AsyncResourceLoader` 增加 DDS/KTX2 decoder 支持（添加 DirectXTex 的 CPU 端使用），editor 导入时检测后缀用对应解码器。这是真正的 multi-slice 端到端，但需要：
- 给 `stbi_load` 加 fallback（DDS 用 DirectXTex，KTX 用 libktx）
- `AsyncResourceLoader::LoadTexturesParallel` 重写
- `TextureImportResult` ABI 扩展（仍是 R1 的部分）
- 巨大工作量（数千行 C++ + 多个新依赖），远超"编辑器多 slice 预览"的范围

**R3.c — 不动**：维持现状（`AvaloniaTextureSliceDecoder` 返回 1×1×1），不做 P1.c。用户已经在 settings 面板能设置 array/mip/depth，运行时会按设置处理，预览只在 v1 限制下工作。**这是 honest 的最小化**：承认现有 PNG/JPG→Avalonia `Bitmap` 路径无法提供多 slice preview。

**推荐 R3.a**——工作范围匹配 P1.c 的"编辑器预览多 slice"目标，不越界到引擎 asset pipeline 改造。

### R4：新增 C# binding（R3.a 路线下取消）

在 `NativeEngineBridge.cs` 加 `_Editor_GetTextureMetadata` + `_Editor_GetTextureSlice` 的 P/Invoke + 包装方法：
```csharp
public sealed class TextureMetadata {
    public uint Width, Height, ArraySize, MipLevels, Depth, Flags;
}
public static TextureMetadata? GetTextureMetadata(string path);
public static byte[]? GetTextureSlicePixels(string path, uint array, uint mip, uint depth);
```

**R3.a 不做 R4**：不需要 native slice API——metadata 从 sidecar 来，PNG/JPG 只能看顶层 slice。

### R5：改造 `TextureEditorViewModel` 让 `MaxMipIndex` / `MaxArrayIndex` / `MaxDepthIndex` 从 sidecar 读（R3.a 实际需要的实现）

进一步勘察后发现 **R3.a 比最初估计更小**——不需要新的 decoder 类，只需让 `TextureSliceSet` 用 sidecar 的 array/mip/depth 而非 PNG 解码结果：

现有代码（`TextureEditorViewModel.cs:41`）：
```csharp
_slices = new TextureSliceSet(decoded.Width, decoded.Height, decoded.ArrayCount, decoded.MipCount, decoded.Rgba);
```

现有代码（`TextureEditorViewModel.cs:52-54`）：
```csharp
Info = new TextureInfo {
    ArraySize = Math.Max(decoded.ArrayCount, Settings.ArraySize),
    MipLevels = Math.Max(decoded.MipCount, Settings.MipLevels),
    Depth = Math.Max(decoded.Depth, Settings.Depth),
    ...
};
```

**矛盾**：`Info`（显示在状态栏 summary）已经用 `Math.Max(decoded, Settings)`——但 `_slices`（决定 `MaxMipIndex` / `MaxArrayIndex` / `MaxDepthIndex`，也就是 selector 的上限）只用 `decoded`。结果：状态栏显示 "Mip Levels: 6"，selector 只能从 0/0 开始——**不一致**。

修改：把 `_slices` 构造也用 `Math.Max`：
```csharp
_slices = new TextureSliceSet(
    decoded.Width, decoded.Height,
    Math.Max(decoded.ArrayCount, Settings.ArraySize),
    Math.Max(decoded.MipCount, Settings.MipLevels),
    decoded.Rgba);  // 仍是 index 0 的 RGBA——PNG 不会变
```

**接口无需变更**：`AvaloniaTextureSliceDecoder` 仍只解 PNG index 0；sidecar 信息在 VM 层面 merge。这是 better separation of concerns：decoder 知道 PNG 怎么解，VM 知道用户意图是什么。

**额外小修复**：现有 `IsNormalMap` / `ShowR` 等 setter 改变后**不触发** `MaxMipIndex` / `MaxArrayIndex` / `MaxDepthIndex` 的 PropertyChanged。

实际触发时机：

1. 用户从 settings UI 改 `ArraySize` / `MipLevels` / `Depth` 时——但目前 `TextureEditorView.axaml:14-24` **只 bind 了 `OutputFormat`**！`ArraySize` / `MipLevels` / `Depth` 在 sidecar 里有字段但**没有 UI 控件可编辑**。这意味着在当前 AXAML 下，settings 里的 `ArraySize` 等永远是默认值（1/0/1），所以 R5 实施后**用户能看到的 UI 变化为零**——除非同时加 settings 控件。
2. `SetAsset()` 加载新资产时（line 197）——`RecomputeMaxIndices` 需要在 `LoadAsync` 完成后被调一次（因为 `_slices` 在 `LoadFromDisk` 里建）
3. `Discard()` 取消编辑时（line 349）——已知需要 `RecomputeMaxIndices()`
4. ~~`Reimport()` 后（line 323 调用 `Save`）~~——经代码阅读确认：Reimport 内部 Save 写盘但不替换 `Settings` 字段，所以**不需要** RecomputeMaxIndices。**实际只有 2 个 call point**（SetAsset + Discard）需要 RecomputeMaxIndices。

**v7 发现**：生产路径 `LoadFromDisk`（line 221）也用相同的 broken pattern——`_slices` 用 `decoded.ArrayCount` 直接构造，但 `Info` 用 `Math.Max(decoded, Settings)`。这意味着 R5 修复**必须同时改 `LoadFromDisk` 和 `SetSlicesForTest`** 两个地方（一个生产路径、一个测试钩子）。两处的 PropertyChanged 调用也需要对齐。

**结论**：R5 实施后**用户能看到的 UI 变化为零**（因为没 UI 改 sidecar settings）。这反而简化了 P1.c：纯内部一致性修复 + 测试覆盖，**没有可观察的 UI 行为变化**。

注意：即便 R5 不带可见 UI 变化，**2 个 call point**（SetAsset line 197, Discard line 349）仍需要插入 `RecomputeMaxIndices()` 调用——因为它们会重新加载 Settings，万一用户在两次打开之间手编辑了 `.meta` 文件，新值需要反映。**测试用例需要覆盖**这种"外部修改 sidecar 后重开"的场景。

这有两种解读：
- **消极**：本次提交没有用户可见价值，只是清理内部不一致
- **积极**：本次提交**为未来 settings UI 扩展**做准备（"先改 internals，等 UI 来了自动显示"）。这也是 P1.b 的 ContentBrowser search/breadcrumb 同样模式——helper 已经存在，本轮只接线

**建议**：本轮做 R5 修复（一致性）+ 测试覆盖，并在 PR 描述里明确说明"用户可见 UI 不变，本轮是为未来扩展铺路"。后续若需要，可加 UI 控件做后续工作。

### Sidecar 二进制格式向前兼容（v6 发现）

`TextureImportSettings.FromBinary` 用 `if (r.BaseStream.Position < r.BaseStream.Length) r.ReadXxx()` 模式读取 `IsSRGB` / `CompressionMode` / `ArraySize` / `Depth` / `IsNormalMap` 这些**后加的字段**。含义：
- 旧版 sidecar（没这些字段）读取时跳过 → 这些字段保持 `T()` 默认值（`ArraySize=1` / `MipLevels=0` / `Depth=1` / `IsNormalMap=false`）
- `AssetImportSettingsStore.Load<T>` 不抛异常（catch 全捕获，返回 `new T()`）
- `AssetImportSettingsStore` 的 `Magic` + `CurrentVersion` 在字段添加时**不递增**——所以 v0 + v1 sidecar 共存

这是 **safe evolution pattern**——意味着即使 P1.c 不存在，sidecar 字段已经在历史上渐进添加而未破坏旧数据。R5 实施不需要担心迁移问题。

### R5 接口扩展决策

R3.a 下**不需要扩展 `ITextureSliceDecoder`**——decoder 仍只解 index 0，sidecar merge 在 VM 层。原本 R5 的 A/B/C 选项问题不复存在。

### R6：UI 接线

**推荐 R6.a（label-only）**：

保持 AXAML 现状——3 个 TextBlock + `MipDisplay`/`ArrayDisplay`/`DepthDisplay` 绑定。R5 实施后这些 label 会自动显示 "Mip: 0/5"（反映 settings 的 5 个 mip），不再显示 "Mip: 0/0"（PNG 的 1 个 mip）。

**不增加 selector**：因为 PNG 永远只能解 index 0，加 selector 让用户能"选" 但 "选完没变化" 是 dishonest。

**R6.b（可选 disabled selector）**：加 ◀/▶ 按钮但 `IsEnabled` 绑定到 `_slices?.MipCount > 1`（PNG 永远 false → 永远 disabled）。视觉上给用户"这里本应有 selector 但 PNG 不支持" 的暗示。

**R6.c（不推荐）**：可点 selector 但点击不更新预览——misleading，不要做。

### R7：触发 MaxMipIndex 等重新计算（settings 改变时）

每次 `Settings.ArraySize` / `MipLevels` / `Depth` 改变后，需要：
- 重新构造 `_slices`（保留同一份 RGBA，只改 metadata counts）
- 触发 `MaxMipIndex` / `MaxArrayIndex` / `MaxDepthIndex` PropertyChanged

实现：把 `RecomputeMaxIndices()` 调用插入 **2 个 settings 替换点**：

```csharp
private void RecomputeMaxIndices() {
    if (_slices == null) return;
    var currentRgba = _slices.Rgba;
    var w = _slices.Width;
    var h = _slices.Height;
    _slices = new TextureSliceSet(w, h,
        Math.Max(1, Settings.ArraySize),
        Math.Max(1, Settings.MipLevels),
        currentRgba);
    OnPropertyChanged(nameof(MaxMipIndex));
    OnPropertyChanged(nameof(MaxArrayIndex));
    OnPropertyChanged(nameof(MaxDepthIndex));
    OnPropertyChanged(nameof(MipDisplay));
    OnPropertyChanged(nameof(ArrayDisplay));
    OnPropertyChanged(nameof(DepthDisplay));
}
```

**v7 修正**：v5 假设的"修改 `SetSlicesForTest` 一处即可"是错的。生产路径 `LoadFromDisk` (line 235) 也用相同 broken pattern——`_slices` 直接用 `decoded.X`，`Info` 用 `Math.Max(decoded, Settings)`。需要**同时修改两处**：

```csharp
// In SetSlicesForTest (line 41) — test hook:
_slices = new TextureSliceSet(
    decoded.Width, decoded.Height,
    Math.Max(decoded.ArrayCount, Settings.ArraySize),
    Math.Max(decoded.MipCount, Settings.MipLevels),
    decoded.Rgba);

// In LoadFromDisk (line 235) — production path:
_slices = new TextureSliceSet(
    decoded.Width, decoded.Height,
    Math.Max(decoded.ArrayCount, Settings.ArraySize),
    Math.Max(decoded.MipCount, Settings.MipLevels),
    decoded.Rgba);
```

`SetSlicesForTest` 的 PropertyChanged 调用（lines 58-63）和 `LoadFromDisk` 的 PropertyChanged 调用（lines 236-241）也需对齐——`RecomputeMaxIndices` 内的 PropertyChanged 调用已经覆盖所需的 6 个属性，所以两个 spot 可以删除原有的 PropertyChanged 代码，统一调 `RecomputeMaxIndices()`。

**调用点**（每个都设置 `Settings = AssetImportSettingsStore.Load<...>(...)` 后跟 `RecomputeMaxIndices()`）：

| 行号 | 方法 | 何时 |
|---|---|---|
| 197 | `SetAsset` | 用户打开 texture asset |
| 349 | `Discard` | 用户按 Discard 按钮（取消 settings 编辑） |

**不需要 RecomputeMaxIndices 的点**：
- `Reimport()` (line 323) — 内部 `Save()` 写盘但不替换 `Settings` 字段；_slices 已经是当前 PNG 解码结果，settings 没变
- `Save()` (line 337) — 同上

**未来 settings UI**：如果将来加 AXAML 控件编辑 `Settings.ArraySize` 等，控件 setter 需要在每次改后调 `RecomputeMaxIndices()`——可走 `Settings.PropertyChanged` 订阅或显式 setter hook。本次不实现。

### R8：测试（必做）

**已有测试**（不需修改）：
- `TextureSliceSetTests` 覆盖 layout 边界
- `TextureEditorViewModelTests` 覆盖 IsNormalMap / IsSRGB / Undo

**新增测试**（写入 `PrimalEditor_Avalonia.Tests/Texture/TextureEditorViewModelTests.cs` 或新文件 `TextureEditorMaxIndicesTests.cs`）：

```csharp
// Case 1: decoded (1,1,1) + settings (ArraySize=1, MipLevels=1, Depth=1)
//   → MaxMipIndex = 0, label "Mip: 0/0"（现状保持）
// Case 2: decoded (1,1,1) + settings (ArraySize=4, MipLevels=8, Depth=1)
//   → MaxMipIndex = 7, MaxArrayIndex = 3, MaxDepthIndex = 0
//   → label "Mip: 0/7" "Array: 0/3"（settings 反映到 UI）
// Case 3: decoded (1,1,1) + settings (ArraySize=0, MipLevels=0, Depth=0)
//   → Math.Max(1, 0) = 1 → MaxMipIndex = 0（防御 0 边界）
// Case 4: SetSlicesForTest 后修改 settings.Settings.ArraySize = 6
//        然后调 RecomputeMaxIndices() → MaxArrayIndex 变为 5
// Case 5: SetSlicesForTest 传入 (4,6,1) + settings (1,1,1)
//   → MaxMipIndex = 5（Math.Max(6,1)=6）— decoded 占上
// Case 6: RecomputeMaxIndices 保留同一份 RGBA 引用（不变 alloc）
//   → 通过 reflection 验证 `_slices.Rgba` 指针不变
//        或：decoder 给同一 buffer 两次，验证 `_slices.Rgba` 引用相同
// Case 7: SetSlicesForTest(null) → MaxMipIndex = 0（_slices null 情况）
```

测试要点：
- **Case 1**：确保 backward compat——单个 PNG 仍显示 "0/0"，不破坏现有行为
- **Case 2**：核心 case——证明 settings 反映到 selector
- **Case 3**：边界——0 在 Math.Max(1, 0) 后的处理（避免 0 slice 显示）
- **Case 4**：触发链——Save() 后 settings 改了，MaxMipIndex 更新
- **Case 5**：Math.Max 的对称——decoded 大 settings 小时，decoded 优先（与 `Info` 一致）
- **Case 6**：GC 友好——避免 RGBA 重新分配导致 `WriteableBitmap` 的 pinned handle 失效
- **Case 7**：null 安全

**无需新增 decoder 测试**：因为 R3.a 不引入新 decoder 类。`AvaloniaTextureSliceDecoder` 不变，现有 4 个测试 (`TextureEditorSliceViewModelTests`) 继续有效。

## 不在范围

- ❌ **修改 runtime 路径加载 DDS/KTX/纹理数组 PNG 序列**：那是 engine 端的工作，超出编辑器扩展
- ❌ **把 `ContentTools` 的完整 import 路径接到编辑器**：那是另一条 pipeline（libContentTools heavy path），改动面比本设计大很多
- ❌ **WPF 的 Import Settings 配置窗口迁移**：与多 slice 无关，独立任务
- ❌ **Volume texture 的 3D 预览**：UI 复杂度远超 2D slice selector，单独规划

## 实施拆解（R3.a 路线，1 个 commit，约 80 LoC）

| 改动 | 文件 | 行数 |
|---|---|---|
| `SetSlicesForTest` 中 `_slices` 构造用 `Math.Max(decoded, Settings)` | `TextureEditorViewModel.cs:41` | ~5 |
| `RecomputeMaxIndices()` 私有方法 + 在 `Save()` / `SetAsset()` 末尾调用 | `TextureEditorViewModel.cs` | ~20 |
| 状态栏加 "Preview limited to slice 0 in v1" 提示 | `TextureEditorView.axaml.cs` 或 VM property | ~10 |
| 测试：`TextureEditorViewModelTests` 新增 3-4 个 case 覆盖 sidecar-driven MaxMipIndex | `PrimalEditor_Avalonia.Tests/Editors/Texture/` | ~40 |

**无 native 改动**，无 AXAML selector 改动（除非选 R6.b）。

## 风险与缓解

1. **VM 中 `_slices` 持有 `ReadOnlyMemory<byte>`**：`RecomputeMaxIndices` 必须保留同一份 `Rgba` 引用，不能重新分配（否则会让 GC 把旧 buffer 释放，破坏 `WriteableBitmap` 的 pinned handle）。缓解：构造新 `TextureSliceSet` 时**传入同一 `Rgba` 实例**，不 copy。
2. **`Info` 已用 `Math.Max`，与新 `_slices` 一致性**：构造 `_slices` 后 `MaxMipIndex` 会与 `Info.MipLevels` 一致。`MipDisplay` 字符串是 `$"Mip: {_mipIndex}/{MaxMipIndex}"`——`_mipIndex` clamp 到 `[0, MaxMipIndex]` 范围，所以当 MaxMipIndex=5, _mipIndex 默认 0 → "Mip: 0/5"。
3. **`Settings` 是 `TextureImportSettings`（自动属性）**：未实现 `INotifyPropertyChanged`——监听 settings 改变需要走 `Settings.PropertyChanged` event 或在 `Save()` 后手动 `RecomputeMaxIndices()`。**采后者**（更显式）。
4. **不影响 `P1.b` 已 commit 的 ContentBrowser 改动**：本轮只动 TextureEditor。

## 待用户确认的关键决策（**修正版**）

0. **R3 路线**：上一版设计的"新增 `Editor_GetTextureSlice` 到 `libContentTools`"已取消，理由见 R3 节。需要你选 R3.a / R3.b / R3.c 三选一。我**强烈推荐 R3.a**（诚实 UI 模式），因为：
   - R3.b（runtime path 加载 DDS/KTX）工作量与收益不成比例，应单独立项
   - R3.c（不动）意味着 P1.c 这一轮什么都不做，等于取消任务
   - R3.a 在 editor 范围内解决问题，不越界到引擎 asset pipeline

1. **R6 UI**: 选项:
   - **R6.a**（推荐）：保留现状 label-only。`MipDisplay` 显示 "Mip: 0/5"，不增加 selector。状态栏加 "Preview limited to slice 0 in v1" 提示。
   - **R6.b**（可选）：加 disabled ◀/▶ 按钮——视觉占位，灰色不可点。
   - **R6.c**（不推荐）：加可点按钮但点击无效——misleading UI。

2. **不需要 R5 接口扩展**（R3.a 路线下 decoder 接口不变）。原 R5 的 A/B/C 选项问题不存在。

不收到 "go" 不会写任何实现代码。

## Lessons learned 候选

实施完成后（无论 R3 选 a/b/c），考虑向 `tasks/lessons.md` 加一条：

> **Avalonia Editor — "decoder 永远返回 1×1×1" 的根因是 pipeline 错位 + 文件根本不存在**
>
> `libContentTools`（import path，支持 DirectXTex 全 array/mip/depth）和 `libEngineDLL`（runtime path，stbi_load 永远 1×1×1）维护两套独立的 cooked texture 路径。编辑器调的是 runtime path，所以即便 import 路径能保留 slice 信息，编辑器也读不到。
>
> **更糟的是**：编辑器的 import 流程（`AssetImporter.cs:88-130`）**从不写 cooked 文件到磁盘**——它复制 source PNG 到 Content 目录并直接通过 `libEngineDLL` 的 GPU 路径加载，没有中间产物。`ContentTools::Import` 这条 heavy path 只在引擎的 offline baking pipeline 里用，不在编辑器里。所以"为编辑器写一个 `Editor_GetTextureSlice` 从 cooked 文件读 array/mip/depth"的方案**前提就是错的**——cooked 文件不存在。
>
> 看到 "decoder 写死 1×1×1" 时先问三个问题：
> 1. 是格式不支持，还是走的路径根本不是有元数据的那条？
> 2. 这条路径有没有产出任何 on-disk artifact 可读？
> 3. UI 显示 "Array 6" 但源是单张 PNG——是不是用户意图（settings）vs 源能力（PNG）的错位需要诚实呈现？

教训：**设计 native ABI 扩展前，先 trace 完整的数据流到 on-disk artifact**。如果 artifact 不存在，再多的 native 调用都无用。