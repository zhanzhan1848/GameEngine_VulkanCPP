# Editor Import Pipeline Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build the plugin-style importer registry, per-category settings dialogs, and app/project defaults store so the content browser's **Import** action routes by asset category to the right ContentTools cook with user-editable settings.

**Architecture:** Managed-side plugin registry (`Native/Importers/`) replaces the `switch (type)` in `Native/AssetImporter.cs`. Each plugin owns its `IAssetImportSettings` POCO, claim logic, settings ViewModel, and `RunAsync` (delegating to existing `NativeEngineBridge` P/Invoke). A new `Import/ImportSettingsDialog` opens once per category batch with merged defaults (asset → project → app → hard-coded). Per-asset `.meta` sidecars persist the chosen settings via the existing `AssetImportSettingsStore`.

**Tech Stack:** C# / .NET 8 / Avalonia 11.3, `CommunityToolkit.Mvvm`, existing `IAssetImportSettings` + `AssetImportSettingsStore` + `NativeEngineBridge` P/Invoke. C++ side requires a new `EDITOR_INTERFACE` entry point to accept the settings blob (one new exported function in `ContentTools/ContentTools.cpp`, mirrored in managed P/Invoke).

**Companion design doc:** `docs/plans/2026-08-07-editor-import-pipeline-design.md` — read first for rationale and locked-in decisions.

**Locked-in answers (from design phase):**
1. UX = per-asset dialog (once per category batch)
2. Dispatch = plugin-style registry
3. Defaults = app + per-project override
4. Batch = grouped by category
5. **Geometry surface = mirror all C++ `geometry_import_settings` fields** (requires new C++ entry point — see Task 0)
6. **Progress UX = status bar only (no `IProgress<double>`)**
7. **Editor reuse = KEEP SEPARATE** — do not touch `TextureEditorView` / `GeometryEditorView` / `PrimitiveMeshView`

---

## Task 0: Add C++ `CookMeshWithSettings` entry point

**Why this is Task 0:** the locked-in "mirror all C++ fields" answer requires the C++ side to actually accept a settings blob. Today `ContentTools.cpp::CookMesh` hard-codes all seven `geometry_import_settings` fields. Without this task, the Geometry import dialog is theatre.

**Files:**
- Modify: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/ContentTools/ContentTools.cpp`
- Modify: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/ContentTools/Geometry.h` (declare new export)

**Step 1: Add the new entry point declaration to `Geometry.h`**

Find the existing `geometry_import_settings` struct (around line 197) and add below it:

```cpp
struct geometry_import_settings
{
    f32      smoothing_angle;
    u8       calculate_normals;
    u8       calculate_tangents;
    u8       reverse_handedness;
    u8       import_embedded_textures;
    u8       import_animations;
    u8       coalesce_meshes;
};
```

Verify the seven fields match what is already there. Do not change existing field order — ABI compatibility.

**Step 2: Implement `CookMeshWithSettings` in `ContentTools.cpp`**

Add a new `EDITOR_INTERFACE` function below the existing `CookMesh`. The existing `CookMesh` calls into the new one with defaults — single source of truth for defaults.

```cpp
EDITOR_INTERFACE u32 CookMeshWithSettings(const char* input, const char* output,
    f32 smoothing_angle,
    u8 calculate_normals, u8 calculate_tangents,
    u8 reverse_handedness, u8 import_embedded_textures,
    u8 import_animations, u8 coalesce_meshes)
{
    using namespace primal::tools;

    if (!input || !output) return 0;

    const char* ext = strrchr(input, '.');
    if (!ext) return 0;

    scene_data data{};
    data.settings.smoothing_angle = smoothing_angle;
    data.settings.calculate_normals = calculate_normals;
    data.settings.calculate_tangents = calculate_tangents;
    data.settings.reverse_handedness = reverse_handedness;
    data.settings.import_embedded_textures = import_embedded_textures;
    data.settings.import_animations = import_animations;
    data.settings.coalesce_meshes = coalesce_meshes;

    if (strcasecmp(ext, ".fbx") == 0)
        ImportFbx(input, &data, nullptr);
    else if (strcasecmp(ext, ".obj") == 0)
        ImportObjAPI(input, &data, nullptr);
    else
        return 0;

    if (!data.buffer || data.buffer_size == 0) return 0;

    FILE* f = std::fopen(output, "wb");
    if (!f) { std::free(data.buffer); return 0; }
    size_t written = std::fwrite(data.buffer, 1, data.buffer_size, f);
    std::fclose(f);
    std::free(data.buffer);
    return (written == data.buffer_size) ? 1u : 0u;
}
```

**Step 3: Refactor existing `CookMesh` to delegate**

```cpp
EDITOR_INTERFACE u32 CookMesh(const char* input, const char* output)
{
    return CookMeshWithSettings(input, output,
        178.0f, 1, 1, 0, 0, 0, 1);
}
```

All existing defaults preserved. No behaviour change for legacy callers.

**Step 4: Commit**

```bash
git add ContentTools/ContentTools.cpp ContentTools/Geometry.h
git commit -m "feat(ContentTools): add CookMeshWithSettings entry point with all 7 geometry_import_settings fields"
```

---

## Task 1: Extend `GeometryImportSettings` with all C++ fields

**Files:**
- Modify: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Editors/Geometry/GeometryImportSettings.cs`

**Step 1: Add seven new managed fields (alongside existing ones, do NOT remove legacy fields)**

```csharp
public class GeometryImportSettings : IAssetImportSettings
{
    public LodGenerationMode LodMode { get; set; } = LodGenerationMode.Manual;
    public float Lod0Threshold { get; set; } = 0f;
    public float Lod1Threshold { get; set; } = 50f;
    public float Lod2Threshold { get; set; } = 100f;
    public bool GenerateTangentSpace { get; set; } = true;
    public bool RecalculateNormals { get; set; } = false;
    public MeshCompressionMode Compression { get; set; } = MeshCompressionMode.None;
    public bool ForceSingleLod { get; set; } = false;

    // NEW — mirror ContentTools/ContentTools.cpp::CookMeshWithSettings
    public float SmoothingAngle { get; set; } = 178f;
    public bool CalculateNormals { get; set; } = true;
    public bool CalculateTangents { get; set; } = true;
    public bool ReverseHandedness { get; set; } = false;
    public bool ImportEmbeddedTextures { get; set; } = false;
    public bool ImportAnimations { get; set; } = false;
    public bool CoalesceMeshes { get; set; } = true;
```

**Step 2: Extend `ToBinary` (append new fields, do NOT reorder existing writes)**

```csharp
    public void ToBinary(BinaryWriter w)
    {
        w.Write((int)LodMode);
        w.Write(Lod0Threshold);
        w.Write(Lod1Threshold);
        w.Write(Lod2Threshold);
        w.Write(GenerateTangentSpace);
        w.Write(RecalculateNormals);
        w.Write((int)Compression);
        w.Write(ForceSingleLod);
        // NEW
        w.Write(SmoothingAngle);
        w.Write(CalculateNormals);
        w.Write(CalculateTangents);
        w.Write(ReverseHandedness);
        w.Write(ImportEmbeddedTextures);
        w.Write(ImportAnimations);
        w.Write(CoalesceMeshes);
    }
```

**Step 3: Extend `FromBinary` with the same position-guarded pattern already used for `ForceSingleLod`**

```csharp
    public void FromBinary(BinaryReader r)
    {
        LodMode = (LodGenerationMode)r.ReadInt32();
        Lod0Threshold = r.ReadSingle();
        Lod1Threshold = r.ReadSingle();
        Lod2Threshold = r.ReadSingle();
        GenerateTangentSpace = r.ReadBoolean();
        RecalculateNormals = r.ReadBoolean();
        Compression = (MeshCompressionMode)r.ReadInt32();
        ForceSingleLod = r.ReadBoolean();
        // NEW — position-guarded
        if (r.BaseStream.Position < r.BaseStream.Length)
            SmoothingAngle = r.ReadSingle();
        if (r.BaseStream.Position < r.BaseStream.Length)
            CalculateNormals = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length)
            CalculateTangents = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length)
            ReverseHandedness = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length)
            ImportEmbeddedTextures = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length)
            ImportAnimations = r.ReadBoolean();
        if (r.BaseStream.Position < r.BaseStream.Length)
            CoalesceMeshes = r.ReadBoolean();
    }
```

**Step 4: Extend `FromJson` (mirror the `LodMode` pattern for each new field)**

```csharp
    public void FromJson(JsonObject settings)
    {
        // ... existing reads unchanged ...
        if (settings.TryGetPropertyValue("SmoothingAngle", out var s0) && s0 is JsonValue v0 && v0.TryGetValue(out float f0)) SmoothingAngle = f0;
        if (settings.TryGetPropertyValue("CalculateNormals", out var s1) && s1 is JsonValue v1 && v1.TryGetValue(out bool b1)) CalculateNormals = b1;
        if (settings.TryGetPropertyValue("CalculateTangents", out var s2) && s2 is JsonValue v2 && v2.TryGetValue(out bool b2)) CalculateTangents = b2;
        if (settings.TryGetPropertyValue("ReverseHandedness", out var s3) && s3 is JsonValue v3 && v3.TryGetValue(out bool b3)) ReverseHandedness = b3;
        if (settings.TryGetPropertyValue("ImportEmbeddedTextures", out var s4) && s4 is JsonValue v4 && v4.TryGetValue(out bool b4)) ImportEmbeddedTextures = b4;
        if (settings.TryGetPropertyValue("ImportAnimations", out var s5) && s5 is JsonValue v5 && v5.TryGetValue(out bool b5)) ImportAnimations = b5;
        if (settings.TryGetPropertyValue("CoalesceMeshes", out var s6) && s6 is JsonValue v6 && v6.TryGetValue(out bool b6)) CoalesceMeshes = b6;
    }
```

**Step 5: Extend `ToJson`**

```csharp
    public JsonObject ToJson()
    {
        return new JsonObject
        {
            ["LodMode"] = (int)LodMode,
            ["Lod0Threshold"] = Lod0Threshold,
            ["Lod1Threshold"] = Lod1Threshold,
            ["Lod2Threshold"] = Lod2Threshold,
            ["GenerateTangentSpace"] = GenerateTangentSpace,
            ["RecalculateNormals"] = RecalculateNormals,
            ["Compression"] = (int)Compression,
            ["ForceSingleLod"] = ForceSingleLod,
            ["SmoothingAngle"] = SmoothingAngle,
            ["CalculateNormals"] = CalculateNormals,
            ["CalculateTangents"] = CalculateTangents,
            ["ReverseHandedness"] = ReverseHandedness,
            ["ImportEmbeddedTextures"] = ImportEmbeddedTextures,
            ["ImportAnimations"] = ImportAnimations,
            ["CoalesceMeshes"] = CoalesceMeshes
        };
    }
```

**Step 6: Build**

Run: `dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor`
Expected: 0 errors, 0 new warnings.

**Step 7: Commit**

```bash
git add PrimalEditor_Avalonia/Editors/Geometry/GeometryImportSettings.cs
git commit -m "feat(editor): extend GeometryImportSettings with all C++ geometry_import_settings fields"
```

---

## Task 2: Add `[P/Invoke]` overload for `CookMeshWithSettings`

**Files:**
- Modify: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Native/NativeEngineBridge.cs` (around line 1253, sibling of `_CookMesh`)

**Step 1: Add the new `[DllImport]` declaration**

```csharp
[DllImport("libContentTools", EntryPoint = "CookMeshWithSettings", CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
private static extern uint _CookMeshWithSettings(
    IntPtr input, IntPtr output,
    float smoothing_angle,
    [MarshalAs(UnmanagedType.U1)] bool calculate_normals,
    [MarshalAs(UnmanagedType.U1)] bool calculate_tangents,
    [MarshalAs(UnmanagedType.U1)] bool reverse_handedness,
    [MarshalAs(UnmanagedType.U1)] bool import_embedded_textures,
    [MarshalAs(UnmanagedType.U1)] bool import_animations,
    [MarshalAs(UnmanagedType.U1)] bool coalesce_meshes);
```

POD field types match `geometry_import_settings` in `ContentTools/Geometry.h` (f32 + u8).

**Step 2: Add a managed wrapper alongside the existing `CookMesh`**

```csharp
public static bool CookMesh(string input, string output, Geometry.GeometryImportSettings settings)
{
    if (!_isLoaded) return false;
    try
    {
        var i = MarshalUtf8(input);
        var o = MarshalUtf8(output);
        return _CookMeshWithSettings(
            i, o,
            settings.SmoothingAngle,
            settings.CalculateNormals,
            settings.CalculateTangents,
            settings.ReverseHandedness,
            settings.ImportEmbeddedTextures,
            settings.ImportAnimations,
            settings.CoalesceMeshes) != 0;
    }
    catch (Exception ex) { Logger.Error("CookMesh failed", ex); return false; }
}
```

The existing 2-arg `CookMesh(input, output)` stays — that's used by `AssetImporter.ImportAsync` and `GeometryReimportService`. It delegates to `CookMeshWithSettings` with defaults at the **C++ level** (Task 0 already wired this) — no managed-side change needed.

**Step 3: Build**

Run: `dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor`
Expected: 0 errors.

**Step 4: Commit**

```bash
git add PrimalEditor_Avalonia/Native/NativeEngineBridge.cs
git commit -m "feat(editor): P/Invoke + managed wrapper for CookMeshWithSettings"
```

---

## Task 3: `IAssetImporterPlugin` + `ImportRequest`

**Files:**
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Native/Importers/IAssetImporterPlugin.cs`
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Native/Importers/ImportRequest.cs`

**Step 1: Write `ImportRequest.cs`**

```csharp
using System.Threading;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Native.Importers;

public sealed class ImportRequest
{
    public string SourcePath { get; init; } = string.Empty;
    public string DestinationDir { get; init; } = string.Empty;
    /// Existing <see cref="IAssetImportSettings"/> chosen by the user.
    public IAssetImportSettings Settings { get; init; } = null!;
    /// Subfolder under DestinationDir derived from the content browser selection.
    public string? Subfolder { get; init; }
}
```

**Step 2: Write `IAssetImporterPlugin.cs`**

```csharp
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Native.Importers;

public interface IAssetImporterPlugin
{
    /// Display name shown in the settings dialog header ("Texture Importer").
    string DisplayName { get; }

    /// AssetType(s) handled. One plugin may claim multiple types.
    IReadOnlyList<AssetType> HandledTypes { get; }

    /// Settings POCO used for both dialog binding and the .meta sidecar.
    /// Defaults: returned by CreateSettingsViewModel.
    Type SettingsType { get; }

    /// True if this plugin should claim a given source file. MUST NOT touch disk
    /// beyond reading the first few KB.
    bool CanClaim(string filePath);

    /// Build the bound ViewModel that drives the settings dialog.
    /// `defaults` is the merged precedence result (asset → project → app → code).
    ImportSettingsViewModelBase CreateSettingsViewModel(IAssetImportSettings defaults);

    /// Execute the cook + load. May produce side artifacts (e.g. .mesh alongside .fbx).
    Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct);
}
```

**Step 3: Add the abstract base ViewModel referenced above**

Create `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Native/Importers/ImportSettingsViewModelBase.cs`:

```csharp
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Native.Importers;

/// <summary>
/// Common base for the per-category settings panel. Backs the
/// <see cref="ImportSettingsDialog"/>; exposes the chosen settings POCO.
/// </summary>
public abstract class ImportSettingsViewModelBase : ViewModelBase
{
    /// The settings POCO the user is editing (a subclass of IAssetImportSettings).
    public abstract IAssetImportSettings Settings { get; }

    /// Stable name used by the registry to persist defaults
    /// (e.g. "TextureImporter", "GeometryImporter").
    public abstract string ImporterName { get; }
}
```

**Step 4: Build + commit**

```bash
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor
git add PrimalEditor_Avalonia/Native/Importers/
git commit -m "feat(editor): IAssetImporterPlugin contract + ImportRequest + settings VM base"
```

---

## Task 4: `AssetImporterRegistry`

**Files:**
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Native/Importers/AssetImporterRegistry.cs`

**Step 1: Write the registry**

```csharp
using System;
using System.Collections.Generic;
using System.Linq;
using PrimalEditor_Avalonia.Common;

namespace PrimalEditor_Avalonia.Native.Importers;

public static class AssetImporterRegistry
{
    private static readonly List<IAssetImporterPlugin> _plugins = new();

    /// Iterated in registration order. The first plugin whose CanClaim() returns
    /// true for a given path wins.
    public static IReadOnlyList<IAssetImporterPlugin> Plugins => _plugins;

    public static void Register(IAssetImporterPlugin plugin)
    {
        if (plugin == null) throw new ArgumentNullException(nameof(plugin));
        if (_plugins.Any(p => p.GetType() == plugin.GetType()))
        {
            Logger.Warning($"AssetImporterRegistry: duplicate plugin {plugin.GetType().Name} ignored");
            return;
        }
        _plugins.Add(plugin);
        Logger.Info($"AssetImporterRegistry: registered {plugin.GetType().Name} ({plugin.DisplayName})");
    }

    public static IAssetImporterPlugin? ResolveForFile(string filePath)
    {
        foreach (var p in _plugins)
            if (p.CanClaim(filePath)) return p;
        return null;
    }

    /// Hook for App startup. Called from App.axaml.cs.OnFrameworkInitializationCompleted.
    public static void RegisterDefaults()
    {
        Register(new TextureImporterPlugin());
        Register(new MeshImporterPlugin());
        Register(new PrimitiveMeshImporterPlugin());
    }
}
```

**Step 2: Commit**

```bash
git add PrimalEditor_Avalonia/Native/Importers/AssetImporterRegistry.cs
git commit -m "feat(editor): AssetImporterRegistry for plugin discovery"
```

Tasks 5–7 will provide the three concrete plugin implementations referenced here.

---

## Task 5: `TextureImporterPlugin`

**Files:**
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Native/Importers/TextureImporterPlugin.cs`
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Import/Settings/TextureImportPanelViewModel.cs`

**Step 1: Write `TextureImportPanelViewModel`**

```csharp
using System.IO;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.Native.Importers;

namespace PrimalEditor_Avalonia.Import.Settings;

/// <summary>
/// Settings panel VM for the Texture import dialog. Re-uses the same
/// <see cref="TextureImportSettings"/> POCO the per-asset editor uses;
/// the panel does not need preview/slice navigation (no cooked data yet).
/// </summary>
public sealed class TextureImportPanelViewModel : ImportSettingsViewModelBase
{
    private readonly TextureImportSettings _settings;

    public TextureImportPanelViewModel(IAssetImportSettings defaults)
    {
        _settings = (TextureImportSettings)(defaults ?? new TextureImportSettings());
    }

    public override IAssetImportSettings Settings => _settings;
    public override string ImporterName => "TextureImporter";

    // Surface every field the existing TextureEditorViewModel does, minus
    // IsNormalMap (matters at preview time, not at first-import — it's a hint
    // to the C++ side via the BC5 path, exposed below).
    public System.Collections.Generic.List<string> Sources { get => _settings.Sources; set { _settings.Sources = value; OnPropertyChanged(); } }
    public TextureDimension Dimension  { get => _settings.Dimension;  set { _settings.Dimension = value; OnPropertyChanged(); } }
    public int             MipLevels  { get => _settings.MipLevels;  set { _settings.MipLevels = value; OnPropertyChanged(); } }
    public float           AlphaThreshold { get => _settings.AlphaThreshold; set { _settings.AlphaThreshold = value; OnPropertyChanged(); } }
    public bool            PreferBC7  { get => _settings.PreferBC7;  set { _settings.PreferBC7 = value; OnPropertyChanged(); } }
    public TextureFormat   OutputFormat { get => _settings.OutputFormat; set { _settings.OutputFormat = value; OnPropertyChanged(); } }
    public bool            Compress   { get => _settings.Compress;   set { _settings.Compress = value; OnPropertyChanged(); } }
    public bool            IsSRGB     { get => _settings.IsSRGB;     set { _settings.IsSRGB = value; OnPropertyChanged(); } }
    public TextureCompressionMode CompressionMode { get => _settings.CompressionMode; set { _settings.CompressionMode = value; OnPropertyChanged(); } }
    public int             ArraySize  { get => _settings.ArraySize;  set { _settings.ArraySize = value; OnPropertyChanged(); } }
    public int             Depth      { get => _settings.Depth;      set { _settings.Depth = value; OnPropertyChanged(); } }
    public bool            IsNormalMap { get => _settings.IsNormalMap; set { _settings.IsNormalMap = value; OnPropertyChanged(); } }
}
```

**Step 2: Write `TextureImporterPlugin`**

```csharp
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.Import.Settings;

namespace PrimalEditor_Avalonia.Native.Importers;

public sealed class TextureImporterPlugin : IAssetImporterPlugin
{
    private static readonly HashSet<string> _exts = new(System.StringComparer.OrdinalIgnoreCase)
    {
        ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".dds", ".hdr", ".exr", ".ktx", ".ktx2"
    };

    public string DisplayName => "Texture Importer";
    public IReadOnlyList<AssetType> HandledTypes { get; } = new[] { AssetType.Texture };
    public Type SettingsType => typeof(TextureImportSettings);

    public bool CanClaim(string filePath) => _exts.Contains(Path.GetExtension(filePath));

    public ImportSettingsViewModelBase CreateSettingsViewModel(IAssetImportSettings defaults)
        => new TextureImportPanelViewModel(defaults);

    public async Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct)
    {
        var destPath = Path.Combine(request.DestinationDir, Path.GetFileName(request.SourcePath));
        Directory.CreateDirectory(request.DestinationDir);

        try { File.Copy(request.SourcePath, destPath, overwrite: true); }
        catch (System.Exception ex)
        {
            return new ImportedAsset { SourcePath = request.SourcePath, DestinationPath = destPath, Type = AssetType.Texture, Success = false, ErrorMessage = ex.Message };
        }

        AssetImportSettingsStore.Save(destPath, request.Settings);

        if (!NativeEngineBridge.IsLoaded)
            return new ImportedAsset { SourcePath = request.SourcePath, DestinationPath = destPath, Type = AssetType.Texture, Success = true };

        var result = await Task.Run(() => NativeEngineBridge.ImportTexture(destPath), ct);
        if (!result.HasValue || result.Value.Success == 0)
        {
            return new ImportedAsset { SourcePath = request.SourcePath, DestinationPath = destPath, Type = AssetType.Texture, Success = false,
                ErrorMessage = result?.ErrorMessage ?? "Texture import returned no result" };
        }

        return new ImportedAsset
        {
            SourcePath = request.SourcePath,
            DestinationPath = destPath,
            Type = AssetType.Texture,
            Success = true,
            NativeHandle = result.Value.Handle,
            Width = (int)result.Value.Width,
            Height = (int)result.Value.Height,
        };
    }
}
```

> **Note:** the `progress` parameter is required by the interface contract but the locked-in decision (no `IProgress<double>` this round) means the parameter is unused here. That's fine — silence is implementation freedom.

**Step 3: Build + commit**

```bash
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor
git add PrimalEditor_Avalonia/Native/Importers/TextureImporterPlugin.cs \
        PrimalEditor_Avalonia/Import/Settings/TextureImportPanelViewModel.cs
git commit -m "feat(editor): TextureImporterPlugin + dialog settings panel VM"
```

---

## Task 6: `MeshImporterPlugin`

**Files:**
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Native/Importers/MeshImporterPlugin.cs`
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Import/Settings/GeometryImportPanelViewModel.cs`

**Step 1: Write `GeometryImportPanelViewModel`**

```csharp
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Native.Importers;

namespace PrimalEditor_Avalonia.Import.Settings;

public sealed class GeometryImportPanelViewModel : ImportSettingsViewModelBase
{
    private readonly GeometryImportSettings _settings;

    public GeometryImportPanelViewModel(IAssetImportSettings defaults)
    {
        _settings = (GeometryImportSettings)(defaults ?? new GeometryImportSettings());
    }

    public override IAssetImportSettings Settings => _settings;
    public override string ImporterName => "GeometryImporter";

    // Existing (per-asset editor) fields
    public LodGenerationMode LodMode { get => _settings.LodMode; set { _settings.LodMode = value; OnPropertyChanged(); } }
    public float Lod0Threshold { get => _settings.Lod0Threshold; set { _settings.Lod0Threshold = value; OnPropertyChanged(); } }
    public float Lod1Threshold { get => _settings.Lod1Threshold; set { _settings.Lod1Threshold = value; OnPropertyChanged(); } }
    public float Lod2Threshold { get => _settings.Lod2Threshold; set { _settings.Lod2Threshold = value; OnPropertyChanged(); } }
    public bool GenerateTangentSpace { get => _settings.GenerateTangentSpace; set { _settings.GenerateTangentSpace = value; OnPropertyChanged(); } }
    public bool RecalculateNormals { get => _settings.RecalculateNormals; set { _settings.RecalculateNormals = value; OnPropertyChanged(); } }
    public MeshCompressionMode Compression { get => _settings.Compression; set { _settings.Compression = value; OnPropertyChanged(); } }
    public bool ForceSingleLod { get => _settings.ForceSingleLod; set { _settings.ForceSingleLod = value; OnPropertyChanged(); } }

    // NEW (Task 1)
    public float SmoothingAngle { get => _settings.SmoothingAngle; set { _settings.SmoothingAngle = value; OnPropertyChanged(); } }
    public bool CalculateNormals { get => _settings.CalculateNormals; set { _settings.CalculateNormals = value; OnPropertyChanged(); } }
    public bool CalculateTangents { get => _settings.CalculateTangents; set { _settings.CalculateTangents = value; OnPropertyChanged(); } }
    public bool ReverseHandedness { get => _settings.ReverseHandedness; set { _settings.ReverseHandedness = value; OnPropertyChanged(); } }
    public bool ImportEmbeddedTextures { get => _settings.ImportEmbeddedTextures; set { _settings.ImportEmbeddedTextures = value; OnPropertyChanged(); } }
    public bool ImportAnimations { get => _settings.ImportAnimations; set { _settings.ImportAnimations = value; OnPropertyChanged(); } }
    public bool CoalesceMeshes { get => _settings.CoalesceMeshes; set { _settings.CoalesceMeshes = value; OnPropertyChanged(); } }
}
```

**Step 2: Write `MeshImporterPlugin`**

```csharp
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Import.Settings;

namespace PrimalEditor_Avalonia.Native.Importers;

public sealed class MeshImporterPlugin : IAssetImporterPlugin
{
    private static readonly HashSet<string> _exts = new(System.StringComparer.OrdinalIgnoreCase)
    {
        ".fbx", ".obj", ".gltf", ".glb", ".dae", ".blend"
    };

    public string DisplayName => "Model Importer";
    public IReadOnlyList<AssetType> HandledTypes { get; } = new[] { AssetType.Mesh };
    public Type SettingsType => typeof(GeometryImportSettings);

    public bool CanClaim(string filePath) => _exts.Contains(Path.GetExtension(filePath));

    public ImportSettingsViewModelBase CreateSettingsViewModel(IAssetImportSettings defaults)
        => new GeometryImportPanelViewModel(defaults);

    public async Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct)
    {
        var destPath = Path.Combine(request.DestinationDir, Path.GetFileName(request.SourcePath));
        var cookedPath = Path.ChangeExtension(destPath, ".mesh");
        Directory.CreateDirectory(request.DestinationDir);

        var settings = (GeometryImportSettings)request.Settings;

        try { File.Copy(request.SourcePath, destPath, overwrite: true); }
        catch (System.Exception ex)
        {
            return new ImportedAsset { SourcePath = request.SourcePath, DestinationPath = destPath, Type = AssetType.Mesh, Success = false, ErrorMessage = ex.Message };
        }

        AssetImportSettingsStore.Save(destPath, settings);

        if (!NativeEngineBridge.IsLoaded)
            return new ImportedAsset { SourcePath = request.SourcePath, DestinationPath = destPath, Type = AssetType.Mesh, Success = true };

        var cookOk = await Task.Run(() => NativeEngineBridge.CookMesh(request.SourcePath, cookedPath, settings), ct);
        if (!cookOk)
        {
            return new ImportedAsset { SourcePath = request.SourcePath, DestinationPath = destPath, Type = AssetType.Mesh, Success = false,
                ErrorMessage = "ContentTools cooking failed" };
        }

        var requestId = NativeEngineBridge.ImportMesh(cookedPath);
        if (requestId == 0)
        {
            return new ImportedAsset { SourcePath = request.SourcePath, DestinationPath = destPath, Type = AssetType.Mesh, Success = false,
                ErrorMessage = "Mesh loader rejected the request (job system down?)" };
        }

        // WaitForMeshAsync — copied from AssetImporter to keep the plugin self-contained.
        var deadline = System.DateTime.UtcNow.AddMilliseconds(60_000);
        while (System.DateTime.UtcNow < deadline)
        {
            ct.ThrowIfCancellationRequested();
            NativeEngineBridge.TickMainThreadJobs();
            var (meshId, status) = NativeEngineBridge.TryGetMeshImportResult(requestId);
            if (status != NativeEngineBridge.MeshImportResultStatus.Pending)
                return new ImportedAsset { SourcePath = request.SourcePath, DestinationPath = destPath, Type = AssetType.Mesh, Success = status == NativeEngineBridge.MeshImportResultStatus.Success, NativeHandle = meshId };
            await Task.Delay(50, ct);
        }
        return new ImportedAsset { SourcePath = request.SourcePath, DestinationPath = destPath, Type = AssetType.Mesh, Success = false, ErrorMessage = "Mesh import timed out" };
    }
}
```

**Step 3: Build + commit**

```bash
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor
git add PrimalEditor_Avalonia/Native/Importers/MeshImporterPlugin.cs \
        PrimalEditor_Avalonia/Import/Settings/GeometryImportPanelViewModel.cs
git commit -m "feat(editor): MeshImporterPlugin + Geometry panel VM with all C++ fields"
```

---

## Task 7: `PrimitiveMeshImporterPlugin`

**Files:**
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Native/Importers/PrimitiveMeshImporterPlugin.cs`
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Import/Settings/PrimitiveMeshImportPanelViewModel.cs`

**Step 1: Write `PrimitiveMeshImportPanelViewModel`**

```csharp
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.PrimitiveMesh;
using PrimalEditor_Avalonia.Native.Importers;

namespace PrimalEditor_Avalonia.Import.Settings;

public sealed class PrimitiveMeshImportPanelViewModel : ImportSettingsViewModelBase
{
    private readonly PrimitiveInitInfo _settings;

    public PrimitiveMeshImportPanelViewModel(IAssetImportSettings defaults)
    {
        _settings = (PrimitiveInitInfo)(defaults ?? new PrimitiveInitInfo());
    }

    public override IAssetImportSettings Settings => _settings;
    public override string ImporterName => "PrimitiveMeshImporter";

    public PrimitiveMeshType Type      { get => _settings.Type;      set { _settings.Type = value; OnPropertyChanged(); } }
    public int              SegmentsX { get => _settings.SegmentsX; set { _settings.SegmentsX = value; OnPropertyChanged(); } }
    public int              SegmentsY { get => _settings.SegmentsY; set { _settings.SegmentsY = value; OnPropertyChanged(); } }
    public int              SegmentsZ { get => _settings.SegmentsZ; set { _settings.SegmentsZ = value; OnPropertyChanged(); } }
    public float            SizeX     { get => _settings.SizeX;     set { _settings.SizeX = value; OnPropertyChanged(); } }
    public float            SizeY     { get => _settings.SizeY;     set { _settings.SizeY = value; OnPropertyChanged(); } }
    public float            SizeZ     { get => _settings.SizeZ;     set { _settings.SizeZ = value; OnPropertyChanged(); } }
    public int              LOD       { get => _settings.LOD;       set { _settings.LOD = value; OnPropertyChanged(); } }
    public float            SmoothingAngle { get => _settings.SmoothingAngle; set { _settings.SmoothingAngle = value; OnPropertyChanged(); } }
    public string           OutputName { get => _settings.OutputName; set { _settings.OutputName = value; OnPropertyChanged(); } }
}
```

**Step 2: Write `PrimitiveMeshImporterPlugin`**

> **Important scope note:** the existing `PrimitiveMeshCooker` writes its own `.pm` files and registers the asset itself. This plugin just adapts that into the registry-shaped contract and persists the settings sidecar. We **do not** redesign the cooker here.

```csharp
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.PrimitiveMesh;
using PrimalEditor_Avalonia.Import.Settings;
using PrimalEditor_Avalonia.Native.Importers;

namespace PrimalEditor_Avalonia.Native.Importers;

public sealed class PrimitiveMeshImporterPlugin : IAssetImporterPlugin
{
    public string DisplayName => "Primitive Mesh Importer";
    public IReadOnlyList<AssetType> HandledTypes { get; } = new[] { AssetType.Mesh };
    public Type SettingsType => typeof(PrimitiveInitInfo);

    // Primitive-mesh plugins are explicitly invoked by the "Add → Primitive"
    // menu, not by file-extension selection. CanClaim always returns false
    // so the content browser's bulk picker never picks it up.
    public bool CanClaim(string filePath) => false;

    public ImportSettingsViewModelBase CreateSettingsViewModel(IAssetImportSettings defaults)
        => new PrimitiveMeshImportPanelViewModel(defaults);

    public Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct)
    {
        // PrimitiveMeshCooker handles the cook + registry registration itself.
        // The plugin only exists so its panel dialog and persisted settings
        // fit the same registry contract.
        var info = (PrimitiveInitInfo)request.Settings;
        // Caller (dialog accept handler) decides where to write — for primitives
        // there is no source file. Return a synthetic ImportedAsset so the
        // existing status bookkeeping works.
        var destName = string.IsNullOrWhiteSpace(info.OutputName) ? "Primitive_Mesh" : info.OutputName;
        return Task.FromResult(new ImportedAsset
        {
            SourcePath = string.Empty,
            DestinationPath = Path.Combine(request.DestinationDir, destName + ".pm"),
            Type = AssetType.Mesh,
            Success = true,
        });
    }
}
```

**Step 3: Build + commit**

```bash
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor
git add PrimalEditor_Avalonia/Native/Importers/PrimitiveMeshImporterPlugin.cs \
        PrimalEditor_Avalonia/Import/Settings/PrimitiveMeshImportPanelViewModel.cs
git commit -m "feat(editor): PrimitiveMeshImporterPlugin + panel VM (settings-dialog wiring only)"
```

---

## Task 8: App-level + project-level settings stores

**Files:**
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Common/EditorSettings.cs`
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/GameProject/ProjectEditorSettings.cs`
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Common/ImportSettingsDefaults.cs`

**Step 1: `Common/EditorSettings.cs` (app-level store)**

```csharp
using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using PrimalEditor_Avalonia.Content.Metadata;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// User-level default import settings, scoped by importer name
/// (e.g. "TextureImporter", "GeometryImporter"). Stored at
/// <c>ApplicationData/PrimalEditorAvalonia/editor-settings.json</c>.
/// Sibling to <see cref="EditorLayoutSettings"/>.
/// </summary>
public static class EditorSettings
{
    public static string Path2
    {
        get
        {
            var dir = System.IO.Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "PrimalEditorAvalonia");
            Directory.CreateDirectory(dir);
            return System.IO.Path.Combine(dir, "editor-settings.json");
        }
    }

    public static JsonObject? Read()
    {
        var p = Path2;
        if (!File.Exists(p)) return null;
        try
        {
            using var fs = File.OpenRead(p);
            return (JsonNode.Parse(fs) as JsonObject);
        }
        catch (Exception ex)
        {
            Logger.Warning($"EditorSettings.Read failed: {ex.Message}");
            return null;
        }
    }

    public static bool Write(JsonObject root)
    {
        var p = Path2;
        var partial = p + ".partial";
        try
        {
            using (var fs = File.Create(partial))
            {
                using var writer = new Utf8JsonWriter(fs, new JsonWriterOptions { Indented = true });
                root.WriteTo(writer);
            }
            if (File.Exists(p)) File.Delete(p);
            File.Move(partial, p);
            return true;
        }
        catch (Exception ex)
        {
            Logger.Error($"EditorSettings.Write failed: {p}", ex);
            try { if (File.Exists(partial)) File.Delete(partial); } catch { }
            return false;
        }
    }

    public static JsonObject? GetImporterSettings(string importerName)
    {
        var root = Read();
        return root?[importerName] as JsonObject;
    }

    public static void SetImporterSettings(string importerName, JsonObject settings)
    {
        var root = Read() ?? new JsonObject();
        root[importerName] = settings;
        Write(root);
    }
}
```

**Step 2: `GameProject/ProjectEditorSettings.cs` (per-project override)**

Same shape as `EditorSettings` but stored at `<project>/EditorSettings.json`. Create new file with full implementation following the `EditorSettings` pattern. Path resolution uses `Project?.Folder` (the existing `Project` class field).

**Step 3: `Common/ImportSettingsDefaults.cs` (precedence resolver)**

```csharp
using System;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Editors.PrimitiveMesh;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.GameProject;
using PrimalEditor_Avalonia.Native.Importers;

namespace PrimalEditor_Avalonia.Common;

/// <summary>
/// Resolves the effective default <see cref="IAssetImportSettings"/> for a
/// given importer at import time. Precedence (first match wins):
///   1. (Asset .meta sidecar — NOT consulted during initial import; the
///       asset has no sidecar yet. After successful import, the chosen
///       settings ARE persisted as the sidecar.)
///   2. Project EditorSettings.json (per-project override)
///   3. ApplicationData EditorSettings (app-level default)
///   4. New T() — hard-coded POCO defaults
/// </summary>
public static class ImportSettingsDefaults
{
    public static IAssetImportSettings ResolveDefaults(
        IAssetImporterPlugin plugin,
        Project? project,
        System.Collections.Generic.IReadOnlyList<string> sampleFilePaths)
    {
        // Walk down the precedence chain; first non-null wins.
        var fromProject = TryLoadFromProject(plugin, project);
        if (fromProject != null) return fromProject;

        var fromApp = TryLoadFromApp(plugin);
        if (fromApp != null) return fromApp;

        return (IAssetImportSettings)Activator.CreateInstance(plugin.SettingsType)!;
    }

    private static IAssetImportSettings? TryLoadFromProject(IAssetImporterPlugin plugin, Project? project)
    {
        if (project == null) return null;
        var node = ProjectEditorSettings.GetImporterSettings(project, ImporterNameOf(plugin));
        return node == null ? null : FromJson(plugin, node);
    }

    private static IAssetImportSettings? TryLoadFromApp(IAssetImporterPlugin plugin)
    {
        var node = EditorSettings.GetImporterSettings(ImporterNameOf(plugin));
        return node == null ? null : FromJson(plugin, node);
    }

    public static string ImporterNameOf(IAssetImporterPlugin p)
    {
        // Re-use the same mapping the existing AssetImportSettingsStore uses.
        return p.SettingsType.Name switch
        {
            nameof(TextureImportSettings) => "TextureImporter",
            nameof(GeometryImportSettings) => "GeometryImporter",
            nameof(PrimitiveInitInfo) => "PrimitiveMeshImporter",
            _ => p.SettingsType.Name,
        };
    }

    public static IAssetImportSettings FromJson(IAssetImporterPlugin plugin, System.Text.Json.Nodes.JsonObject json)
    {
        var s = (IAssetImportSettings)Activator.CreateInstance(plugin.SettingsType)!;
        s.FromJson(json);
        return s;
    }
}
```

**Step 4: Build + commit**

```bash
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor
git add PrimalEditor_Avalonia/Common/EditorSettings.cs \
        PrimalEditor_Avalonia/GameProject/ProjectEditorSettings.cs \
        PrimalEditor_Avalonia/Common/ImportSettingsDefaults.cs
git commit -m "feat(editor): app + project level settings stores with precedence resolver"
```

---

## Task 9: `ImportSettingsDialog` (AXAML + code-behind)

**Files:**
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Import/ImportSettingsDialog.axaml`
- Create: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Import/ImportSettingsDialog.axaml.cs`

**Step 1: AXAML**

```xml
<Window xmlns="https://github.com/avaloniaui"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        xmlns:tex="using:PrimalEditor_Avalonia.Import.Settings"
        xmlns:geo="using:PrimalEditor_Avalonia.Import.Settings"
        xmlns:prim="using:PrimalEditor_Avalonia.Import.Settings"
        x:Class="PrimalEditor_Avalonia.Import.ImportSettingsDialog"
        Width="560" SizeToContent="Height"
        WindowStartupLocation="CenterOwner"
        Title="{Binding PluginName}">
  <Grid RowDefinitions="Auto,*,Auto" Margin="12">
    <TextBlock Grid.Row="0" Text="{Binding HeaderLine}" Margin="0,0,0,8"/>
    <ContentControl Grid.Row="1" Content="{Binding Panel}"/>
    <StackPanel Grid.Row="2" Orientation="Horizontal" HorizontalAlignment="Right" Spacing="8" Margin="0,12,0,0">
      <Button Content="Cancel" Click="OnCancel"/>
      <Button Content="Import" Classes="accent" Click="OnAccept"/>
    </StackPanel>
  </Grid>
</Window>
```

**Step 2: code-behind**

```csharp
using System.Collections.Generic;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native.Importers;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Import;

public partial class ImportSettingsDialog : Window
{
    private readonly ImportSettingsViewModelBase _panel;

    public string PluginName { get; }
    public string HeaderLine { get; }
    public ImportSettingsViewModelBase Panel => _panel;

    private ImportSettingsDialog() { InitializeComponent(); }

    private ImportSettingsDialog(IAssetImporterPlugin plugin, IReadOnlyList<string> files, IAssetImportSettings defaults)
    {
        InitializeComponent();
        PluginName = plugin.DisplayName;
        HeaderLine = $"Import {files.Count} {string.Join(", ", System.Linq.Enumerable.Select(files, System.IO.Path.GetFileName))}";
        _panel = plugin.CreateSettingsViewModel(defaults);
        DataContext = this;
    }

    public static async Task<(bool accepted, IAssetImportSettings? settings)> ShowAsync(
        Window owner, IAssetImporterPlugin plugin, IReadOnlyList<string> files, IAssetImportSettings defaults)
    {
        var dlg = new ImportSettingsDialog(plugin, files, defaults);
        var tcs = new TaskCompletionSource<(bool, IAssetImportSettings?)>();
        dlg.Closed += (_, _) => tcs.TrySetResult(dlg._accepted
            ? (true, dlg._panel.Settings)
            : (false, null));
        await dlg.ShowDialog(owner);
        return await tcs.Task;
    }

    private bool _accepted;
    private void OnAccept(object? s, Avalonia.Interactivity.RoutedEventArgs e)
    {
        _accepted = true;
        Close();
    }

    private void OnCancel(object? s, Avalonia.Interactivity.RoutedEventArgs e) => Close();
}
```

**Step 3: Build + commit**

```bash
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor
git add PrimalEditor_Avalonia/Import/
git commit -m "feat(editor): ImportSettingsDialog"
```

---

## Task 10: Settings panels (per-category AXAML)

Three minimal panels. Each hosts the corresponding panel VM. Bindings use compiled bindings (`x:DataType`).

**Files:**
- Create: `PrimalEditor_Avalonia/Import/Settings/TextureImportPanel.axaml` + `.cs`
- Create: `PrimalEditor_Avalonia/Import/Settings/GeometryImportPanel.axaml` + `.cs`
- Create: `PrimalEditor_Avalonia/Import/Settings/PrimitiveMeshImportPanel.axaml` + `.cs`

**Step 1: TextureImportPanel.axaml** — host every `TextureImportPanelViewModel` property. Two-column grid with labels + bound controls. ~80 LOC.

**Step 2: GeometryImportPanel.axaml** — host every `GeometryImportPanelViewModel` property INCLUDING the seven new fields from Task 1. Use a `ScrollViewer` because the dialog can grow. ~120 LOC.

**Step 3: PrimitiveMeshImportPanel.axaml** — same approach, all `PrimitiveMeshImportPanelViewModel` properties. ~80 LOC.

**Step 4: Build + commit**

```bash
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor
git add PrimalEditor_Avalonia/Import/Settings/
git commit -m "feat(editor): per-category settings panel AXAML (Texture/Geometry/Primitive)"
```

---

## Task 11: Wire `ContentBrowserViewModel.ImportAssetAsync` to the registry

**Files:**
- Modify: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/ViewModels/ContentBrowserViewModel.cs`

**Step 1: Replace the for-loop body in `ImportAssetAsync` with the registry-shaped flow**

```csharp
public async Task ImportAssetAsync()
{
    if (_project == null) { Logger.Warning("Cannot import asset: no project loaded"); return; }
    var topLevel = Avalonia.Application.Current?.ApplicationLifetime is
        Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime d ? d.MainWindow : null;
    if (topLevel == null) return;

    var options = new Avalonia.Platform.Storage.FilePickerOpenOptions
    {
        Title = "Import Asset", AllowMultiple = true,
        FileTypeFilter = /* unchanged */
    };
    var files = await topLevel.StorageProvider.OpenFilePickerAsync(options);
    if (files == null || files.Count == 0) return;

    // Group by importer plugin (Type)
    var subfolder = SelectedFolder != null && SelectedFolder.FullPath != _project.ContentPath
        ? Path.GetFileName(SelectedFolder.FullPath) : null;

    var groups = System.Linq.Enumerable.Select(files, f => new { File = f, Path = f.Path.LocalPath })
        .Where(x => AssetImporterRegistry.ResolveForFile(x.Path) != null)
        .GroupBy(x => AssetImporterRegistry.ResolveForFile(x.Path)!.GetType())
        .ToList();

    var perFileSettings = new Dictionary<string, IAssetImportSettings>();
    foreach (var group in groups)
    {
        var plugin = group.First().File != null
            ? AssetImporterRegistry.ResolveForFile(group.First().Path)!
            : null!;
        var paths = group.Select(g => g.Path).ToList();

        var defaults = ImportSettingsDefaults.ResolveDefaults(plugin, _project, paths);
        var (accepted, settings) = await ImportSettingsDialog.ShowAsync(topLevel, plugin, paths, defaults);
        if (!accepted) continue;
        foreach (var p in paths) perFileSettings[p] = settings!;
    }

    var imported = 0; var failed = 0; var firstError = ""; var total = perFileSettings.Count;
    IsImporting = true;
    try
    {
        var index = 0;
        foreach (var (sourcePath, settings) in perFileSettings)
        {
            index++;
            var name = Path.GetFileName(sourcePath);
            var destDir = string.IsNullOrEmpty(subfolder) ? _project.ContentPath : Path.Combine(_project.ContentPath, subfolder);
            ImportingStatus = $"Importing {name} ({index}/{total})...";

            var plugin = AssetImporterRegistry.ResolveForFile(sourcePath)!;
            var asset = await plugin.RunAsync(new ImportRequest
            {
                SourcePath = sourcePath,
                DestinationDir = destDir,
                Subfolder = subfolder,
                Settings = settings,
            }, progress: null, ct: default);

            if (asset != null && asset.Success)
            {
                imported++;
                AssetRegistry.Register(asset.DestinationPath);  // ensures .meta GUID + SQLite row
                Logger.Info($"Imported: {Path.GetFileName(asset.DestinationPath)} ({asset.Type})");
            }
            else
            {
                failed++;
                var msg = asset?.ErrorMessage ?? "unknown error";
                Logger.Warning($"Import failed: {name} - {msg}");
                if (string.IsNullOrEmpty(firstError)) firstError = $"{name}: {msg}";
            }
        }
    }
    finally
    {
        IsImporting = false;
        ImportingStatus = string.Empty;
    }

    StatusMessage = failed > 0
        ? $"Imported: {imported}, Failed: {failed}. First error: {firstError}"
        : $"Imported: {imported}, Failed: {failed}";
    RefreshFolders();
}
```

**Step 2: Build + commit**

```bash
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor
git add PrimalEditor_Avalonia/ViewModels/ContentBrowserViewModel.cs
git commit -m "refactor(editor): ContentBrowser ImportAssetAsync dispatches via AssetImporterRegistry"
```

---

## Task 12: Register plugins at app startup

**Files:**
- Modify: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/App.axaml.cs`

**Step 1: Call `RegisterDefaults()` early**

Find `OnFrameworkInitializationCompleted` (or `Program.Main` if that runs first). Add `AssetImporterRegistry.RegisterDefaults();` before any window/dock setup, surrounded by try/catch that logs and continues (registry empty ⇒ legacy `AssetImporter` paths still work because of fallback).

**Step 2: Build + commit**

```bash
dotnet build PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor
git add PrimalEditor_Avalonia/App.axaml.cs
git commit -m "chore(editor): register AssetImporterRegistry plugins at startup"
```

---

## Task 13: Keep `Native/AssetImporter.cs` as compat shim (do NOT delete)

**Files:**
- Read-only review: `/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/.worktrees/avalonia-migration/PrimalEditor_Avalonia/Native/AssetImporter.cs`

**Step 1: Verify no callers broke**

`grep -rn "AssetImporter\." PrimalEditor_Avalonia/ | grep -v "AssetImporterRegistry" | grep -v "Native/Importers/"`

Expected: only `AssetImporter.DetectType` / `AssetImporter.ImportAsync` references inside ContentBrowserViewModel (now removed in Task 11) and TextureEditorViewModel.Reimport (untouched). The latter is fine — `ImportAsync` is still a public API.

`AssetImporter.cs` stays intact for backward compat — the existing `TextureEditorViewModel.Reimport` still calls it, and that path doesn't need the new settings UX (per-asset editor is a separate flow per the locked-in "keep separate" decision).

**Step 2: No code change required. Log verification only.**

```bash
echo "Compat verified"
```

(No commit.)

---

## Task 14: Manual end-to-end smoke test

**Step 1: Run the editor**

`dotnet run --project PrimalEditor_Avalonia/PrimalEditor_Avalonia.csproj -c DebugEditor -- --project <some test project>`

**Step 2: Open content browser → Import → pick one .png and one .fbx**

Expected:
- One Texture dialog appears (since group-by-category ⇒ two dialogs for two categories, but the second auto-opens after the first is accepted)
- After both accepted, status reads `Imported: 2, Failed: 0`
- Per-asset `.meta` sidecar exists for both, contains the chosen importer + settings JSON

**Step 3: Reimport the .fbx via the per-asset editor and verify the cooked `.mesh` matches the first cook byte-for-byte**

This validates the C++ settings round-trip: same `geometry_import_settings` blob ⇒ same cooked binary.

**Step 4: Commit (no code change) — push the worktree branch**

```bash
git push origin <branch>
```

---

## Out of scope (deferred per locked-in decisions)

- **Per-asset editor migration to new panels** — separate future PR (locked-in).
- **`IProgress<double>` dialog progress bar** — status bar sufficient (locked-in).
- **Audio / Script / Shader / Material plugins** — no ContentTools cook step yet.
- **App-level settings UI surface** (Settings window for editing `editor-settings.json`) — separate ticket.
- **Project `EditorSettings.json` loader hook on `Project.Open`** — minimal: Task 8 already loads on demand via `ResolveDefaults`. Lazy load OK for v1.

## Risks remaining after this plan

- **No C++ build/CI step is exercised here.** Tasks 0 + 2 require `ContentTools` to be rebuilt and `libContentTools.dylib` (macOS) / `ContentTools.dll` (Windows) to be re-copied into the editor's run dir. If CI doesn't pick up the new function symbol, `NativeEngineBridge.CookMesh(input, output, settings)` will throw `EntryPointNotFoundException` at runtime. Mitigation: locally rebuild ContentTools with `cmake --build ContentTools/build` (or Visual Studio) before first manual smoke test.
- **Mixed-category batch ordering**: Task 11 iterates `groups` in dictionary-insertion order. If two categories share the same `IAssetImportPlugin` (e.g. two Texture variants), the dict collapses them; this is OK for v1.
- **Failure mid-batch** (CookMesh succeeds, ImportMesh fails): leaves a `.mesh` file but no asset registered. Acceptable for v1 — the next content-browser refresh surfaces the orphan and the user can delete or reimport.
