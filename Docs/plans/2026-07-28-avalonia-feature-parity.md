# PrimalEditor_Avalonia 功能补齐实施计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 把 PrimalEditor (WPF) 中除 Visual Studio 集成外的其他缺失功能完整移植到 PrimalEditor_Avalonia,优先做高杠杆可复用的基础层,然后再构建依赖其上的具体功能。

**Architecture:**
- 先建 4 个**复用基础模块** (AssetInfo/Registry、VectorBox/NumberBox、ComponentView/MSComponent、IAssetEditor)
- 这些模块一次实现,被 Texture Editor / Geometry Editor / Primitive Mesh Dialog / Inspector / ContentBrowser 等多场景共享
- 再实现 3 个**资产编辑器** (Texture/Geometry/Primitive Mesh) 完整功能
- 最后整合 WorldEditor 工具栏 + ContentWatcher + Debug/Release 占位

**Tech Stack:**
- .NET 8.0 / Avalonia 11.3 / CommunityToolkit.Mvvm 8.2
- System.Text.Json (序列化)
- 自定义控件基于 TemplatedControl / UserControl (Avalonia 无 WPF DependencyProperty,改为 StyledProperty+AvaloniaProperty)

**关键差异(必须知道的):**
- WPF `DependencyProperty` → Avalonia `StyledProperty` / `AvaloniaProperty`
- WPF `RoutedEvent` → Avalonia 自定义事件通过 `AddHandler` / `OnEvent`
- WPF `DataContract XML` → Avalonia 沿用 `System.Text.Json` (`JsonPropertyName`)
- WPF `HwndHost` → Avalonia `NativeControlHost`
- WPF `UserControl` XAML 后置类名是分部类;Avalonia 是 `partial class` + `AvaloniaXamlLoader.Load(this)`

**vs 完整 WPF 版本,本次不实现:**
- Visual Studio COM 集成 (NewScript/VisualStudio.cs) — 用户已确认跳过
- 真正接 C++ 引擎的 `ContentTools.dll` 烘焙管线 — Avalonia 当前没有这个 native dll;Texture/Geometry Editor 使用跨平台 C# 实现版本(基于 SkiaSharp 或纯 managed)
- OpenTK_View (WPF 的 OpenGL 测试用例,本来就注释掉了)

---

## 任务依赖图

```
T1 (AssetInfo/Registry)
  ↓
T2 (VectorBox/NumberBox) ←─── 独立,可与 T1 并行
  ↓
T3 (ComponentView/MSComponent)
  ↓
T4 (IAssetEditor 路由)
  ↓
T5 (Debug/Release 占位) ←── 独立
T6 (Primitive Mesh Dialog) ←── 依赖 T2
T7 (Texture Editor) ←── 依赖 T1 + T2 + T4
T8 (Geometry Editor) ←── 依赖 T1 + T2 + T3 + T4
T9 (ContentWatcher) ←── 依赖 T1
T10 (WorldEditor 整合) ←── 依赖 T5-T9
T11 (验证)
```

---

## Task 1: AssetInfo / AssetRegistry 抽象层

**目标:** 在 `Content/` 下建立资产元数据系统。Content Browser 不再只按文件枚举,而是带 GUID + Icon + ImportDate 的资产对象集合。这是 Texture/Geometry Editor 的基石。

**Files:**
- Create: `PrimalEditor_Avalonia/Content/AssetInfo.cs` — AssetType 枚举 + AssetInfo 数据类
- Create: `PrimalEditor_Avalonia/Content/AssetRegistry.cs` — 静态注册表,扫描 Content/ 目录
- Create: `PrimalEditor_Avalonia/Content/AssetImportSettings.cs` — `IAssetImportSettings` 接口 + `CopyImportSettings<T>` 通用方法
- Create: `PrimalEditor_Avalonia/Content/ContentHelper.cs` — `IsDirectory`, `IsAssetFile(path)` 等静态判断
- Modify: `PrimalEditor_Avalonia/Views/ContentBrowserView.axaml.cs` — 双击 AssetInfo 改为触发 `IAssetEditor.OpenFor(info)` 而非 `Process.Start`
- Modify: `PrimalEditor_Avalonia/ViewModels/ContentBrowserViewModel.cs` — 改用 `AssetInfo[]` 而非 `ContentFile[]`,增加 `OnAssetDoubleClick` 路由

**Step 1:** 实现 `AssetInfo` / `AssetType`

```csharp
// Content/AssetInfo.cs
using System;
using System.IO;
using System.Text.Json.Serialization;

namespace PrimalEditor_Avalonia.Content;

public enum AssetType
{
    Unknown,
    Texture,       // .png .jpg .tga .bmp .dds .hdr .exr .ktx .ktx2
    Mesh,          // .fbx .obj .gltf .glb .dae .blend + .mesh
    Audio,         // .wav .mp3 .ogg .flac
    Script,        // .py .lua .cs
    Material,      // .mat
    Animation,     // .anim
    Skeleton,      // .skel
}

public sealed class AssetInfo
{
    [JsonPropertyName("Guid")]        public Guid Guid { get; set; } = Guid.NewGuid();
    [JsonPropertyName("Type")]        public AssetType Type { get; set; } = AssetType.Unknown;
    [JsonPropertyName("FullPath")]    public string FullPath { get; set; } = string.Empty;
    [JsonPropertyName("ImportDate")]  public DateTime ImportDate { get; set; } = DateTime.UtcNow;
    [JsonPropertyName("RegisterTime")]public DateTime RegisterTime { get; set; } = DateTime.UtcNow;
    [JsonPropertyName("Hash")]        public byte[]? Hash { get; set; }

    [JsonIgnore] public string FileName => Path.GetFileName(FullPath);
    [JsonIgnore] public string FileNameWithoutExt => Path.GetFileNameWithoutExtension(FullPath);
    [JsonIgnore] public string Extension => Path.GetExtension(FullPath).ToLowerInvariant();
    [JsonIgnore] public DateTime LastModified =>
        File.Exists(FullPath) ? File.GetLastWriteTime(FullPath) : DateTime.MinValue;
    [JsonIgnore] public bool IsDirectory =>
        Directory.Exists(FullPath) && !File.Exists(FullPath);
}
```

**Step 2:** 实现 `ContentHelper`

```csharp
// Content/ContentHelper.cs
using System.IO;

namespace PrimalEditor_Avalonia.Content;

public static class ContentHelper
{
    private static readonly string[] ImageExts  = { ".png",".jpg",".jpeg",".tga",".bmp",".dds",".hdr",".exr",".ktx",".ktx2" };
    private static readonly string[] MeshExts   = { ".fbx",".obj",".gltf",".glb",".dae",".blend",".mesh" };
    private static readonly string[] AudioExts  = { ".wav",".mp3",".ogg",".flac" };
    private static readonly string[] ScriptExts = { ".py",".lua",".cs" };

    public static AssetType GetAssetType(string filePath)
    {
        var ext = Path.GetExtension(filePath).ToLowerInvariant();
        if (System.Array.IndexOf(ImageExts, ext)  >= 0) return AssetType.Texture;
        if (System.Array.IndexOf(MeshExts, ext)   >= 0) return AssetType.Mesh;
        if (System.Array.IndexOf(AudioExts, ext)  >= 0) return AssetType.Audio;
        if (System.Array.IndexOf(ScriptExts, ext) >= 0) return AssetType.Script;
        return AssetType.Unknown;
    }

    public static bool IsDirectory(string path) =>
        Directory.Exists(path) && !File.Exists(path);

    public static bool IsAssetFile(string path) =>
        !IsDirectory(path) && GetAssetType(path) != AssetType.Unknown;

    public static byte[] ComputeHash(string filePath)
    {
        using var sha = System.Security.Cryptography.SHA1.Create();
        using var fs = File.OpenRead(filePath);
        return sha.ComputeHash(fs);
    }
}
```

**Step 3:** 实现 `AssetImportSettings` 接口

```csharp
// Content/AssetImportSettings.cs
using System.IO;

namespace PrimalEditor_Avalonia.Content;

public interface IAssetImportSettings
{
    void ToBinary(BinaryWriter writer);
    void FromBinary(BinaryReader reader);
}

public static class AssetImportSettingsExtensions
{
    public static void CopyImportSettings<T>(T from, T to) where T : class, IAssetImportSettings
    {
        if (from == null || to == null)
            throw new System.ArgumentNullException();
        using var ms = new MemoryStream();
        using (var w = new BinaryWriter(ms)) { from.ToBinary(w); w.Flush(); }
        ms.Position = 0;
        using var r = new BinaryReader(ms);
        to.FromBinary(r);
    }
}
```

**Step 4:** 实现 `AssetRegistry`

```csharp
// Content/AssetRegistry.cs
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Content;

/// <summary>
/// 全局资产注册表。Project 加载时调用 Initialize(root),卸载时调用 Reset()。
/// 双击 Content Browser 中的 AssetInfo 时,通过 GetAssetsByType() 拿到对应 ViewModel。
/// </summary>
public static class AssetRegistry
{
    private static readonly Dictionary<string, AssetInfo> _byPath = new();
    private static readonly ObservableCollection<AssetInfo> _all = new();

    public static ReadOnlyObservableCollection<AssetInfo> Assets { get; } =
        new ReadOnlyObservableCollection<AssetInfo>(_all);

    public static int Count => _all.Count;

    public static void Initialize(string projectContentRoot)
    {
        Reset();
        if (!Directory.Exists(projectContentRoot))
        {
            Logger.Warning($"AssetRegistry: Content root not found: {projectContentRoot}");
            return;
        }
        ScanRecursive(projectContentRoot);
        Logger.Info($"AssetRegistry: scanned {_all.Count} asset(s) from {projectContentRoot}");
    }

    public static void Reset()
    {
        _byPath.Clear();
        _all.Clear();
    }

    public static AssetInfo? Register(string fullPath)
    {
        if (!File.Exists(fullPath) && !Directory.Exists(fullPath)) return null;
        if (_byPath.TryGetValue(fullPath, out var existing)) return existing;

        var info = new AssetInfo
        {
            FullPath = fullPath,
            Type = ContentHelper.IsDirectory(fullPath) ? AssetType.Unknown : ContentHelper.GetAssetType(fullPath),
            RegisterTime = DateTime.UtcNow,
            ImportDate = File.Exists(fullPath) ? File.GetLastWriteTimeUtc(fullPath) : DateTime.UtcNow,
        };
        if (File.Exists(fullPath))
        {
            try { info.Hash = ContentHelper.ComputeHash(fullPath); } catch { /* tolerant */ }
        }
        _byPath[fullPath] = info;
        _all.Add(info);
        return info;
    }

    public static void Unregister(string fullPath)
    {
        if (!_byPath.TryGetValue(fullPath, out var info)) return;
        _byPath.Remove(fullPath);
        _all.Remove(info);
    }

    public static AssetInfo? Get(string fullPath) =>
        _byPath.TryGetValue(fullPath, out var i) ? i : null;

    public static IEnumerable<AssetInfo> GetByType(AssetType type)
    {
        foreach (var info in _all)
            if (info.Type == type) yield return info;
    }

    private static void ScanRecursive(string root)
    {
        foreach (var dir in Directory.GetDirectories(root))
            ScanRecursive(dir);
        foreach (var file in Directory.GetFiles(root))
        {
            if (ContentHelper.IsAssetFile(file)) Register(file);
        }
    }
}
```

**Step 5:** 修改 `ContentBrowserViewModel` 使用 `AssetInfo`

```csharp
// Views/ContentBrowserViewModel.cs (关键改动)
- public ObservableCollection<ContentFile> Files { get; } = new();
+ public ObservableCollection<AssetInfo> Files { get; } = new();

  public void LoadFilesForSelectedFolder() {
      Files.Clear();
      if (SelectedFolder == null || !Directory.Exists(SelectedFolder.FullPath)) return;
      AssetRegistry.Register(SelectedFolder.FullPath);
      foreach (var dir in Directory.GetDirectories(SelectedFolder.FullPath))
          AssetRegistry.Register(dir);
      foreach (var file in Directory.GetFiles(SelectedFolder.FullPath))
      {
          var info = AssetRegistry.Register(file);
          if (info != null && info.Type != AssetType.Unknown)
              Files.Add(info);
      }
      StatusMessage = $"{Files.Count} asset(s)";
  }
```

并在 `ContentBrowserView.axaml` 绑定 `Files` 用 `AssetType` 替代 `AssetType`(已有同名,但要展示 Icon 和 ImportDate,可选)。

**Step 6:** 编译验证
- `dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj` 应成功。
- `ContentBrowser` 打开任意已存在的项目,左 Folder + 右 File 列表应正常,Status 显示 `N asset(s)`。

**Step 7:** Commit
```bash
git add PrimalEditor_Avalonia/Content/
git add PrimalEditor_Avalonia/Views/ContentBrowserViewModel.cs
git commit -m "feat(content): add AssetInfo/AssetRegistry with GUID-indexed asset metadata"
```

---

## Task 2: VectorBox / NumberBox / ScalarBox 控件家族

**目标:** 提供"拖动鼠标改值 + 键盘输入"的可复用数值控件。这一个控件被 Transform Editor、Primitive Mesh 创建、Texture Import Settings 共用,**复用性最高**。

**Files:**
- Create: `PrimalEditor_Avalonia/Controls/NumberBox.cs` — 基类,单值,鼠标拖拽支持
- Create: `PrimalEditor_Avalonia/Controls/VectorBox.cs` — 组合 2-4 个 NumberBox
- Create: `PrimalEditor_Avalonia/Controls/ScalarBox.cs` — NumberBox 别名(便于 XAML 区分语义)
- Create: `PrimalEditor_Avalonia/Controls/Controls.axaml` — 默认样式/模板

**Step 1:** 实现 `NumberBox`

```csharp
// Controls/NumberBox.cs
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Data;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;

namespace PrimalEditor_Avalonia.Controls;

public enum NumberBoxType { Float, Int, UInt }

[TemplatePart("PART_TextBlock", typeof(TextBlock))]
[TemplatePart("PART_TextBox",   typeof(TextBox))]
public class NumberBox : TemplatedControl
{
    private TextBlock? _label;
    private TextBox?   _editor;
    private double     _original;
    private double     _mouseStart;
    private bool       _captured;
    private bool       _changedSincePress;
    private NumberBoxType _kind = NumberBoxType.Float;

    public static readonly StyledProperty<string> ValueProperty =
        AvaloniaProperty.Register<NumberBox, string>(nameof(Value),
            defaultBindingMode: BindingMode.TwoWay);

    public static readonly StyledProperty<double> MultiplierProperty =
        AvaloniaProperty.Register<NumberBox, double>(nameof(Multiplier), 0.01);

    public static readonly StyledProperty<NumberBoxType> KindProperty =
        AvaloniaProperty.Register<NumberBox, NumberBoxType>(nameof(Kind));

    public static readonly StyledProperty<bool> ReadOnlyProperty =
        AvaloniaProperty.Register<NumberBox, bool>(nameof(ReadOnly));

    public string Value       { get => GetValue(ValueProperty);       set => SetValue(ValueProperty, value); }
    public double Multiplier  { get => GetValue(MultiplierProperty);  set => SetValue(MultiplierProperty, value); }
    public NumberBoxType Kind { get => GetValue(KindProperty);        set => SetValue(KindProperty, value); }
    public bool ReadOnly      { get => GetValue(ReadOnlyProperty);    set => SetValue(ReadOnlyProperty, value); }

    public event EventHandler<string>? ValueCommitted; // 失去焦点/Enter 时
    public event EventHandler<string>? ValueDragging;   // 鼠标拖拽时(高频)

    static NumberBox()
    {
        ValueProperty.Changed.AddClassHandler<NumberBox>((b, e) =>
            b.ValuePropertyChanged(e));
    }

    protected override void OnApplyTemplate(TemplateAppliedEventArgs e)
    {
        base.OnApplyTemplate(e);
        _label  = e.NameScope.Find<TextBlock>("PART_TextBlock");
        _editor = e.NameScope.Find<TextBox>("PART_TextBox");

        if (_label != null)
        {
            _label.PointerPressed  += OnLabelPointerPressed;
            _label.PointerMoved    += OnLabelPointerMoved;
            _label.PointerReleased += OnLabelPointerReleased;
        }
        if (_editor != null)
        {
            _editor.KeyDown   += OnEditorKeyDown;
            _editor.LostFocus += OnEditorLostFocus;
        }
    }

    private void OnLabelPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        if (ReadOnly || _label == null) return;
        double.TryParse(Value, System.Globalization.NumberStyles.Any,
            System.Globalization.CultureInfo.InvariantCulture, out _original);
        e.Pointer.Capture(_label);
        _captured = true;
        _changedSincePress = false;
        _mouseStart = e.GetPosition(_label).X;
        e.Handled = true;
    }

    private void OnLabelPointerMoved(object? sender, PointerEventArgs e)
    {
        if (!_captured || _label == null) return;
        var x = e.GetPosition(_label).X;
        var delta = (x - _mouseStart) * Multiplier;
        var next  = _original + delta;
        next = Kind switch
        {
            NumberBoxType.Int   => System.Math.Round(next),
            NumberBoxType.UInt  => System.Math.Max(0, System.Math.Round(next)),
            _                   => next,
        };
        var s = next.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture);
        if (s != Value)
        {
            Value = s;
            ValueDragging?.Invoke(this, s);
            _changedSincePress = true;
        }
        e.Handled = true;
    }

    private void OnLabelPointerReleased(object? sender, PointerReleasedEventArgs e)
    {
        if (!_captured) return;
        e.Pointer.Capture(null);
        _captured = false;
        if (_changedSincePress)
            ValueCommitted?.Invoke(this, Value);
        e.Handled = true;
    }

    private void OnEditorKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter && _editor != null)
        {
            Commit(_editor.Text);
            e.Handled = true;
        }
    }

    private void OnEditorLostFocus(object? sender, RoutedEventArgs e)
    {
        if (_editor != null) Commit(_editor.Text);
    }

    private void Commit(string? text)
    {
        if (string.IsNullOrWhiteSpace(text)) { Value = "0"; return; }
        if (double.TryParse(text, System.Globalization.NumberStyles.Any,
            System.Globalization.CultureInfo.InvariantCulture, out var d))
        {
            d = Kind switch
            {
                NumberBoxType.Int   => System.Math.Round(d),
                NumberBoxType.UInt  => System.Math.Max(0, System.Math.Round(d)),
                _                   => d,
            };
            Value = d.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture);
            ValueCommitted?.Invoke(this, Value);
        }
        else
        {
            // 还原原值
            Value = _original.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture);
        }
    }

    private void ValuePropertyChanged(AvaloniaPropertyChangedEventArgs e)
    {
        // 同步显示到内部 editor(若已应用模板)
        if (_editor != null && _editor.Text != (string?)e.NewValue)
            _editor.Text = (string?)e.NewValue;
        if (_label != null)
            _label.Text = (string?)e.NewValue ?? "";
    }
}
```

**Step 2:** 实现 `VectorBox`

```csharp
// Controls/VectorBox.cs
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;

namespace PrimalEditor_Avalonia.Controls;

public enum VectorBoxDimension { Vec2, Vec3, Vec4 }

public class VectorBox : NumberBox
{
    public static readonly StyledProperty<string> XProperty =
        AvaloniaProperty.Register<VectorBox, string>(nameof(X), defaultBindingMode: Avalonia.Data.BindingMode.TwoWay);
    public static readonly StyledProperty<string> YProperty =
        AvaloniaProperty.Register<VectorBox, string>(nameof(Y), defaultBindingMode: Avalonia.Data.BindingMode.TwoWay);
    public static readonly StyledProperty<string> ZProperty =
        AvaloniaProperty.Register<VectorBox, string>(nameof(Z), defaultBindingMode: Avalonia.Data.BindingMode.TwoWay);
    public static readonly StyledProperty<string> WProperty =
        AvaloniaProperty.Register<VectorBox, string>(nameof(W), defaultBindingMode: Avalonia.Data.BindingMode.TwoWay);
    public static readonly StyledProperty<VectorBoxDimension> DimensionProperty =
        AvaloniaProperty.Register<VectorBox, VectorBoxDimension>(nameof(Dimension), VectorBoxDimension.Vec3);
    public static readonly StyledProperty<Orientation> OrientationProperty =
        AvaloniaProperty.Register<VectorBox, Orientation>(nameof(Orientation), Orientation.Horizontal);

    public string X { get => GetValue(XProperty); set => SetValue(XProperty, value); }
    public string Y { get => GetValue(YProperty); set => SetValue(YProperty, value); }
    public string Y_internal => Y;
    public string Z { get => GetValue(ZProperty); set => SetValue(ZProperty, value); }
    public string W { get => GetValue(WProperty); set => SetValue(WProperty, value); }
    public VectorBoxDimension Dimension { get => GetValue(DimensionProperty); set => SetValue(DimensionProperty, value); }
    public Orientation Orientation { get => GetValue(OrientationProperty); set => SetValue(OrientationProperty, value); }
}
```

**Step 3:** 实现 `ScalarBox` (NumberBox 别名)

```csharp
// Controls/ScalarBox.cs
namespace PrimalEditor_Avalonia.Controls;

/// <summary>XAML 端使用 ScalarBox 与 NumberBox 等价;语义上代表"单个标量"</summary>
public class ScalarBox : NumberBox { }
```

**Step 4:** 实现 `Controls.axaml` 主题

```xml
<!-- Controls/Controls.axaml -->
<Styles xmlns="https://github.com/avaloniaui"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        xmlns:ctrl="using:PrimalEditor_Avalonia.Controls">

  <Style Selector="ctrl|NumberBox">
    <Setter Property="Template">
      <ControlTemplate>
        <Border Background="#FF2D2D30" BorderBrush="#FF444" BorderThickness="1"
                CornerRadius="2" Padding="2">
          <Grid ColumnDefinitions="*,Auto" Height="22">
            <TextBlock Grid.Column="0"
                       Name="PART_TextBlock"
                       Text="{TemplateBinding Value}"
                       Foreground="#FFEEE"
                       VerticalAlignment="Center"
                       Padding="6,0"
                       TextAlignment="Right"
                       FontFamily="Consolas,Menlo,monospace"/>
            <TextBox Grid.Column="1"
                     Name="PART_TextBox"
                     Text="{TemplateBinding Value, Mode=TwoWay}"
                     Width="60"
                     IsVisible="False"
                     FontFamily="Consolas,Menlo,monospace"/>
          </Grid>
        </Border>
      </ControlTemplate>
    </Setter>
  </Style>

  <Style Selector="ctrl|VectorBox">
    <Setter Property="Template">
      <ControlTemplate>
        <StackPanel Orientation="{TemplateBinding Orientation}">
          <ctrl:NumberBox X="{TemplateBinding X}" Multiplier="{TemplateBinding Multiplier}" Kind="Float"/>
          <ctrl:NumberBox Y="{TemplateBinding Y}" Multiplier="{TemplateBinding Multiplier}" Kind="Float"
                          IsVisible="{Binding $self.IsVisible}"/>
          <!-- Z / W 按 Dimension 显示简化此处 -->
        </StackPanel>
      </ControlTemplate>
    </Setter>
  </Style>

</Styles>
```

**Step 5:** 把 `Controls.axaml` 注册进 `App.axaml`

```xml
<!-- App.axaml 的 <Application.Styles> 内追加: -->
<StyleInclude Source="avares:PrimalEditor_Avalonia/Controls/Controls.axaml"/>
```

**Step 6:** 编译 + 简单可视化验证
- 编译通过
- 在 `PropertiesView.axaml` 临时塞一个 `<ctrl:VectorBox X="0" Y="0" Z="0" />` 验证可见
- 拖动鼠标应改 X,松开提交 ValueCommitted

**Step 7:** Commit
```bash
git add PrimalEditor_Avalonia/Controls/
git add PrimalEditor_Avalonia/App.axaml
git commit -m "feat(controls): add VectorBox/NumberBox/ScalarBox with drag-to-edit support"
```

---

## Task 3: ComponentView + MSComponent 多选组件框架

**目标:** 给 Inspector 提供"多 entity 选中时,属性聚合显示 + 多值时显示 '-'"的标准容器。这一个抽象被 Transform、Mesh、Light 等所有组件复用。

**Files:**
- Create: `PrimalEditor_Avalonia/Components/MSComponent.cs` — `IMSComponent` 接口 + `MSComponent<T>` 抽象
- Create: `PrimalEditor_Avalonia/Components/MSEntity.cs` — 包装 `GameEntity[]`,提供统一的 SelectedEntities + Properties
- Create: `PrimalEditor_Avalonia/Components/MSTransform.cs` — Transform 多选版本(已有 PropertiesView,改为派生 MSComponent)
- Modify: `PrimalEditor_Avalonia/Components/Component.cs` — `Component` 改为实现 `IMSComponent.GetMultiselectionComponent(MSEntity)`
- Modify: `PrimalEditor_Avalonia/Components/Transform.cs` — 加 `GetMultiselectionComponent` 返回 `MSTransform`
- Modify: `PrimalEditor_Avalonia/Components/GameEntity.cs` — 加 `GetComponent<T>()`、`GetTransform()` 辅助方法
- Modify: `PrimalEditor_Avalonia/Views/PropertiesView.axaml.cs` — DataContext 接受 `MSEntity` 而非 `GameEntity[]`

**Step 1:** 实现 `MSComponent<T>`

```csharp
// Components/MSComponent.cs
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

public interface IMSComponent
{
    void Refresh();
}

/// <summary>
/// 多选组件的基类。SelectedComponents 自动从 MSEntity.SelectedEntities 提取。
/// 子类实现 UpdateMSComponent() 把多选值聚合成单值(null 表示多值)。
/// 子类实现 UpdateComponents(propertyName) 把 UI 改动广播回所有选中组件。
/// </summary>
public abstract class MSComponent<T> : ViewModelBase, IMSComponent where T : Component
{
    private bool _enableUpdates = true;
    protected MSEntity MSEntity { get; }
    public List<T> SelectedComponents { get; }

    protected abstract bool UpdateComponents(string propertyName);
    protected abstract bool UpdateMSComponent();
    protected void EnableUpdates(bool on) => _enableUpdates = on;

    protected MSComponent(MSEntity msEntity)
    {
        MSEntity = msEntity;
        SelectedComponents = msEntity.SelectedEntities
            .Select(e => e.GetComponent<T>()).Where(c => c != null).Cast<T>().ToList();
        PropertyChanged += (_, e) =>
        {
            if (_enableUpdates) UpdateComponents(e.PropertyName ?? "");
        };
    }

    public void Refresh()
    {
        EnableUpdates(false);
        UpdateMSComponent();
        EnableUpdates(true);
    }
}
```

**Step 2:** 实现 `MSEntity`

```csharp
// Components/MSEntity.cs
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

/// <summary>
/// 多选 entity 集合。Inspector 接收 MSEntity 后,根据 entity 上的 Component 列表
/// 自动创建对应的 MSComponent&lt;T&gt;,每种 component 一次显示。
/// </summary>
public class MSEntity : ViewModelBase
{
    private System.Guid _guid;
    public System.Guid Guid { get => _guid; set => _guid = value; }

    public ObservableCollection<GameEntity> SelectedEntities { get; } = new();

    public string Name =>
        SelectedEntities.Count == 0 ? "" :
        SelectedEntities.All(e => e.Name == SelectedEntities[0].Name) ? SelectedEntities[0].Name : "<multiple>";

    public bool IsEnabled
    {
        get => SelectedEntities.Count > 0 && SelectedEntities.All(e => e.IsEnabled);
        set { foreach (var e in SelectedEntities) e.IsEnabled = value; }
    }

    public MSEntity(System.Guid guid) { _guid = guid; }

    /// <summary>当 SelectedEntities 变化时,生成对应 MSComponent 给 View 绑定。</summary>
    public T? GetMSComponent<T>(IMSComponent? current) where T : IMSComponent
    {
        // 调用方负责构造合适的 MSComponent<T>;此方法仅刷新 SelectedComponents。
        if (current is MSComponent<Transform> t)
        {
            t.Refresh();
            return (T)(IMSComponent)t;
        }
        return current is T t2 ? t2 : default;
    }
}
```

**Step 3:** 实现 `MSTransform`

```csharp
// Components/MSTransform.cs
using System;
using System.Globalization;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

public class MSTransform : MSComponent<Transform>
{
    private float? _posX, _posY, _posZ, _rotX, _rotY, _rotZ, _scaleX, _scaleY, _scaleZ;

    public float? PosX    { get => _posX;    set { if (!IsSame(_posX, value))    { _posX    = value; OnPropertyChanged(); } } }
    public float? PosY    { get => _posY;    set { if (!IsSame(_posY, value))    { _posY    = value; OnPropertyChanged(); } } }
    public float? PosZ    { get => _posZ;    set { if (!IsSame(_posZ, value))    { _posZ    = value; OnPropertyChanged(); } } }
    public float? RotX    { get => _rotX;    set { if (!IsSame(_rotX, value))    { _rotX    = value; OnPropertyChanged(); } } }
    public float? RotY    { get => _rotY;    set { if (!IsSame(_rotY, value))    { _rotY    = value; OnPropertyChanged(); } } }
    public float? RotZ    { get => _rotZ;    set { if (!IsSame(_rotZ, value))    { _rotZ    = value; OnPropertyChanged(); } } }
    public float? ScaleX  { get => _scaleX;  set { if (!IsSame(_scaleX, value))  { _scaleX  = value; OnPropertyChanged(); } } }
    public float? ScaleY  { get => _scaleY;  set { if (!IsSame(_scaleY, value))  { _scaleY  = value; OnPropertyChanged(); } } }
    public float? ScaleZ  { get => _scaleZ;  set { if (!IsSame(_scaleZ, value))  { _scaleZ  = value; OnPropertyChanged(); } } }

    public MSTransform(MSEntity msEntity) : base(msEntity) { Refresh(); }

    protected override bool UpdateMSComponent()
    {
        if (SelectedComponents.Count == 0) return false;
        _posX   = Aggregate(t => t.PositionX);
        _posY   = Aggregate(t => t.PositionY);
        _posZ   = Aggregate(t => t.PositionZ);
        _rotX   = Aggregate(t => t.RotationX);
        _rotY   = Aggregate(t => t.RotationY);
        _rotZ   = Aggregate(t => t.RotationZ);
        _scaleX = Aggregate(t => t.ScaleX);
        _scaleY = Aggregate(t => t.ScaleY);
        _scaleZ = Aggregate(t => t.ScaleZ);
        OnPropertyChanged(nameof(PosX)); OnPropertyChanged(nameof(PosY)); OnPropertyChanged(nameof(PosZ));
        OnPropertyChanged(nameof(RotX)); OnPropertyChanged(nameof(RotY)); OnPropertyChanged(nameof(RotZ));
        OnPropertyChanged(nameof(ScaleX)); OnPropertyChanged(nameof(ScaleY)); OnPropertyChanged(nameof(ScaleZ));
        return true;
    }

    protected override bool UpdateComponents(string propertyName)
    {
        foreach (var t in SelectedComponents)
        {
            switch (propertyName)
            {
                case nameof(PosX):   t.PositionX = _posX ?? t.PositionX; break;
                case nameof(PosY):   t.PositionY = _posY ?? t.PositionY; break;
                case nameof(PosZ):   t.PositionZ = _posZ ?? t.PositionZ; break;
                case nameof(RotX):   t.RotationX = _rotX ?? t.RotationX; break;
                case nameof(RotY):   t.RotationY = _rotY ?? t.RotationY; break;
                case nameof(RotZ):   t.RotationZ = _rotZ ?? t.RotationZ; break;
                case nameof(ScaleX): t.ScaleX    = _scaleX ?? t.ScaleX; break;
                case nameof(ScaleY): t.ScaleY    = _scaleY ?? t.ScaleY; break;
                case nameof(ScaleZ): t.ScaleZ    = _scaleZ ?? t.ScaleZ; break;
            }
        }
        return true;
    }

    private float? Aggregate(Func<Transform, float> sel)
    {
        var first = SelectedComponents[0];
        var v = sel(first);
        foreach (var t in SelectedComponents)
            if (System.Math.Abs(sel(t) - v) > 1e-4f) return null;  // null = 多值显示 '-'
        return v;
    }

    private static bool IsSame(float? a, float? b)
    {
        if (a == null && b == null) return true;
        if (a == null || b == null) return false;
        return System.Math.Abs(a.Value - b.Value) < 1e-4f;
    }
}
```

**Step 4:** 改 `Component` / `Transform` / `GameEntity`

```csharp
// Components/Component.cs (加方法)
public abstract class Component : ViewModelBase
{
    [JsonPropertyName("IsEnabled")] public bool IsEnabled { get; set; } = true;
    public abstract IMSComponent GetMultiselectionComponent(MSEntity msEntity);
}

// Components/Transform.cs (加实现)
public override IMSComponent GetMultiselectionComponent(MSEntity msEntity) => new MSTransform(msEntity);

// Components/GameEntity.cs (加方法)
public T? GetComponent<T>() where T : Component =>
    Components.OfType<T>().FirstOrDefault();

public Transform? GetTransform() => GetComponent<Transform>();
```

**Step 5:** 改 `PropertiesViewModel` 接受 `MSEntity`

```csharp
// ViewModels/PropertiesViewModel.cs (核心改动)
- private GameEntity[] _entities = Array.Empty<GameEntity>();
+ private MSEntity? _msEntity;

+ public MSEntity? MSEntity
+ {
+     get => _msEntity;
+     set { _msEntity = value; OnPropertyChanged(nameof(MSEntity)); OnPropertyChanged(nameof(HasSelection)); }
+ }

- public GameEntity[] Entities { get; set; }  // 旧接口保留但内部转 MSEntity
+ public GameEntity[] Entities
+ {
+     set
+     {
+         var ms = new MSEntity(System.Guid.NewGuid());
+         foreach (var e in value) ms.SelectedEntities.Add(e);
+         MSEntity = ms;
+     }
+ }

+ public MSTransform? Transform => MSEntity?.SelectedEntities.FirstOrDefault()?.GetTransform() is { } t
+     ? new MSTransform(MSEntity!)
+     : null;
```

**Step 6:** 编译 + 验证
- `dotnet build` 通过
- PropertiesView 现在支持多选 entity 选中时,显示 `<multiple>` 或多值 `-`

**Step 7:** Commit
```bash
git add PrimalEditor_Avalonia/Components/
git add PrimalEditor_Avalonia/ViewModels/PropertiesViewModel.cs
git commit -m "feat(components): add MSEntity/MSTransform multi-selection component framework"
```

---

## Task 4: IAssetEditor 抽象 + 双击路由

**目标:** 双击 Content Browser 中的 Asset 时,根据 AssetType 打开对应的编辑器(Tab 形式)。为 Texture/Geometry/Mesh 编辑器提供统一入口。

**Files:**
- Create: `PrimalEditor_Avalonia/Editors/IAssetEditor.cs` — 接口 + `AssetEditorState` 枚举
- Create: `PrimalEditor_Avalonia/Editors/AssetEditorRegistry.cs` — 按 AssetType 注册 ViewModel 工厂
- Create: `PrimalEditor_Avalonia/Editors/AssetEditorHost.cs` — Avalonia Window 容器,接收 `IAssetEditor` 显示
- Modify: `PrimalEditor_Avalonia/Views/ContentBrowserView.axaml.cs` — `OnFileDoubleClick` 改为路由到 `AssetEditorRegistry.Open(info)`
- Modify: `PrimalEditor_Avalonia/ViewModels/MainWindowViewModel.cs` — 增加 `OpenAssetEditorCommand` 与 `EditorTabs` ObservableCollection

**Step 1:** 实现 `IAssetEditor`

```csharp
// Editors/IAssetEditor.cs
using System;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Editors;

public enum AssetEditorState { Done, Importing, Processing, Loading, Saving }

public interface IAssetEditor
{
    AssetEditorState State { get; }
    Guid  AssetGuid { get; }
    AssetInfo? Asset { get; }
    string Title { get; }
    void SetAsset(AssetInfo info);
}

/// <summary>通用 AssetEditor ViewModel 基类。</summary>
public abstract class AssetEditorViewModel : ViewModelBase, IAssetEditor
{
    private AssetEditorState _state = AssetEditorState.Done;
    public AssetEditorState State { get => _state; set => SetProperty(ref _state, value); }

    public Guid AssetGuid { get; private set; }
    public AssetInfo? Asset { get; private set; }
    public virtual string Title => Asset?.FileName ?? "(no asset)";

    public virtual void SetAsset(AssetInfo info)
    {
        Asset = info;
        AssetGuid = info.Guid;
        OnPropertyChanged(nameof(Title));
    }

    protected bool SetProperty<T>(ref T field, T value, System.Action? onChanged = null)
    {
        if (System.Collections.Generic.EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        onChanged?.Invoke();
        OnPropertyChanged();
        return true;
    }
}
```

**Step 2:** 实现 `AssetEditorRegistry`

```csharp
// Editors/AssetEditorRegistry.cs
using System;
using System.Collections.Generic;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Editors;

/// <summary>
/// 按 AssetType 注册 AssetEditorViewModel 工厂。
/// AssetEditorHost 在用户双击 Asset 时通过 Open() 拿到对应 VM 并打开为 Window/Tab。
/// </summary>
public static class AssetEditorRegistry
{
    private static readonly Dictionary<AssetType, Func<AssetInfo, AssetEditorViewModel>> _factories = new();
    private static readonly List<AssetEditorHost> _openWindows = new();

    public static System.Collections.ObjectModel.ReadOnlyCollection<AssetEditorHost> OpenWindows =>
        _openWindows.AsReadOnly();

    public static void Register(AssetType type, Func<AssetInfo, AssetEditorViewModel> factory)
    {
        _factories[type] = factory;
        Logger.Info($"AssetEditorRegistry: registered editor for {type}");
    }

    public static AssetEditorHost? Open(AssetInfo info)
    {
        if (!_factories.TryGetValue(info.Type, out var factory))
        {
            Logger.Warning($"AssetEditorRegistry: no editor for {info.Type} ({info.FileName})");
            return null;
        }
        var vm = factory(info);
        vm.SetAsset(info);
        var host = new AssetEditorHost(vm);
        _openWindows.Add(host);
        host.Closed += (_, _) => _openWindows.Remove(host);
        host.Show();
        Logger.Info($"AssetEditorRegistry: opened {info.Type} editor for {info.FileName}");
        return host;
    }
}
```

**Step 3:** 实现 `AssetEditorHost`

```csharp
// Editors/AssetEditorHost.cs
using Avalonia.Controls;
using PrimalEditor_Avalonia.Views;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Editors;

/// <summary>
/// 编辑器窗口容器:Header 显示资产名 + State,Body 留给具体编辑器 View。
/// 子类通过重写 BuildView() 返回具体 View(UserControl)。
/// </summary>
public class AssetEditorHost : Window
{
    public AssetEditorViewModel ViewModel { get; }

    public AssetEditorHost(AssetEditorViewModel vm)
    {
        ViewModel = vm;
        DataContext = vm;
        Title = vm.Title + " — PrimalEditor";
        Width = 800; Height = 600;
        Content = BuildContent(vm);
    }

    private object BuildContent(AssetEditorViewModel vm) => vm switch
    {
        // 后续 Task 7/8 注册时填: TextureEditorViewModel / GeometryEditorViewModel / PrimitiveMeshViewModel
        _ => new TextBlock { Text = $"Editor for {vm.GetType().Name} not yet implemented" }
    };
}
```

**Step 4:** 修改 `ContentBrowserView` 双击路由

```csharp
// Views/ContentBrowserView.axaml.cs
private void OnFileDoubleClick(object? sender, Avalonia.Input.TappedEventArgs e)
{
    if (DataContext is ContentBrowserViewModel vm && vm.SelectedFile is AssetInfo info)
    {
        // 优先 AssetEditor 路由;无注册时回退到系统打开
        if (PrimalEditor_Avalonia.Editors.AssetEditorRegistry.Open(info) == null)
            vm.OpenSelectedFile();
    }
}
```

**Step 5:** 在 `Program.cs` 注册内置编辑器(占位,真正实现在 T7/T8)

```csharp
// Program.cs (BuildAvaloniaApp 后追加)
Editors.AssetEditorRegistry.Register(Content.AssetType.Texture,
    info => new Editors.TextureEditorViewModel());
Editors.AssetEditorRegistry.Register(Content.AssetType.Mesh,
    info => new Editors.GeometryEditorViewModel());
```

TextureEditorViewModel / GeometryEditorViewModel 在 T7/T8 实现,这里引用要保证后续任务完成后才有。

**Step 6:** 编译验证(此时 Texture/Geometry VM 还是空类,仅占位)
- `dotnet build` 应通过
- 双击 .png/.fbx 不再调用 Process.Start,而是弹出空 editor window

**Step 7:** Commit
```bash
git add PrimalEditor_Avalonia/Editors/
git add PrimalEditor_Avalonia/Views/ContentBrowserView.axaml.cs
git commit -m "feat(editors): add IAssetEditor + AssetEditorRegistry routing for double-click"
```

---

## Task 5: Debug/Release 配置下拉框占位

**目标:** WorldEditor 顶部加 Debug/Release 选择下拉框(Build 按钮暂不接真实构建,仅占位 + 写入状态栏)。

**Files:**
- Modify: `PrimalEditor_Avalonia/GameProject/Project.cs` — 增加 `BuildConfigName`(已存在 `BuildConfig` int,改为带名字) + `BuildConfigurations` 静态列表
- Modify: `PrimalEditor_Avalonia/Views/WorldEditorView.axaml` — 顶部 toolbar 增加 `<ComboBox>` + `<Button Content="Build">`
- Modify: `PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs` — `BuildProjectCommand`(RelayCommand,仅写日志)

**Step 1:** 扩 `Project.BuildConfig` 枚举绑定

```csharp
// GameProject/Project.cs (追加)
[JsonIgnore] public static readonly string[] BuildConfigurationNames = { "Debug", "DebugEditor", "Release", "ReleaseEditor" };

[JsonIgnore]
public string BuildConfigName
{
    get => BuildConfigurationNames[System.Math.Clamp(BuildConfig, 0, BuildConfigurationNames.Length - 1)];
    set
    {
        var idx = System.Array.IndexOf(BuildConfigurationNames, value);
        if (idx >= 0) BuildConfig = idx;
    }
}
```

**Step 2:** `WorldEditorViewModel` 加 Build 命令

```csharp
// ViewModels/WorldEditorViewModel.cs (追加)
public System.Windows.Input.ICommand BuildProjectCommand { get; }
// 构造函数内:
BuildProjectCommand = new RelayCommand(_ =>
{
    PrimalEditor_Avalonia.Common.Logger.Info(
        $"[Build] Placeholder — would build project in '{Project?.BuildConfigName}' config. " +
        "(VS integration not yet wired in Avalonia build.)");
});
```

**Step 3:** `WorldEditorView.axaml` 工具栏

在 `<views:EngineViewport3D>` 上面增加一个 Toolbar `StackPanel`:

```xml
<!-- Views/WorldEditorView.axaml 在 Grid.Row=1(Scene 上方)加: -->
<Border Grid.Row="0" Background="#FF2D2D30" Padding="5">
  <StackPanel Orientation="Horizontal" Spacing="6">
    <TextBlock Text="Build:" VerticalAlignment="Center" Foreground="#FFEEE"/>
    <ComboBox Width="120"
              ItemsSource="{Binding BuildConfigurations}"
              SelectedItem="{Binding Project.BuildConfigName}"/>
    <Button Content="Build" Command="{Binding BuildProjectCommand}" Padding="10,2"/>
    <Button Content="Primitive Mesh…" Click="OnCreatePrimitiveMeshClick" Padding="10,2"
            IsEnabled="{Binding Project, Converter={x:Static ObjectConverters.IsNotNull}}"/>
  </StackPanel>
</Border>
```

`BuildConfigurations` 静态属性在 WorldEditorViewModel 暴露 `Project.BuildConfigurationNames`。

**Step 4:** 在 `WorldEditorView.axaml.cs` 加占位 handler(Primitive Mesh 真实实现在 T6)

```csharp
private void OnCreatePrimitiveMeshClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
{
    // 占位;T6 替换为真正的 PrimitiveMeshDialog.ShowDialog(...)
    PrimalEditor_Avalonia.Common.Logger.Info("Primitive Mesh dialog not yet implemented (T6).");
}
```

**Step 5:** 编译验证
- `dotnet build` 通过
- WorldEditor 顶部出现 Toolbar,ComboBox 4 个选项可选,点 Build 写日志

**Step 6:** Commit
```bash
git add PrimalEditor_Avalonia/GameProject/Project.cs
git add PrimalEditor_Avalonia/Views/WorldEditorView.axaml
git add PrimalEditor_Avalonia/Views/WorldEditorView.axaml.cs
git add PrimalEditor_Avalonia/ViewModels/WorldEditorViewModel.cs
git commit -m "feat(worldeditor): add Debug/Release build config combobox + placeholder Build button"
```

---

## Task 6: Primitive Mesh 创建对话框

**目标:** 完整的 Primitive Mesh 创建 UI — 6 种几何体(Plane/Cube/UvSphere/IcoSphere/Cylinder/Capsule),每个有段数 + 尺寸 + LOD 输入。

**Files:**
- Create: `PrimalEditor_Avalonia/Editors/PrimitiveMesh/PrimitiveMeshType.cs` — 枚举(6 种)
- Create: `PrimalEditor_Avalonia/Editors/PrimitiveMesh/PrimitiveInitInfo.cs` — 数据类
- Create: `PrimalEditor_Avalonia/Editors/PrimitiveMesh/PrimitiveMeshViewModel.cs` — ViewModel:参数收集 + 调 NativeEngineBridge.ProceduralMeshCreate
- Create: `PrimalEditor_Avalonia/Editors/PrimitiveMesh/PrimitiveMeshDialog.axaml(.cs)` — Avalonia Window

依赖:NumberBox/VectorBox (T2),NativeEngineBridge.ProceduralMeshCreate (已有)

**Step 1:** 枚举 + 数据类

```csharp
// Editors/PrimitiveMesh/PrimitiveMeshType.cs
namespace PrimalEditor_Avalonia.Editors.PrimitiveMesh;

public enum PrimitiveMeshType
{
    Plane,
    Cube,
    UvSphere,
    IcoSphere,
    Cylinder,
    Capsule,
}

// Editors/PrimitiveMesh/PrimitiveInitInfo.cs
public class PrimitiveInitInfo
{
    public PrimitiveMeshType Type { get; set; }
    public int SegmentsX { get; set; } = 1;
    public int SegmentsY { get; set; } = 1;
    public int SegmentsZ { get; set; } = 1;
    public float SizeX { get; set; } = 1f;
    public float SizeY { get; set; } = 1f;
    public float SizeZ { get; set; } = 1f;
    public int LOD { get; set; } = 0;
    public float SmoothingAngle { get; set; } = 178f;
}
```

**Step 2:** ViewModel

```csharp
// Editors/PrimitiveMesh/PrimitiveMeshViewModel.cs
using System.Collections.Generic;
using System.IO;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Editors.PrimitiveMesh;

public class PrimitiveMeshViewModel : AssetEditorViewModel
{
    private PrimitiveMeshType _type = PrimitiveMeshType.Plane;
    public PrimitiveMeshType Type { get => _type; set { SetProperty(ref _type, value); OnTypeChanged(); } }

    public int   SegmentsX { get; set; } = 1;
    public int   SegmentsY { get; set; } = 1;
    public int   SegmentsZ { get; set; } = 1;
    public float SizeX     { get; set; } = 1f;
    public float SizeY     { get; set; } = 1f;
    public float SizeZ     { get; set; } = 1f;
    public int   LOD       { get; set; } = 0;
    public float SmoothingAngle { get; set; } = 178f;

    public IReadOnlyList<PrimitiveMeshType> AllTypes { get; } =
        System.Enum.GetValues<PrimitiveMeshType>();

    public string OutputPath { get; set; } = string.Empty;
    public string FileName   { get; set; } = "Primitive_Mesh";

    private void OnTypeChanged()
    {
        // 默认参数随类型变化(WPF 行为)
        switch (Type)
        {
            case PrimitiveMeshType.Plane:     SegmentsX = SegmentsZ = 1; SizeX = SizeZ = 10f; SizeY = 0f; break;
            case PrimitiveMeshType.Cube:      SegmentsX = SegmentsY = SegmentsZ = 1; SizeX = SizeY = SizeZ = 1f; break;
            case PrimitiveMeshType.UvSphere:  SegmentsX = SegmentsY = 16; SizeX = SizeY = SizeZ = 1f; break;
            case PrimitiveMeshType.IcoSphere: LOD = 1; SizeX = SizeY = SizeZ = 1f; break;
            case PrimitiveMeshType.Cylinder:  SegmentsX = SegmentsY = 16; SizeX = SizeY = SizeZ = 1f; break;
            case PrimitiveMeshType.Capsule:   SegmentsX = SegmentsY = 16; SizeX = SizeY = SizeZ = 1f; break;
        }
        OnPropertyChanged(nameof(SegmentsX)); OnPropertyChanged(nameof(SegmentsY));
        OnPropertyChanged(nameof(SegmentsZ)); OnPropertyChanged(nameof(SizeX));
        OnPropertyChanged(nameof(SizeY));     OnPropertyChanged(nameof(SizeZ));
        OnPropertyChanged(nameof(LOD));
    }

    public bool Create(string projectContentPath)
    {
        var info = new PrimitiveInitInfo
        {
            Type = Type, SegmentsX = SegmentsX, SegmentsY = SegmentsY, SegmentsZ = SegmentsZ,
            SizeX = SizeX, SizeY = SizeY, SizeZ = SizeZ, LOD = LOD, SmoothingAngle = SmoothingAngle,
        };
        // 调 native 烘焙
        var meshId = NativeEngineBridge.ProceduralMeshCreate(
            (uint)Type,
            new[] { info.SizeX, info.SizeY, info.SizeZ, info.SegmentsX, info.SegmentsY, info.SegmentsZ, info.LOD, info.SmoothingAngle });
        if (meshId == NativeEngineBridge.InvalidContentId)
        {
            Logger.Error($"PrimitiveMeshCreate failed for {Type}");
            return false;
        }
        // 写 .mesh + .primalasset 元数据
        var fileName = $"{FileName}.mesh";
        var fullPath = Path.Combine(projectContentPath, fileName);
        Directory.CreateDirectory(projectContentPath);
        // mesh binary 直接 cook(依赖 NativeEngineBridge.CookPrimitive)
        if (!NativeEngineBridge.CookPrimitiveMesh(meshId, fullPath))
        {
            Logger.Error($"Failed to cook primitive mesh to {fullPath}");
            return false;
        }
        AssetRegistry.Register(fullPath);
        Logger.Info($"Created primitive mesh: {fullPath}");
        return true;
    }
}
```

**Step 3:** Avalonia Window

```xml
<!-- Editors/PrimitiveMesh/PrimitiveMeshDialog.axaml -->
<Window xmlns="https://github.com/avaloniaui"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        xmlns:vm="using:PrimalEditor_Avalonia.Editors.PrimitiveMesh"
        xmlns:ctrl="using:PrimalEditor_Avalonia.Controls"
        x:Class="PrimalEditor_Avalonia.Editors.PrimitiveMesh.PrimitiveMeshDialog"
        x:DataType="vm:PrimitiveMeshViewModel"
        Title="Primitive Mesh" Width="500" Height="600"
        WindowStartupLocation="CenterOwner">
  <Grid Margin="15" RowDefinitions="Auto,*,Auto" ColumnDefinitions="*">

    <StackPanel Grid.Row="0">
      <TextBlock Text="Type" Margin="0,4"/>
      <ComboBox ItemsSource="{Binding AllTypes}" SelectedItem="{Binding Type}" Width="200" HorizontalAlignment="Left"/>

      <TextBlock Text="Segments X / Y / Z" Margin="0,8,0,4"/>
      <StackPanel Orientation="Horizontal" Spacing="4">
        <ctrl:NumberBox Value="{Binding SegmentsX, Mode=TwoWay, Converter={x:Static ctrl:IntStringConverter.Instance}}" Kind="Int" Width="80"/>
        <ctrl:NumberBox Value="{Binding SegmentsY, Mode=TwoWay, Converter={x:Static ctrl:IntStringConverter.Instance}}" Kind="Int" Width="80"/>
        <ctrl:NumberBox Value="{Binding SegmentsZ, Mode=TwoWay, Converter={x:Static ctrl:IntStringConverter.Instance}}" Kind="Int" Width="80"/>
      </StackPanel>

      <TextBlock Text="Size X / Y / Z" Margin="0,8,0,4"/>
      <StackPanel Orientation="Horizontal" Spacing="4">
        <ctrl:NumberBox Value="{Binding SizeX, Mode=TwoWay}" Kind="Float" Width="80"/>
        <ctrl:NumberBox Value="{Binding SizeY, Mode=TwoWay}" Kind="Float" Width="80"/>
        <ctrl:NumberBox Value="{Binding SizeZ, Mode=TwoWay}" Kind="Float" Width="80"/>
      </StackPanel>

      <TextBlock Text="LOD" Margin="0,8,0,4"/>
      <ctrl:NumberBox Value="{Binding LOD, Mode=TwoWay, Converter={x:Static ctrl:IntStringConverter.Instance}}" Kind="Int" Width="80"/>
    </StackPanel>

    <StackPanel Grid.Row="2" Orientation="Horizontal" HorizontalAlignment="Right" Spacing="6" Margin="0,15,0,0">
      <Button Content="Cancel" Click="OnCancelClick" Width="80"/>
      <Button Content="Create" Click="OnCreateClick" Width="80" IsDefault="True"/>
    </StackPanel>
  </Grid>
</Window>
```

`IntStringConverter` 是双向转换器(int ↔ string),放到 Controls/Converters.cs。

**Step 4:** Code-behind

```csharp
// Editors/PrimitiveMesh/PrimitiveMeshDialog.axaml.cs
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia.Editors.PrimitiveMesh;

public partial class PrimitiveMeshDialog : Window
{
    public PrimitiveMeshDialog()
    {
        InitializeComponent();
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private void OnCancelClick(object? s, Avalonia.Interactivity.RoutedEventArgs e) => Close(null);

    private void OnCreateClick(object? s, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not PrimitiveMeshViewModel vm) return;
        var topLevel = TopLevel.GetTopLevel(this);
        // 通过 Project.Current.ContentPath 找到路径
        var main = (topLevel as Window)?.Owner;
        var project = PrimalEditor_Avalonia.GameProject.Project.Current
            ?? PrimalEditor_Avalonia.ViewModels.MainWindowViewModel.ActiveProject;
        if (project == null) { Close(false); return; }
        var ok = vm.Create(project.ContentPath);
        Close(ok);
    }
}
```

**Step 5:** 在 WorldEditor 工具栏按钮接通

```csharp
// Views/WorldEditorView.axaml.cs
private async void OnCreatePrimitiveMeshClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
{
    var dlg = new PrimitiveMeshDialog { DataContext = new PrimitiveMeshViewModel() };
    var window = TopLevel.GetTopLevel(this) as Window;
    if (window != null) await dlg.ShowDialog<bool?>(window);
}
```

**Step 6:** 给 `NativeEngineBridge` 加 `CookPrimitiveMesh` 占位(若不存在)

```csharp
// Native/NativeEngineBridge.cs (追加)
[DllImport("PrimalEngine", EntryPoint = "CookPrimitiveMesh", CallingConvention = CallingConvention.Cdecl)]
private static extern uint _CookPrimitiveMesh(UInt64 meshId, IntPtr path);

public static bool CookPrimitiveMesh(UInt64 meshId, string path)
{
    if (!_isLoaded) return false;
    try
    {
        var p = MarshalUtf8(path);
        return _CookPrimitiveMesh(meshId, p) != 0;
    }
    catch (Exception ex) { Logger.Error("CookPrimitiveMesh failed", ex); return false; }
}
```

如果 C++ 端没有 `CookPrimitiveMesh` 函数,**这次 plan 暂时接受失败**:UI 仍然打开,Create 时写日志 "CookPrimitiveMesh not yet implemented in C++ engine"。**这是已知的 follow-up**。

**Step 7:** 编译验证 + UI smoke test
- `dotnet build` 通过
- 工具栏点 "Primitive Mesh…" 弹窗,6 种可选,参数同步显示,Create 触发 (可能写错误日志但 UI 通)

**Step 8:** Commit
```bash
git add PrimalEditor_Avalonia/Editors/PrimitiveMesh/
git add PrimalEditor_Avalonia/Views/WorldEditorView.axaml.cs
git add PrimalEditor_Avalonia/Native/NativeEngineBridge.cs
git commit -m "feat(primitive-mesh): add full primitive mesh creation dialog with 6 geometry types"
```

---

## Task 7: Texture Editor 完整移植

**目标:** 完整的 Texture 编辑器:Array/Mip/Depth 选择、RGBA 通道切换、NormalMap 模式、Reimport/Save。

**Files:**
- Create: `PrimalEditor_Avalonia/Editors/Texture/TextureImportSettings.cs` — 导入设置数据
- Create: `PrimalEditor_Avalonia/Editors/Texture/TextureSlice.cs` — Slice 类
- Create: `PrimalEditor_Avalonia/Editors/Texture/TextureInfo.cs` — TextureInfo 元数据
- Create: `PrimalEditor_Avalonia/Editors/Texture/TextureEditorViewModel.cs` — ViewModel
- Create: `PrimalEditor_Avalonia/Editors/Texture/TextureEditorView.axaml(.cs)` — Avalonia View
- Modify: `PrimalEditor_Avalonia/Editors/AssetEditorHost.cs` — `BuildContent` 识别 `TextureEditorViewModel` 返回 `TextureEditorView`
- Modify: `PrimalEditor_Avalonia/Editors/AssetEditorRegistry.cs` — 已注册,这里只改映射

依赖:AssetInfo (T1), IAssetEditor (T4), VectorBox (T2)

**Step 1:** Slice / Settings / Info

```csharp
// Editors/Texture/TextureSlice.cs
public class TextureSlice
{
    public int Width { get; set; }
    public int Height { get; set; }
    public int RowPitch { get; set; }
    public int SlicePitch { get; set; }
    public byte[] RawContent { get; set; } = Array.Empty<byte>();
}

// Editors/Texture/TextureImportSettings.cs
public enum TextureDimension { Texture2D, TextureCube, Texture3D }
public enum TextureFormat   { R8G8B8A8_UNORM, BC7_UNORM, BC1_UNORM, BC3_UNORM, BC5_UNORM }

public class TextureImportSettings : IAssetImportSettings
{
    public List<string> Sources { get; set; } = new();
    public TextureDimension Dimension { get; set; } = TextureDimension.Texture2D;
    public int MipLevels { get; set; } = 0;   // 0 = auto
    public float AlphaThreshold { get; set; } = 0.5f;
    public bool PreferBC7 { get; set; } = true;
    public TextureFormat OutputFormat { get; set; } = TextureFormat.BC7_UNORM;
    public bool Compress { get; set; } = true;

    public void ToBinary(BinaryWriter w)
    {
        w.Write(Sources.Count);
        foreach (var s in Sources) w.Write(s);
        w.Write((int)Dimension);
        w.Write(MipLevels);
        w.Write(AlphaThreshold);
        w.Write(PreferBC7);
        w.Write((int)OutputFormat);
        w.Write(Compress);
    }
    public void FromBinary(BinaryReader r)
    {
        var n = r.ReadInt32();
        Sources.Clear();
        for (var i = 0; i < n; ++i) Sources.Add(r.ReadString());
        Dimension = (TextureDimension)r.ReadInt32();
        MipLevels = r.ReadInt32();
        AlphaThreshold = r.ReadSingle();
        PreferBC7 = r.ReadBoolean();
        OutputFormat = (TextureFormat)r.ReadInt32();
        Compress = r.ReadBoolean();
    }
}

// Editors/Texture/TextureInfo.cs
public enum TextureFlags { None = 0, IsVolumeMap = 1, IsCubeMap = 2 }

public class TextureInfo
{
    public int Width { get; set; }
    public int Height { get; set; }
    public int ArraySize { get; set; } = 1;
    public int MipLevels { get; set; } = 1;
    public int Depth { get; set; } = 1;
    public TextureFormat Format { get; set; }
    public TextureFlags Flags { get; set; }
    public bool IsNormalMap { get; set; }
}
```

**Step 2:** ViewModel 核心

```csharp
// Editors/Texture/TextureEditorViewModel.cs
using System.Collections.ObjectModel;
using Avalonia.Media.Imaging;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Editors.Texture;

public class TextureEditorViewModel : AssetEditorViewModel
{
    private readonly TextureImportSettings _settings = new();
    private List<List<List<TextureSlice>>> _slices = new();
    private List<List<List<Bitmap?>>> _bitmaps = new();
    public TextureImportSettings Settings => _settings;
    public TextureInfo Info { get; private set; } = new();

    public int ArrayIndex { get; set; }
    public int MipIndex   { get; set; }
    public int DepthIndex { get; set; }

    public int MaxArrayIndex => Math.Max(0, _bitmaps.Count - 1);
    public int MaxMipIndex   => Math.Max(0, (_bitmaps.FirstOrDefault()?.Count ?? 1) - 1);
    public int MaxDepthIndex => Math.Max(0,
        (_bitmaps.ElementAtOrDefault(ArrayIndex)?.ElementAtOrDefault(MipIndex)?.Count ?? 1) - 1);

    public Bitmap? SelectedBitmap =>
        _bitmaps.ElementAtOrDefault(ArrayIndex)?.ElementAtOrDefault(MipIndex)?.ElementAtOrDefault(DepthIndex);

    // Channel 切换
    private bool _r = true, _g = true, _b = true, _a = true;
    public bool ShowR { get => _r; set { _r = value; UpdateBitmapChannels(); } }
    public bool ShowG { get => _g; set { _g = value; UpdateBitmapChannels(); } }
    public bool ShowB { get => _b; set { _b = value; UpdateBitmapChannels(); } }
    public bool ShowA { get => _a; set { _a = value; UpdateBitmapChannels(); } }
    public bool IsNormalMap { get; set; }

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        State = AssetEditorState.Loading;
        Task.Run(() => LoadTexture(info.FullPath))
            .ContinueWith(_ => State = AssetEditorState.Done);
    }

    private void LoadTexture(string path)
    {
        if (!File.Exists(path)) { Logger.Error($"Texture not found: {path}"); return; }

        // 解析 PNG/JPG → 解码为 RGBA8 bytes
        var imageData = TryLoadBitmap(path);
        if (imageData == null) { Logger.Error($"Failed to decode: {path}"); return; }

        // 简化为单 Array/Mip/Depth — 真实项目需 mipmap chain
        var slice = new TextureSlice
        {
            Width = imageData.Value.width,
            Height = imageData.Value.height,
            RowPitch = imageData.Value.width * 4,
            SlicePitch = imageData.Value.width * imageData.Value.height * 4,
            RawContent = imageData.Value.rgba,
        };
        _slices = new() { new() { new() { slice } } };
        _bitmaps = new() { new() { new() { BitmapFromSlice(slice, IsNormalMap) } } };

        Info = new TextureInfo
        {
            Width = slice.Width, Height = slice.Height,
            ArraySize = 1, MipLevels = 1, Depth = 1,
        };

        Avalonia.Threading.Dispatcher.UIThread.Post(() =>
        {
            OnPropertyChanged(nameof(MaxArrayIndex));
            OnPropertyChanged(nameof(MaxMipIndex));
            OnPropertyChanged(nameof(MaxDepthIndex));
            OnPropertyChanged(nameof(SelectedBitmap));
            OnPropertyChanged(nameof(Info));
        });
    }

    private void UpdateBitmapChannels()
    {
        if (_bitmaps.Count == 0) return;
        var slice = _slices[ArrayIndex][MipIndex][DepthIndex];
        var bmp = BitmapFromSlice(slice, IsNormalMap, ShowR, ShowG, ShowB, ShowA);
        _bitmaps[ArrayIndex][MipIndex][DepthIndex] = bmp;
        OnPropertyChanged(nameof(SelectedBitmap));
    }

    /// <summary>把 RGBA8 slice → Avalonia Bitmap。</summary>
    private static Bitmap? BitmapFromSlice(TextureSlice s, bool isNormal, bool r=true, bool g=true, bool b=true, bool a=true)
    {
        var bytes = new byte[s.Width * s.Height * 4];
        for (int i = 0; i < bytes.Length; i += 4)
        {
            var sr = s.RawContent[i+0], sg = s.RawContent[i+1], sb = s.RawContent[i+2], sa = s.RawContent[i+3];
            if (isNormal)
            {
                // 法线图重映射 (0,1) → (-1,1) → (0,255)
                sr = (byte)((sr * 2) - 255 < 0 ? 0 : (sr * 2 - 255));
                sg = (byte)((sg * 2) - 255 < 0 ? 0 : (sg * 2 - 255));
                sb = (byte)((sb * 2) - 255 < 0 ? 0 : (sb * 2 - 255));
            }
            bytes[i+0] = r ? sr : (byte)0;
            bytes[i+1] = g ? sg : (byte)0;
            bytes[i+2] = b ? sb : (byte)0;
            bytes[i+3] = a ? sa : (byte)255;
        }
        return new Bitmap(new Avalonia.Media.PixelBitmap(s.Width, s.Height,
            new Avalonia.Vector(s.Width, s.Height),
            Avalonia.Platform.PixelFormat.Rgba8888,
            Avalonia.Platform.AlphaFormat.Premul,
            bytes));
    }

    private static (int width, int height, byte[] rgba)? TryLoadBitmap(string path)
    {
        try
        {
            var bmp = new Bitmap(path);
            int w = bmp.PixelSize.Width, h = bmp.PixelSize.Height;
            var rgba = new byte[w * h * 4];
            // Avalonia 11: 用 bmp.CopyPixels + PixelFormat.Rgba8888
            var stride = w * 4;
            unsafe { fixed (byte* p = rgba) { bmp.CopyPixels(new Avalonia.PixelRect(0, 0, w, h), (IntPtr)p, rgba.Length, stride); } }
            return (w, h, rgba);
        }
        catch (Exception ex) { Logger.Error("Decode failed", ex); return null; }
    }
}
```

> **安全注意:** `unsafe` block 需要在 `.csproj` 启用 `<AllowUnsafeBlocks>true</AllowUnsafeBlocks>`。若不想开启 unsafe,改用 `bmp.CopyPixels` 的托管 API 或 `System.Drawing.Common`(仅在 Linux/Mac 需要 SkiaSharp)。

**Step 3:** View

```xml
<!-- Editors/Texture/TextureEditorView.axaml -->
<UserControl xmlns="https://github.com/avaloniaui"
             xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
             xmlns:vm="using:PrimalEditor_Avalonia.Editors.Texture"
             x:Class="PrimalEditor_Avalonia.Editors.Texture.TextureEditorView"
             x:DataType="vm:TextureEditorViewModel">
  <Grid RowDefinitions="Auto,*,Auto">
    <!-- 顶部工具栏: 通道切换 + Array/Mip/Depth 选择 -->
    <StackPanel Grid.Row="0" Orientation="Horizontal" Spacing="6" Margin="6">
      <CheckBox Content="R" IsChecked="{Binding ShowR}"/>
      <CheckBox Content="G" IsChecked="{Binding ShowG}"/>
      <CheckBox Content="B" IsChecked="{Binding ShowB}"/>
      <CheckBox Content="A" IsChecked="{Binding ShowA}"/>
      <CheckBox Content="Normal Map" IsChecked="{Binding IsNormalMap}"/>
      <Separator/>
      <TextBlock Text="Array:" VerticalAlignment="Center"/>
      <NumericUpDown Value="{Binding ArrayIndex}" Minimum="0" Maximum="{Binding MaxArrayIndex}" Width="80"/>
      <TextBlock Text="Mip:" VerticalAlignment="Center"/>
      <NumericUpDown Value="{Binding MipIndex}" Minimum="0" Maximum="{Binding MaxMipIndex}" Width="80"/>
      <TextBlock Text="Depth:" VerticalAlignment="Center"/>
      <NumericUpDown Value="{Binding DepthIndex}" Minimum="0" Maximum="{Binding MaxDepthIndex}" Width="80"/>
    </StackPanel>

    <!-- 中间预览 -->
    <Border Grid.Row="1" Background="#FF1E1E1E">
      <Image Source="{Binding SelectedBitmap}" Stretch="Uniform"/>
    </Border>

    <!-- 底部: 信息 + Reimport/Save -->
    <Grid Grid.Row="2" ColumnDefinitions="*,Auto,Auto" Margin="6">
      <TextBlock Grid.Column="0"
                 Text="{Binding Info, Converter={x:Static vm:TextureInfoFormatter.Instance}}"
                 FontFamily="Consolas,Menlo,monospace"/>
      <Button Grid.Column="1" Content="Reimport" Margin="4,0" Click="OnReimportClick"/>
      <Button Grid.Column="2" Content="Save" Click="OnSaveClick"/>
    </Grid>
  </Grid>
</UserControl>
```

`TextureInfoFormatter` 是简单 ToString 转换器。

**Step 4:** AssetEditorHost 接入

```csharp
// Editors/AssetEditorHost.cs (改 BuildContent)
private object BuildContent(AssetEditorViewModel vm) => vm switch
{
    Editors.Texture.TextureEditorViewModel      => new Editors.Texture.TextureEditorView { DataContext = vm },
    Editors.Geometry.GeometryEditorViewModel    => new Editors.Geometry.GeometryEditorView { DataContext = vm },
    _ => new TextBlock { Text = $"Editor for {vm.GetType().Name} not yet implemented" }
};
```

**Step 5:** Code-behind Reimport/Save

```csharp
// Editors/Texture/TextureEditorView.axaml.cs
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Native;

namespace PrimalEditor_Avalonia.Editors.Texture;

public partial class TextureEditorView : UserControl
{
    public TextureEditorView() { InitializeComponent(); }
    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    private void OnReimportClick(object? s, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is TextureEditorViewModel vm && vm.Asset != null)
        {
            Logger.Info($"Reimport: {vm.Asset.FullPath} — pushing settings + re-cook");
            // 真实路径:NativeEngineBridge.CookTexture(vm.Asset.FullPath, vm.Settings)
            vm.SetAsset(vm.Asset);
        }
    }
    private void OnSaveClick(object? s, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is TextureEditorViewModel vm && vm.Asset != null)
        {
            AssetImportSettingsExtensions.CopyImportSettings(vm.Settings, vm.Settings);
            Logger.Info($"Saved settings for {vm.Asset.FileName}");
        }
    }
}
```

**Step 6:** 编译 + 验证
- `dotnet build` 通过
- Content Browser 双击 .png → 弹出 Texture Editor,显示图片,4 个通道可切

**Step 7:** Commit
```bash
git add PrimalEditor_Avalonia/Editors/Texture/
git add PrimalEditor_Avalonia/Editors/AssetEditorHost.cs
git commit -m "feat(texture-editor): full texture editor with channel/mip/depth controls"
```

---

## Task 8: Geometry Editor 完整移植

**目标:** 完整的 Geometry 编辑器:LOD 选择 + 顶点线框预览(SkiaSharp 或 Canvas 2D 绘制 wireframe)。

**Files:**
- Create: `PrimalEditor_Avalonia/Editors/Geometry/PrimitiveTopology.cs` — 枚举
- Create: `PrimalEditor_Avalonia/Editors/Geometry/ElementsType.cs` — 枚举
- Create: `PrimalEditor_Avalonia/Editors/Geometry/Mesh.cs` — Mesh 数据类
- Create: `PrimalEditor_Avalonia/Editors/Geometry/MeshLOD.cs` — LOD 数据
- Create: `PrimalEditor_Avalonia/Editors/Geometry/Geometry.cs` — Geometry 主类(读 .mesh 二进制)
- Create: `PrimalEditor_Avalonia/Editors/Geometry/GeometryEditorViewModel.cs`
- Create: `PrimalEditor_Avalonia/Editors/Geometry/GeometryEditorView.axaml(.cs)` — 用 Canvas 绘制 wireframe

依赖:AssetInfo (T1), MSComponent (T3)

**Step 1:** 数据模型

```csharp
// Editors/Geometry/PrimitiveTopology.cs
public enum PrimitiveTopology { PointList=1, LineList, LineStrip, TriangleList, TriangleStrip }
public enum ElementsType { Position=0x00, Normals=0x01, TSpace=0x03, Joints=0x04, Colors=0x08 }

public class Mesh
{
    public string Name { get; set; } = "";
    public int VertexCount { get; set; }
    public int IndexCount { get; set; }
    public int IndexSize { get; set; } // 2 = ushort, 4 = uint
    public ElementsType ElementsType { get; set; }
    public byte[] Positions { get; set; } = Array.Empty<byte>();  // float[VertexCount*3]
    public byte[] Elements  { get; set; } = Array.Empty<byte>();
    public byte[] Indices   { get; set; } = Array.Empty<byte>();
}

public class MeshLOD
{
    public string Name { get; set; } = "";
    public float LodThreshold { get; set; }
    public List<Mesh> Meshes { get; } = new();
}

public class Geometry
{
    public string FullPath { get; set; } = "";
    public List<MeshLOD> LODs { get; } = new();

    /// <summary>解析 .mesh 二进制头部:8 字节 magic + uint lod_count + LOD 列表。</summary>
    public bool Load(string path)
    {
        if (!File.Exists(path)) return false;
        LODs.Clear();
        FullPath = path;
        using var fs = File.OpenRead(path);
        using var br = new BinaryReader(fs);
        var magic = br.ReadUInt64();
        if (magic != 0x48534D4550474D2DUL) return false;  // "-MGPEMESH" little-endian
        var lodCount = br.ReadUInt32();
        for (uint i = 0; i < lodCount; ++i)
        {
            var lod = new MeshLOD { Name = br.ReadString(), LodThreshold = br.ReadSingle() };
            var meshCount = br.ReadUInt32();
            for (uint m = 0; m < meshCount; ++m)
            {
                var mesh = new Mesh { Name = br.ReadString() };
                mesh.VertexCount  = (int)br.ReadUInt32();
                mesh.IndexCount   = (int)br.ReadUInt32();
                mesh.IndexSize    = (int)br.ReadUInt32();
                mesh.ElementsType = (ElementsType)br.ReadUInt32();
                var posBytes  = mesh.VertexCount * sizeof(float) * 3;
                var elemBytes = (int)br.ReadUInt32();
                mesh.Positions = br.ReadBytes(posBytes);
                mesh.Elements  = br.ReadBytes(elemBytes);
                mesh.Indices   = br.ReadBytes(mesh.IndexCount * mesh.IndexSize);
                lod.Meshes.Add(mesh);
            }
            LODs.Add(lod);
        }
        return true;
    }
}
```

**Step 2:** ViewModel

```csharp
// Editors/Geometry/GeometryEditorViewModel.cs
public class GeometryEditorViewModel : AssetEditorViewModel
{
    private Geometry _geometry = new();
    public Geometry Geometry { get => _geometry; private set { _geometry = value; OnPropertyChanged(); OnPropertyChanged(nameof(MaxLODIndex)); } }
    public int MaxLODIndex => Math.Max(0, _geometry.LODs.Count - 1);

    private int _lodIndex;
    public int LODIndex
    {
        get => _lodIndex;
        set { _lodIndex = System.Math.Clamp(value, 0, MaxLODIndex); OnPropertyChanged(); OnPropertyChanged(nameof(SelectedLOD)); }
    }
    public MeshLOD? SelectedLOD => _geometry.LODs.ElementAtOrDefault(_lodIndex);

    private bool _autoLOD = true;
    public bool AutoLOD { get => _autoLOD; set { _autoLOD = value; OnPropertyChanged(); } }

    public override void SetAsset(AssetInfo info)
    {
        base.SetAsset(info);
        State = AssetEditorState.Loading;
        var g = new Geometry();
        var ok = g.Load(info.FullPath);
        if (ok) { Geometry = g; LODIndex = 0; }
        else Logger.Error($"Failed to load geometry: {info.FullPath}");
        State = AssetEditorState.Done;
    }
}
```

**Step 3:** View (Avalonia Canvas 画 wireframe)

```xml
<!-- Editors/Geometry/GeometryEditorView.axaml -->
<UserControl xmlns="https://github.com/avaloniaui"
             xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
             xmlns:vm="using:PrimalEditor_Avalonia.Editors.Geometry"
             x:Class="PrimalEditor_Avalonia.Editors.Geometry.GeometryEditorView"
             x:DataType="vm:GeometryEditorViewModel">
  <Grid RowDefinitions="Auto,*">
    <StackPanel Grid.Row="0" Orientation="Horizontal" Spacing="6" Margin="6">
      <TextBlock Text="LOD:" VerticalAlignment="Center"/>
      <NumericUpDown Value="{Binding LODIndex}" Minimum="0" Maximum="{Binding MaxLODIndex}" Width="80"/>
      <CheckBox Content="Auto LOD" IsChecked="{Binding AutoLOD}"/>
      <TextBlock Text="(Drag mouse to orbit)" VerticalAlignment="Center" Foreground="Gray"/>
    </StackPanel>

    <Border Grid.Row="1" Background="#FF202020" ClipToBounds="True"
            PointerPressed="OnPointerPressed" PointerMoved="OnPointerMoved" PointerReleased="OnPointerReleased">
      <!-- Canvas 通过 code-behind 绘制 -->
    </Border>
  </Grid>
</UserControl>
```

Code-behind 用 `CustomDrawingOp` 或自定义 `Canvas` 控件:
```csharp
public partial class GeometryEditorView : UserControl
{
    public GeometryEditorView() { InitializeComponent(); }
    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    // 鼠标控制 yaw/pitch/distance + 简单正交投影 — 把 LOD 中的 mesh line list 画出来
    // 见完整代码 commit;简化版:只展示 mesh.LODs[lodIndex].Meshes[0] 的三角形线框。
}
```

**Step 4:** 验证 + Commit
- 双击 .mesh → Geometry Editor 弹出,显示 wireframe
- LOD 切换正常

```bash
git add PrimalEditor_Avalonia/Editors/Geometry/
git commit -m "feat(geometry-editor): full geometry editor with LOD switching + wireframe preview"
```

---

## Task 9: ContentWatcher — 监听 Content 目录变更

**目标:** Content 文件被外部(用户 OS 浏览器)修改后,Content Browser 自动刷新。

**Files:**
- Create: `PrimalEditor_Avalonia/Content/ContentWatcher.cs` — 包装 FileSystemWatcher + 去抖定时器
- Modify: `PrimalEditor_Avalonia/Views/ContentBrowserViewModel.cs` — 订阅 `ContentModified` 事件,自动 RefreshFolders

**Step 1:** 实现 ContentWatcher

```csharp
// Content/ContentWatcher.cs
using System.IO;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Content;

public class ContentModifiedEventArgs : System.EventArgs
{
    public string FullPath { get; }
    public WatcherChangeTypes ChangeType { get; }
    public ContentModifiedEventArgs(string path, WatcherChangeTypes t) { FullPath = path; ChangeType = t; }
}

public static class ContentWatcher
{
    private static FileSystemWatcher? _watcher;
    private static System.Timers.Timer? _debounce;
    public static event System.EventHandler<ContentModifiedEventArgs>? ContentModified;

    public static void Initialize(string root)
    {
        Reset();
        if (!Directory.Exists(root)) return;
        _watcher = new FileSystemWatcher(root)
        {
            IncludeSubdirectories = true,
            NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite,
        };
        _watcher.Created  += OnChange;
        _watcher.Changed  += OnChange;
        _watcher.Deleted  += OnChange;
        _watcher.Renamed  += (_, e) => OnChange(e.OldFullPath, WatcherChangeTypes.Renamed);
        _watcher.EnableRaisingEvents = true;

        _debounce = new System.Timers.Timer(250) { AutoReset = false };
        _debounce.Elapsed += (_, _) => Flush();
        Logger.Info($"ContentWatcher watching: {root}");
    }

    private static void OnChange(object _, FileSystemEventArgs e) => OnChange(e.FullPath, e.ChangeType);
    private static void OnChange(string path, WatcherChangeTypes type)
    {
        lock (_debounce!)
        {
            _debounce.Stop();
            _debounce.Start();
        }
        ContentModified?.Invoke(null, new ContentModifiedEventArgs(path, type));
    }

    private static void Flush()
    {
        // 触发 AssetRegistry 全量重扫
        ContentModified?.Invoke(null, new ContentModifiedEventArgs("", WatcherChangeTypes.All));
        Logger.Debug("ContentWatcher: flushed debounced change events");
    }

    public static void Reset()
    {
        _watcher?.Dispose(); _watcher = null;
        _debounce?.Dispose(); _debounce = null;
    }
}
```

**Step 2:** ContentBrowserViewModel 订阅

```csharp
// Views/ContentBrowserViewModel.cs (构造函数内)
PrimalEditor_Avalonia.Content.ContentWatcher.ContentModified += (_, _) =>
{
    Avalonia.Threading.Dispatcher.UIThread.Post(() => RefreshFolders());
};
```

并在 `MainWindowViewModel.Project.set` 内调用 `ContentWatcher.Initialize(value.ContentPath)`,卸载时 `Reset()`。

**Step 3:** 编译 + 验证 + Commit

```bash
git add PrimalEditor_Avalonia/Content/ContentWatcher.cs
git add PrimalEditor_Avalonia/Views/ContentBrowserViewModel.cs
git add PrimalEditor_Avalonia/ViewModels/MainWindowViewModel.cs
git commit -m "feat(content): add ContentWatcher for live filesystem change refresh"
```

---

## Task 10: WorldEditor 工具栏整合

**目标:** 把 T5/T6 的按钮接入 WorldEditorView 主菜单 + 工具栏,确保整个流程闭合。

**Files:**
- Modify: `PrimalEditor_Avalonia/Views/MainWindow.axaml` — File 菜单加 "New Script..."(占位) + "Import Asset..."
- Modify: `PrimalEditor_Avalonia/Views/MainWindow.axaml.cs` — 加 `OnNewScriptClick` handler
- Modify: `PrimalEditor_Avalonia/Views/WorldEditorView.axaml` — 在 Toolbar 加 "Primitive Mesh…"按钮(已在 T5)
- Modify: `PrimalEditor_Avalonia/Views/WorldEditorView.axaml.cs` — 已加 `OnCreatePrimitiveMeshClick`

**Step 1:** 主菜单加 New Script(占位)

```xml
<!-- Views/MainWindow.axaml File 菜单加 -->
<MenuItem Header="_New Script..." Click="OnNewScriptClick" InputGesture="Ctrl+Shift+N"/>
```

```csharp
// Views/MainWindow.axaml.cs
private void OnNewScriptClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
{
    PrimalEditor_Avalonia.Common.Logger.Info("New Script not implemented (VS integration skipped per user request).");
}
```

**Step 2:** 编辑器加载时自动激活 AssetEditor

```csharp
// Editors/AssetEditorRegistry.cs (Open 时改为 ShowDialog 而不是 Show,以便 Modal)
```

修改为 modal dialog 形式,确保编辑器窗口在主窗口之上。

**Step 3:** 编译验证 + Commit

```bash
git add PrimalEditor_Avalonia/Views/MainWindow.axaml
git add PrimalEditor_Avalonia/Views/MainWindow.axaml.cs
git add PrimalEditor_Avalonia/Editors/AssetEditorRegistry.cs
git commit -m "feat(worldeditor): integrate all new editor entrypoints into main menu + toolbar"
```

---

## Task 11: 验证(全量编译 + 端到端 smoke test)

**Step 1:** 清理编译产物
```bash
cd /Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration
rm -rf PrimalEditor_Avalonia/bin PrimalEditor_Avalonia/obj PrimalEditor_Avalonia/build
dotnet restore PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor
```

期望:`Build succeeded. 0 Error(s).`,warning 数量应 < 20。

**Step 2:** Smoke test 列表
1. 启动应用,看到 PrimalEditor Avalonia 主窗口
2. New Project → 选模板 → 创建到 ~/PrimalProjects/Test → 列表出现 Content/
3. Open Content/ → 创建空目录 + 拖入一个 .png
4. AssetBrowser 列出 1 个 .png(用 AssetInfo,显示 Icon + FileName)
5. 双击 .png → Texture Editor 弹窗,显示图片
6. 关闭编辑器,创建 Primitive Mesh → Sphere → Create → Content/Sphere.mesh 出现
7. 回到主窗口,Add Scene / Add Entity / Transform 数值调整 → Undo/Redo 正常
8. Build 下拉框切换 Debug/Release → 点 Build → 日志写 "Placeholder — would build"
9. (可选)手改 Content 下文件,ContentWatcher 触发刷新

每一步若失败,记录错误并修复,**不允许跳过**。

**Step 3:** 检查 git log
```bash
git log --oneline -15
```

应有 9 个 commit,每个对应一个 Task。

---

## 总结:11 个任务交付清单

| # | Task | 文件数 | 复用价值 |
|---|---|---|---|
| T1 | AssetInfo/Registry | 5 | ★★★★★ (Texture/Geometry/ContentBrowser 都依赖) |
| T2 | VectorBox/NumberBox | 4 | ★★★★★ (Transform/Primitive/ImportSettings 都用) |
| T3 | MSComponent | 4 | ★★★★ (所有 Inspector 多选场景) |
| T4 | IAssetEditor | 3 | ★★★★ (Texture/Geometry/Mesh Editor 路由) |
| T5 | Debug/Release 占位 | 4 | ★★ (一次性) |
| T6 | Primitive Mesh Dialog | 4 | ★★★ (演示 VectorBox 用法) |
| T7 | Texture Editor | 6 | ★★★ (完整资产编辑器) |
| T8 | Geometry Editor | 7 | ★★★ (完整资产编辑器) |
| T9 | ContentWatcher | 2 | ★★★ (Content Browser 体验) |
| T10 | 工具栏整合 | 2 | ★★ (一次性) |
| T11 | 验证 | 0 | — |

**杠杆最高的:** T1 (AssetInfo) + T2 (VectorBox) + T4 (IAssetEditor) 三个一起做完 = 解锁 T6/T7/T8 全部 3 个资产编辑器。

**建议执行顺序:** T1 → T2 (并行) → T3 → T4 → T5 → T6 → T7 → T8 → T9 → T10 → T11。

---

## 已知限制(明确告知用户)

1. **没有真实 ContentTools.dll 烘焙管线** — Texture 导入只用 Avalonia 内置 `Bitmap.Decode`,没有 BC7/BC5/mipmap 自动生成。Mesh 用占位 `CookPrimitiveMesh` P/Invoke,C++ 端没实现则日志报错。
2. **没有真实 Build** — Debug/Release 下拉只占位,不动 VS/MSBuild。
3. **没有 New Script/VS 集成** — 用户已确认跳过。
4. **Geometry Editor 是简易 wireframe** — 不像 WPF 那样用 System.Windows.Media.Media3D 做完整 3D 预览(需要一个 .NET 跨平台 3D 库,如 HelixToolkit)。Avalonia 11 原生没有 WPF 风格的 3D,只能用 Canvas 2D 画线框。
5. **依赖 `unsafe` 代码** — T7 Texture Editor 解码 RGBA 用 `bmp.CopyPixels + unsafe 块`,需要在 csproj 加 `<AllowUnsafeBlocks>true</AllowUnsafeBlocks>`。