using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class PropertiesView : UserControl
{
    public PropertiesView()
    {
        InitializeComponent();
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }

    /// <summary>
    /// HDR environment map controls in the LIGHT section — delegate to the
    /// scene's <see cref="ViewModels.SceneEnvironmentViewModel"/> via
    /// <see cref="ViewModels.PropertiesViewModel.EnvMapPath"/>. Scene-level
    /// setting; the native push lives in SceneEnvironmentViewModel.
    /// </summary>
    /// <summary>
    /// Accept drops from the Content Browser onto the LIGHT section's HDR
    /// Env Map slot. Only equirectangular Radiance .hdr files are accepted —
    /// the engine loader (stbi_loadf) reads Radiance HDR, not cooked .texture
    /// artifacts or EXR.
    /// </summary>
    // Engine-cooked .texture (TEXR) and raw Radiance .hdr are both valid
    // environment sources — ForwardSceneRenderer::SetEnvironmentMap
    // dispatches on the extension.
    private static bool IsHdrEnvCandidate(string path) =>
        path.EndsWith(".hdr", System.StringComparison.OrdinalIgnoreCase) ||
        path.EndsWith(".texture", System.StringComparison.OrdinalIgnoreCase);

    private static string? ResolveHdrEnvPath(Avalonia.Input.DragEventArgs e)
    {
        var payload = AssetDropHandler.ReadPayload(e);
        // Resolve through the registry first. Content Browser payload paths
        // are an asset identity, not a promise that every target should use
        // the serialized string verbatim (container children are one example).
        var asset = payload == null ? null : AssetDropHandler.Resolve(payload);
        var raw = asset?.FullPath ?? payload?.FullPath ?? AssetDropHandler.FirstFilePath(e);
        if (string.IsNullOrEmpty(raw) || !IsHdrEnvCandidate(raw)) return null;
        var type = asset?.Type ?? ContentHelper.GetAssetType(raw);
        return type == AssetType.Texture ? raw : null;
    }

    private void OnLightEnvMapDragOver(object? sender, Avalonia.Input.DragEventArgs e)
    {
        var accepted = ResolveHdrEnvPath(e) != null;
        e.DragEffects = accepted
            ? Avalonia.Input.DragDropEffects.Copy
            : Avalonia.Input.DragDropEffects.None;
        // Highlight the slot while a droppable asset hovers so the user can
        // see the target.
        if (sender is Avalonia.Controls.Border zone)
        {
            zone.BorderBrush = accepted
                ? Avalonia.Media.Brushes.DodgerBlue
                : Avalonia.Media.Brushes.IndianRed;
        }
        e.Handled = true;
    }

    private void OnLightEnvMapDragLeave(object? sender, Avalonia.Input.DragEventArgs e)
    {
        if (sender is Avalonia.Controls.Border zone)
        {
            zone.BorderBrush = Avalonia.Media.Brush.Parse("#3A3F4B");
        }
        e.Handled = true;
    }

    private void OnLightEnvMapDrop(object? sender, Avalonia.Input.DragEventArgs e)
    {
        e.Handled = true;
        if (sender is Avalonia.Controls.Border zone)
        {
            zone.BorderBrush = Avalonia.Media.Brush.Parse("#3A3F4B");
        }
        if (DataContext is not ViewModels.PropertiesViewModel vm) return;
        var path = ResolveHdrEnvPath(e);
        if (path == null)
        {
            Logger.Warning("HDR Env Map: drop rejected — drag an .hdr asset from the Content Browser.");
            return;
        }
        vm.EnvMapPath = path;
        Logger.Info($"HDR Env Map set from drop: {path}");
    }

    // ---- PARTICLE FX 资产拖拽槽（同 HDR Env Map 槽的交互惯例） ----------------

    private static string? ResolvePfxEmitterPath(Avalonia.Input.DragEventArgs e)
    {
        var payload = AssetDropHandler.ReadPayload(e);
        var asset = payload == null ? null : AssetDropHandler.Resolve(payload);
        var raw = asset?.FullPath ?? payload?.FullPath ?? AssetDropHandler.FirstFilePath(e);
        if (string.IsNullOrEmpty(raw)) return null;
        return raw.EndsWith(".pfx_emitter", System.StringComparison.OrdinalIgnoreCase) ? raw : null;
    }

    private void OnPfxAssetDragOver(object? sender, Avalonia.Input.DragEventArgs e)
    {
        e.Handled = true;
        e.DragEffects = ResolvePfxEmitterPath(e) != null
            ? Avalonia.Input.DragDropEffects.Copy
            : Avalonia.Input.DragDropEffects.None;
    }

    private void OnPfxAssetDragLeave(object? sender, Avalonia.Input.DragEventArgs e)
    {
        e.Handled = true;
    }

    private void OnPfxAssetDrop(object? sender, Avalonia.Input.DragEventArgs e)
    {
        e.Handled = true;
        if (DataContext is not ViewModels.PropertiesViewModel vm) return;
        var path = ResolvePfxEmitterPath(e);
        if (path == null)
        {
            Logger.Warning("Particle FX: drop rejected — drag a .pfx_emitter asset from the Content Browser.");
            return;
        }
        vm.ParticleFXAssetPath = path;   // setter 会触发 PfxEmitterResyncCallback（重建发射器）
        Logger.Info($"Particle FX asset bound from drop: {path}");
    }

    private void OnPfxAssetClearClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not ViewModels.PropertiesViewModel vm) return;
        if (string.IsNullOrEmpty(vm.ParticleFXAssetPath)) return;
        vm.ParticleFXAssetPath = string.Empty;   // 触发发射器销毁
        Logger.Info("Particle FX asset binding cleared.");
    }

    private void OnLuaParamRemoveClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (sender is Avalonia.Controls.Button { Tag: ViewModels.ScriptParamRow row })
            (DataContext as ViewModels.PropertiesViewModel)?.LuaRemoveParam(row);
    }

    private async void OnBrowseLightEnvMapClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not ViewModels.PropertiesViewModel vm) return;
        var topLevel = TopLevel.GetTopLevel(this);
        if (topLevel == null) return;

        var files = await topLevel.StorageProvider.OpenFilePickerAsync(
            new Avalonia.Platform.Storage.FilePickerOpenOptions
            {
                Title = "Pick an equirectangular HDR environment map",
                AllowMultiple = false,
                FileTypeFilter = new[]
                {
                    new Avalonia.Platform.Storage.FilePickerFileType("HDR environment")
                    {
                        Patterns = new[] { "*.hdr", "*.texture" },
                    },
                    new Avalonia.Platform.Storage.FilePickerFileType("All files")
                    {
                        Patterns = new[] { "*" },
                    },
                },
            });
        if (files.Count == 0) return;
        vm.EnvMapPath = files[0].Path.LocalPath;
    }

    private void OnClearLightEnvMapClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is ViewModels.PropertiesViewModel vm) vm.EnvMapPath = string.Empty;
    }

    /// <summary>
    /// "..." button next to a MaterialOverride slot's Guid box: open a
    /// file picker pre-filtered to <c>.material</c> assets inside the
    /// project's content root, then bind the chosen file's GUID into
    /// the slot. We use Avalonia's StorageProvider rather than the
    /// editor's ContentBrowser VM because the Properties panel is
    /// always reachable — even with no project folder selected.
    /// </summary>
    private async void OnPickMaterial(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (sender is not Control host) return;
        var overrideSlot = host.DataContext as MaterialOverrideViewModel;
        var meshSlot = host.DataContext as MeshMaterialSlotViewModel;
        if (overrideSlot == null && meshSlot == null) return;

        var topLevel = TopLevel.GetTopLevel(this);
        if (topLevel == null) return;

        var project = ProjectContextLocator.Current;
        if (project == null)
        {
            Logger.Warning("OnPickMaterial: no project loaded");
            return;
        }

        var picker = new Avalonia.Platform.Storage.FilePickerOpenOptions
        {
            Title = "Choose material asset",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new Avalonia.Platform.Storage.FilePickerFileType("Material")
                {
                    Patterns = new[] { "*.material", "*.material.json", "*.mat" },
                },
            },
        };

        // Use the platform IStorageBrowserFactory to seed the start
        // folder; falls back silently if the headless / test profile
        // doesn't surface one. We don't block on failure — the user
        // can still navigate manually.
        try { picker.SuggestedStartLocation = await topLevel.StorageProvider.TryGetFolderFromPathAsync(new System.Uri(project.ContentPath)); }
        catch { /* picker works without a start folder */ }

        var result = await topLevel.StorageProvider.OpenFilePickerAsync(picker);
        if (result == null || result.Count == 0) return;
        var picked = result[0];
        if (picked == null) return;

        var path = picked.Path.LocalPath;
        // The Material asset's GUID comes from its .meta sidecar. If
        // the .meta doesn't exist yet (the user picked a fresh file),
        // AssetRegistry.Register will mint one. We register + look up.
        AssetRegistry.Register(path);
        var info = AssetRegistry.Get(path);
        if (info == null) return;
        if (meshSlot != null && DataContext is PropertiesViewModel properties)
        {
            properties.AssignMeshMaterial(meshSlot.Index, info);
            return;
        }

        overrideSlot!.MaterialGuid = info.Guid.ToString("D");
        // Flag the project so SaveAll actually writes the override.
        ProjectContextLocator.Current?.MarkDirty();
        ProjectContextLocator.Current?.ActiveScene?.MarkDirty();
    }

    private void OnMeshMaterialDragOver(object? sender, DragEventArgs e)
    {
        var info = ResolveDroppedAsset(e);
        var type = info == null
            ? AssetType.Unknown
            : info.Type != AssetType.Unknown
                ? info.Type
                : ContentHelper.GetAssetType(info.FullPath);
        e.DragEffects = type == AssetType.Material
            ? DragDropEffects.Copy
            : DragDropEffects.None;
        e.Handled = true;
    }

    private void OnMeshMaterialDrop(object? sender, DragEventArgs e)
    {
        if (sender is not Control { DataContext: MeshMaterialSlotViewModel slot }
            || DataContext is not PropertiesViewModel properties)
            return;

        var asset = ResolveDroppedAsset(e);
        if (asset != null) properties.AssignMeshMaterial(slot.Index, asset);
        e.Handled = true;
    }

    private void OnMeshAssetDragOver(object? sender, DragEventArgs e)
    {
        var info = ResolveDroppedAsset(e);
        var type = info == null ? AssetType.Unknown
            : info.Type != AssetType.Unknown ? info.Type : ContentHelper.GetAssetType(info.FullPath);
        e.DragEffects = type is AssetType.Mesh or AssetType.Model
            ? DragDropEffects.Copy
            : DragDropEffects.None;
        e.Handled = true;
    }

    private void OnMeshAssetDrop(object? sender, DragEventArgs e)
    {
        if (DataContext is PropertiesViewModel properties
            && ResolveDroppedAsset(e) is { } asset)
            properties.AssignMeshAsset(asset);
        e.Handled = true;
    }

    private async void OnPickMesh(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not PropertiesViewModel properties) return;
        var topLevel = TopLevel.GetTopLevel(this);
        var project = ProjectContextLocator.Current;
        if (topLevel == null || project == null) return;

        var picker = new Avalonia.Platform.Storage.FilePickerOpenOptions
        {
            Title = "Choose Mesh asset",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new Avalonia.Platform.Storage.FilePickerFileType("Mesh / Model")
                {
                    Patterns = new[] { "*.fbx", "*.obj", "*.gltf", "*.glb", "*.model", "*.model.json" },
                },
            },
        };
        try { picker.SuggestedStartLocation = await topLevel.StorageProvider.TryGetFolderFromPathAsync(new System.Uri(project.ContentPath)); }
        catch { }
        var result = await topLevel.StorageProvider.OpenFilePickerAsync(picker);
        if (result == null || result.Count == 0) return;
        var path = result[0].Path.LocalPath;
        AssetRegistry.Register(path);
        if (AssetRegistry.Get(path) is { } asset) properties.AssignMeshAsset(asset);
    }

    private void OnClearMesh(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is PropertiesViewModel properties) properties.ClearMeshAsset();
    }

    private void OnRevealMesh(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is PropertiesViewModel properties) properties.RevealMeshAsset();
    }

    private static AssetInfo? ResolveDroppedAsset(DragEventArgs e)
    {
        var payload = AssetDropHandler.ReadPayload(e);
        if (payload != null) return AssetDropHandler.Resolve(payload);

        var path = AssetDropHandler.FirstFilePath(e);
        if (string.IsNullOrWhiteSpace(path) || !System.IO.File.Exists(path)) return null;
        AssetRegistry.Register(path);
        return AssetRegistry.Get(path) ?? new AssetInfo
        {
            FullPath = path,
            Type = ContentHelper.GetAssetType(path),
        };
    }

    /// <summary>
    /// Enter key on a transform TextBox: move keyboard focus away so
    /// the TextBox's TwoWay binding commits the typed value to the
    /// source via the default LostFocus trigger. Escape does the same
    /// (reverts to the pre-edit source value).
    /// </summary>
    private void OnTransformTextBoxKeyDown(object? sender, Avalonia.Input.KeyEventArgs e)
    {
        if (sender is not TextBox tb) return;
        if (e.Key == Avalonia.Input.Key.Enter || e.Key == Avalonia.Input.Key.Escape)
        {
            // Moving focus to the parent panel triggers the TextBox's
            // LostFocus, which flushes the TwoWay binding. Enter
            // commits the new value; Escape reverts to the source
            // value (Avalonia's default LostFocus revert behaviour).
            this.Focus();
            e.Handled = true;
        }
    }

    /// <summary>
    /// "Look Through This Camera" / "Restore Editor Camera" button click.
    /// Toggles the viewport's render mode. The button only appears in
    /// the panel when a Camera entity is selected — see
    /// <see cref="PropertiesViewModel.HasCameraComponent"/>.
    /// </summary>
    private void OnLookThroughCameraClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not PropertiesViewModel vm) return;
        vm.OnLookThroughCameraClicked();
    }

    private void OnSetMainCameraClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is PropertiesViewModel vm) vm.OnSetMainCameraClicked();
    }

    /// <summary>
    /// B4 P3: open the (singleton) particle curve editor bound to the
    /// selected entity's emitter.
    /// </summary>
    private void OnOpenCurveEditorClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not PropertiesViewModel vm || vm.SelectedEntityNativeId == 0)
        {
            PrimalEditor_Avalonia.Common.Logger.Info("[Particle] curve editor: no emitter selected");
            return;
        }
        var lifetime = Avalonia.Application.Current?.ApplicationLifetime
            as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
        PrimalEditor_Avalonia.Editors.Particle.ParticleCurveEditorWindow.ShowFor(
            lifetime?.MainWindow, vm.SelectedEntityNativeId);
    }
}
