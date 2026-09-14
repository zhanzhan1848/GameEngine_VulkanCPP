# Editor Import Pipeline (Categories + Adjustable Settings)

**Date:** 2026-08-07
**Owner:** Editor team
**Status:** Design — **approved**, locked-in answers recorded below

## Locked-in decisions (2026-08-07)

| Question | Answer |
|---|---|
| Settings UX during import | Per-asset dialog |
| Dispatch model | Plugin-style registry |
| Default settings location | App settings + per-project override |
| Batch behaviour | Group by category, prompt once per group |
| **Geometry settings surface** | **Mirror all C++ `geometry_import_settings` fields** — extend `GeometryImportSettings` with `SmoothingAngle`, `CalculateNormals`, `CalculateTangents`, `ReverseHandedness`, `ImportEmbeddedTextures`, `ImportAnimations`, `CoalesceMeshes`; wire each to the matching C++ field |
| **Cook progress UX** | **Status bar only** — no `IProgress<double>` wiring this round (YAGNI) |
| **Editor view reuse** | **Keep separate** — don't migrate `TextureEditorView` / `GeometryEditorView` / `PrimitiveMeshView` onto the new panels in this PR |

## Problem

The PrimalEditor_Avalonia content browser calls `Native/AssetImporter.ImportAsync`, which has a single hard-coded `switch (type)` that:

- **Texture** → `NativeEngineBridge.ImportTexture(destPath)` (no settings payload)
- **Mesh** → `NativeEngineBridge.CookMesh(...) + ImportMesh(...)` (no settings payload)
- **Audio / Script / Material / Shader** → file-copy only

The `IAssetImportSettings` hierarchy (`TextureImportSettings`, `GeometryImportSettings`, `PrimitiveInitInfo`) and the sidecar store `AssetImportSettingsStore` are wired into per-asset editor views (e.g. `TextureEditorViewModel.SetAsset` reads `TextureImportSettings` from the `.meta` sidecar). But the **import flow itself does not consult settings**, and there is no global / project-level default-settings store — every asset adopts whatever the C++ ContentTools chooses, and the user can only adjust after the fact by reimporting through the per-asset editor.

This means:

1. Users cannot pick compression format / mip strategy / LOD thresholds etc. *before* the first cook.
2. Adding a new asset category requires editing two switch statements (extension set + dispatch).
3. Defaults cannot be shared across assets or across projects.

## Goal

In the content browser's **Import** action, route by asset category to the appropriate ContentTools importer; before kicking off the cook, show a settings dialog bound to that category's `IAssetImportSettings`. Defaults come from app-level + project-level settings; per-asset overrides persist to the existing `.meta` sidecar.

## Design

### 1. Importer plugin registry

New layer `Native/Importers/` with one class per category. Each implements a single interface so the dispatcher iterates rather than switches.

```
Native/Importers/
├── IAssetImporterPlugin.cs          # the contract
├── AssetImporterRegistry.cs         # discovery + lookup
├── TextureImporterPlugin.cs         # bind to NativeEngineBridge.ImportTexture
├── MeshImporterPlugin.cs            # bind to NativeEngineBridge.CookMesh / ImportMesh
├── PrimitiveMeshImporterPlugin.cs   # bind to existing PrimitiveMeshCooker
└── (future) ShaderImporterPlugin, MaterialImporterPlugin, AudioImporterPlugin
```

```csharp
public interface IAssetImporterPlugin
{
    /// Display name shown in dialog headers ("Texture Importer")
    string DisplayName { get; }

    /// AssetType(s) handled. One plugin → many types is allowed
    /// (e.g. a ModelImporter could claim both Mesh and SkinnedMesh).
    IReadOnlyList<AssetType> HandledTypes { get; }

    /// Settings POCO used for both the dialog binding and the .meta sidecar.
    Type SettingsType { get; }

    /// True if this plugin should claim a given source file (extension sniff,
    /// optional cheap magic-byte check). MUST NOT touch disk beyond reading
    /// the first few KB.
    bool CanClaim(string filePath);

    /// Build the bound ViewModel that drives the settings dialog.
    /// Implementations re-host the existing editor VMs (TextureEditorViewModel
    /// works as-is; Geometry settings need a thin new wrapper VM).
    ImportSettingsViewModelBase CreateSettingsViewModel(IAssetImportSettings defaults);

    /// Execute the cook+load. The destination is the path the caller chose;
    /// the implementation may produce side artifacts (e.g. .mesh alongside .fbx).
    Task<ImportedAsset> RunAsync(ImportRequest request, IProgress<double>? progress, CancellationToken ct);
}
```

`AssetImporterRegistry` is a static registry initialised at app startup. Plugins self-register via a partial-class `AssetImporterRegistry.RegisterDefault()` called from `App.axaml.cs.OnFrameworkInitializationCompleted`. This mirrors the existing `IAssetRegistryBackend` / `ConfigureBackend` style.

Lookup:

```csharp
public static IAssetImporterPlugin? ResolveForFile(string filePath);
public static IReadOnlyList<IAssetImporterPlugin> ResolveForTypes(IReadOnlyList<AssetType> types);
```

### 2. Settings dialog (`Import/SettingsDialog.axaml`)

A new transient Window, opened **once per asset category batch**. Bound to a re-hosted settings panel. We do **not** reuse the existing `TextureEditorView` directly (it is a docked `UserControl` with surrounding channels toolbar). Instead, a thin reusable control per importer — for Texture, a stripped subset of the existing channels toolbar (sRGB / normal-map / format / compress / mip / array / depth) wrapped in a stand-alone `Window`. For Geometry, the existing `GeometryImportSettings` enum sliders wrap in a new minimal `UserControl`. For PrimitiveMesh, a slim version of `PrimitiveMeshView`.

The dialog also surfaces the merged-defaults provenance ("Defaults from: Project EditorSettings → App EditorSettings → hard-coded"). This is informational only — no separate UI selector yet (YAGNI).

```csharp
public class ImportSettingsDialog : Window
{
    public static Task<(bool accepted, IAssetImportSettings? settings)> ShowAsync(
        Window owner,
        IAssetImporterPlugin plugin,
        IReadOnlyList<string> filePaths,
        IAssetImportSettings defaults);
}
```

### 3. Settings precedence + persistence

```
Asset .meta sidecar       (per-asset, set on Save or during import)
  │
  │  ← falls through when missing
  ▼
Project EditorSettings    (per-project, scoped by importer name)
  │
  │  ← falls through when missing
  ▼
App EditorSettings        (user-level, scoped by importer name)
  │
  │  ← falls through when missing
  ▼
Hard-coded defaults       (the constructor on each IAssetImportSettings)
```

Two new stores — both keyed by importer name (e.g. `"TextureImporter"`, `"GeometryImporter"`):

`Common/AppSettings.cs` — app-level defaults:
```csharp
{
  "textureImportSettings": { ... },
  "geometryImportSettings": { ... },
  "primitiveMeshSettings": { ... }
}
```
Stored at `ApplicationData/PrimalEditorAvalonia/editor-settings.json` (sibling to the existing `editor-layout.json`).

`GameProject/ProjectEditorSettings.cs` — per-project overrides. Stored at `<project>/EditorSettings.json`, auto-created on first import. Project class loads it on Open and merges into the per-importer settings resolver.

A single resolver:

```csharp
public static class ImportSettingsDefaults
{
    /// Resolve effective defaults for a category, applying precedence.
    public static IAssetImportSettings ResolveDefaults(
        IAssetImporterPlugin plugin,
        Project? project,
        IReadOnlyList<string> sampleFilePaths);
}
```

Per-asset overrides from existing `.meta` sidecars are NOT consulted during the initial-import dialog (the asset has no .meta yet). Once the user accepts the dialog and the cook completes, the dialog result **is** what we save to the per-asset `.meta`.

### 4. Dispatch flow inside ContentBrowser

`ViewModels/ContentBrowserViewModel.ImportAssetAsync` is refactored:

```csharp
public async Task ImportAssetAsync()
{
    var files = await OpenFilePickerAsync();            // existing
    var project = _project;

    // 1. Group by importer (via AssetImporterRegistry).
    var groups = files
        .GroupBy(f => AssetImporterRegistry.ResolveForFile(f.Path.LocalPath)?.GetType())
        .Where(g => g.Key != null)
        .ToList();

    // 2. For each group, prompt once with merged defaults.
    var perFileSettings = new Dictionary<string, IAssetImportSettings>();
    foreach (var group in groups)
    {
        var plugin = AssetImporterRegistry.Instances[group.Key!];
        var paths = group.Select(f => f.Path.LocalPath).ToList();
        var defaults = ImportSettingsDefaults.ResolveDefaults(
            plugin, project, paths);
        var (accepted, settings) = await ImportSettingsDialog.ShowAsync(
            OwnerWindow, plugin, paths, defaults);
        if (!accepted) continue;
        foreach (var p in paths) perFileSettings[p] = settings!;
    }

    // 3. For each file, dispatch to its plugin's RunAsync.
    IsImporting = true;
    try
    {
        foreach (var (path, settings) in perFileSettings)
        {
            var plugin = AssetImporterRegistry.ResolveForFile(path)!;
            var asset = await plugin.RunAsync(new ImportRequest {
                SourcePath = path,
                DestinationDir = subfolder,
                Settings = settings,
            }, ...);
            // existing bookkeeping (status, log, .meta save, registry insert)
        }
    }
    finally { IsImporting = false; }
}
```

After a successful cook we persist the settings choice to the asset's `.meta` sidecar via the existing `AssetImportSettingsStore.Save<TextureImportSettings>(destPath, settings)` call. This both creates the sidecar and registers the GUID (already done by `AssetRegistry.Register`).

### 5. Native bridge changes

Currently `NativeEngineBridge.ImportTexture(path)` accepts only a path. The dialog-driven flow needs the settings to reach the C++ side. Two options:

- **Add overloads that take a settings blob**: `ImportTexture(path, settings)` serialises `TextureImportSettings` to the existing texture_import_settings C++ POD (we already have `texture_import_settings` struct in `ContentTools/TextureImporter.cpp:48`). This mirrors the existing field names one-for-one.
- **Pass via env / JSON side-file**: less intrusive on the P/Invoke surface, but the engine code already has to read settings — wire it up.

We pick the **direct overload** because the C++ `Import()` already expects `texture_data::import_settings` to be populated before the call. New import calls will simply populate that struct from the managed side. Same applies to the geometry / model flow which already reads `scene_data::settings`.

`NativeEngineBridge` changes:
- Add `[DllImport]` entry points that take a settings blob for texture + mesh.
- Keep the old `ImportTexture(path)` overload as a thin wrapper that constructs a default `texture_import_settings` (preserves all current callers — `TextureEditorViewModel.Reimport`, `AssetImporter.ImportAsync`).

### 6. Settings dialog per category — re-hosts

#### Texture
A new `Import/Settings/TextureImportPanel.axaml` — a `UserControl` with:
- Dimension (2D / Cube / 3D) — ComboBox
- OutputFormat (BC1/BC3/BC5/BC7/UNORM) — ComboBox
- Compress — CheckBox
- PreferBC7 — CheckBox (greyed when !Compress)
- IsSRGB — CheckBox
- IsNormalMap — CheckBox
- MipLevels — NumberBox (0 = auto)
- AlphaThreshold — NumberBox (slider)
- ArraySize, Depth — NumberBox

Bound via compiled bindings to a new `TextureImportPanelViewModel` that wraps a single `TextureImportSettings` and surfaces the same fields the existing `TextureEditorViewModel` does — minus preview/slice navigation since there's no cooked data yet. Approx 80 LOC + AXAML.

#### Geometry / Model
A new `Import/Settings/GeometryImportPanel.axaml` bound to `GeometryImportPanelViewModel`:
- LodMode (Manual / Auto) — ComboBox
- Lod0/1/2 thresholds — NumberBoxes
- GenerateTangentSpace — CheckBox
- RecalculateNormals — CheckBox
- Compression — ComboBox (None / Auto)
- ForceSingleLod — CheckBox
- **SmoothingAngle** — NumberBox (degrees, default 178)
- **CalculateNormals** — CheckBox
- **CalculateTangents** — CheckBox
- **ReverseHandedness** — CheckBox
- **ImportEmbeddedTextures** — CheckBox
- **ImportAnimations** — CheckBox
- **CoalesceMeshes** — CheckBox

`GeometryImportSettings.cs` is extended with the seven C++ fields (see "Locked-in decisions" above). Each round-trips through `ToBinary` / `FromBinary` / `ToJson` / `FromJson`. `GeometryImportPanelViewModel` exposes each as a property bound to the panel.

`NativeEngineBridge.CookMesh` / `ImportMesh` get a new overload that accepts a settings blob. The C++ `geometry_import_settings` struct is filled field-for-field; no ContentTools C++ changes needed. Approx 80 LOC + AXAML.

#### PrimitiveMesh
A new `Import/Settings/PrimitiveMeshImportPanel.axaml` bound to `PrimitiveMeshImportPanelViewModel` — re-uses existing form fields from `PrimitiveMeshView`, excludes the 3D preview section. Reuses `PrimitiveInitInfo.cs`. Approx 60 LOC + AXAML.

### 7. Fallback / no-settings categories

For `Audio`, `Script`, `Shader`, `Material` — none of these currently have a `ContentTools` cook step (the engine does the heavy lifting at runtime). The dialog is skipped silently: the registry plugin's `SettingsType` is `IAssetImportSettings` itself but produces an empty placeholder; import falls through to "file copy + log". Setting up these plugins is YAGNI; the resolver returns `null` and `ImportAssetAsync` falls back to the existing copy-only behaviour. **No regression** to ship.

## File / Module Inventory

```
NEW
  PrimalEditor_Avalonia/Native/Importers/IAssetImporterPlugin.cs
  PrimalEditor_Avalonia/Native/Importers/AssetImporterRegistry.cs
  PrimalEditor_Avalonia/Native/Importers/TextureImporterPlugin.cs
  PrimalEditor_Avalonia/Native/Importers/MeshImporterPlugin.cs
  PrimalEditor_Avalonia/Native/Importers/PrimitiveMeshImporterPlugin.cs
  PrimalEditor_Avalonia/Native/Importers/ImportRequest.cs
  PrimalEditor_Avalonia/Import/ImportSettingsDialog.axaml
  PrimalEditor_Avalonia/Import/ImportSettingsDialog.axaml.cs
  PrimalEditor_Avalonia/Import/Settings/TextureImportPanel.axaml(.cs)
  PrimalEditor_Avalonia/Import/Settings/TextureImportPanelViewModel.cs
  PrimalEditor_Avalonia/Import/Settings/GeometryImportPanel.axaml(.cs)
  PrimalEditor_Avalonia/Import/Settings/GeometryImportPanelViewModel.cs
  PrimalEditor_Avalonia/Import/Settings/PrimitiveMeshImportPanel.axaml(.cs)
  PrimalEditor_Avalonia/Import/Settings/PrimitiveMeshImportPanelViewModel.cs
  PrimalEditor_Avalonia/Common/AppSettings.cs
  PrimalEditor_Avalonia/GameProject/ProjectEditorSettings.cs
  PrimalEditor_Avalonia/Common/ImportSettingsDefaults.cs

CHANGED
  PrimalEditor_Avalonia/App.axaml.cs                # register plugins at startup
  PrimalEditor_Avalonia/Native/NativeEngineBridge.cs # +settings overloads
  PrimalEditor_Avalonia/Native/AssetImporter.cs     # thin wrapper (compat) or deleted in favour of plugins
  PrimalEditor_Avalonia/ViewModels/ContentBrowserViewModel.cs  # new dispatch flow
  PrimalEditor_Avalonia/GameProject/Project.cs     # load/save ProjectEditorSettings
```

## Behaviour Walkthrough

1. User clicks **Import** in content browser.
2. File picker (existing) — multi-select.
3. Files grouped by category via plugin lookup:
   - `3 × .png` → one TextureImporterPlugin group
   - `1 × .fbx` → one MeshImporterPlugin group
4. Texture dialog opens with defaults: app-level → project-level → hard-coded. User tweaks: `OutputFormat = BC3_UNORM`, `IsSRGB = false` (it is a mask texture), `MipLevels = 0` (auto).
5. User clicks **Import** (or **Cancel** — aborts the whole group).
6. Per-file dispatch: each .png is `File.Copy`'d into Content/<subfolder>; settings serialised into the texture_import_settings blob; `NativeEngineBridge.ImportTexture(path, blob)` runs; result + GUID registered in `AssetRegistry`; per-asset `.meta` sidecar written.
7. Mesh dialog opens. User accepts defaults. One `.fbx` cooks into Content/<subfolder>/<name>.mesh; settings blob reaches the C++ side as `geometry_import_settings`.
8. Status bar shows `Imported: 4, Failed: 0`.

## Edge Cases

- **No engine loaded** (`NativeEngineBridge.IsLoaded == false`): the plugin still runs (settings get saved to `.meta`); no cooked artifact. Existing log line preserved.
- **Mixed category batch** (`1 png + 1 fbx`): two dialogs shown sequentially, one per category.
- **Cancellation mid-import** (`CancellationToken` from dialog dismiss): partial files cleaned; `Register` not called; status bar shows `Cancelled by user`.
- **Existing assets re-imported**: reuses existing flow via `GeometryReimportService` and `TextureEditorViewModel.Reimport` — no change.
- **ProjectEditorSettings missing or malformed**: silently degrade to app-level; log at `Warning`.
- **AppSettings missing or malformed**: silently degrade to hard-coded; log at `Warning`.
- **No suitable plugin found** (`ResolveForFile` returns null): behaves as today — file copied only, no .meta sidecar beyond the GUID one.

## Out of Scope (YAGNI)

- Drag-and-drop to a settings panel for live preview
- Settings diff view between layers (app / project / sidecar)
- Export/import settings profiles
- Per-asset bulk apply ("apply these settings to all selected")
- Auto-detection of natural-map / cubic-map without user confirmation
- Audio / Script plugins (they have no ContentTools cook step yet)
- **Built-in progress bar** in the import dialog (status-bar string sufficient — locked-in)
- **Migrating per-asset editor views** onto the new panels (separate future PR — locked-in)

## Testing Strategy

- **Unit**: `AssetImporterRegistry` resolution logic; `ImportSettingsDefaults` precedence chain with stub stores; `ImportSettingsDialog` accept/cancel paths using a windowless test host.
- **Integration**: `ContentBrowserViewModel.ImportAssetAsync` with mocked plugins and fake dialog result; verifies that each `IAssetImportSettings` reaches the matching plugin's `RunAsync`.
- **Native bridge**: smoke test that the new `ImportTexture(path, blob)` overload produces the same cooked artifact as `ImportTexture(path)` with default blob (regression guard).
- **Manual**: end-to-end import of a `.png` and a `.fbx`, verify dialog appears, verify .meta sidecar contains the chosen settings, verify cooked artifact loads in the engine.

## Risks & Open Questions

- **Engine ABI churn**: the new settings blobs must match `texture_import_settings` / `geometry_import_settings` C++ PODs exactly. Mitigation: ship a small round-trip test at startup that reads/writes a known-blob and compares bytewise.
- **Dialog UX for very long imports** ("Cooking 13000-face mesh..."): **status-bar string only** (decision: YAGNI — see locked-in decisions). The existing `ImportingStatus` field on `ContentBrowserViewModel` plus the `StatusMessage` summary at the bottom of the content browser carries the user-visible feedback.
- **Multiple dialogs queued** if user is in mid-pipeline with a mix of types: acceptable; document the order and a "Skip remaining" button on later dialogs as a future polish.
- **Should `ProjectEditorSettings` live in the project tree or in the user's settings dir?** Decision: in the project tree (`<project>/EditorSettings.json`) so it travels with the project on disk. App-level lives in `ApplicationData` as today.

## Rollout

- Land the registry + dialog + one plugin (Texture) behind an `#if false` flag if needed for safer review
- Then ship Mesh + PrimitiveMesh
- Then add the AppSettings UI surface (Settings dialog with project defaults) — separate ticket
