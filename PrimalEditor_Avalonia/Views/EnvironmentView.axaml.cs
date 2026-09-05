using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using Avalonia.Platform.Storage;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Views;

public partial class EnvironmentView : UserControl
{
    public EnvironmentView()
    {
        InitializeComponent();
        // GI 体积列表（plan §6.2 B1）随数据上下文就绪刷新一次，
        // 免去手动点 Refresh。
        DataContextChanged += (_, _) =>
            (DataContext as SceneEnvironmentViewModel)?.GiVolumes.Refresh();
    }

    private void InitializeComponent()
    {
        AvaloniaXamlLoader.Load(this);
    }

    private async void OnBrowseEnvMapClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not SceneEnvironmentViewModel vm || !vm.HasScene) return;
        var top = TopLevel.GetTopLevel(this);
        if (top == null) return;

        var files = await top.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Pick an equirectangular HDR environment map",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("HDR environment")
                {
                    Patterns = new[] { "*.hdr", "*.exr" },
                },
                new FilePickerFileType("All files")
                {
                    Patterns = new[] { "*" },
                },
            },
        });
        if (files.Count == 0) return;
        vm.EnvMapPath = files[0].Path.LocalPath;
    }

    private static bool IsHdrEnvCandidate(string path) =>
        path.EndsWith(".hdr", System.StringComparison.OrdinalIgnoreCase) ||
        path.EndsWith(".texture", System.StringComparison.OrdinalIgnoreCase);

    private static string? ResolveHdrEnvPath(string? raw)
    {
        if (string.IsNullOrEmpty(raw)) return null;
        return IsHdrEnvCandidate(raw) ? raw : null;
    }

    private void OnEnvMapDragOver(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (e is not Avalonia.Input.DragEventArgs de) return;
        var payload = PrimalEditor_Avalonia.Content.AssetDropHandler.ReadPayload(de);
        var path = payload?.FullPath ?? PrimalEditor_Avalonia.Content.AssetDropHandler.FirstFilePath(de);
        var accepted = ResolveHdrEnvPath(path) != null;
        de.DragEffects = accepted
            ? Avalonia.Input.DragDropEffects.Copy
            : Avalonia.Input.DragDropEffects.None;
        if (sender is Avalonia.Controls.Border zone)
        {
            zone.BorderBrush = accepted
                ? Avalonia.Media.Brushes.DodgerBlue
                : Avalonia.Media.Brushes.IndianRed;
        }
        de.Handled = true;
    }

    private void OnEnvMapDragLeave(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (sender is Avalonia.Controls.Border zone)
        {
            zone.BorderBrush = Avalonia.Media.Brush.Parse("#3A3F4B");
        }
        e.Handled = true;
    }

    private void OnEnvMapDrop(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (e is not Avalonia.Input.DragEventArgs de) return;
        de.Handled = true;
        if (sender is Avalonia.Controls.Border zone)
        {
            zone.BorderBrush = Avalonia.Media.Brush.Parse("#3A3F4B");
        }
        if (DataContext is not SceneEnvironmentViewModel vm) return;
        var payload = PrimalEditor_Avalonia.Content.AssetDropHandler.ReadPayload(de);
        var path = ResolveHdrEnvPath(payload?.FullPath ?? PrimalEditor_Avalonia.Content.AssetDropHandler.FirstFilePath(de));
        if (path == null)
        {
            PrimalEditor_Avalonia.Common.Logger.Warning("HDR Env Map: drop rejected — drag an .hdr asset from the Content Browser.");
            return;
        }
        vm.EnvMapPath = path;
    }

    private void OnClearEnvMapClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is not SceneEnvironmentViewModel vm) return;
        vm.EnvMapPath = string.Empty;
    }

    // GI VOLUMES（plan §6.2 B1 L1）：转发到 GiVolumeViewModel。
    private void OnGiVolumesRefreshClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is SceneEnvironmentViewModel vm) vm.GiVolumes.Refresh();
    }

    private void OnGiVolumesAppendClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is SceneEnvironmentViewModel vm) vm.GiVolumes.Append();
    }

    private void OnGiVolumesRemoveClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is SceneEnvironmentViewModel vm) vm.GiVolumes.Remove();
    }

    private void OnGiVolumesMaterializeClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is SceneEnvironmentViewModel vm) vm.GiVolumes.MaterializeDerived();
    }

    private void OnGiVolumesApplyClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        if (DataContext is SceneEnvironmentViewModel vm) vm.GiVolumes.Apply();
    }

    /// <summary>
    /// B1 L2: open the (singleton) DDGI probe inspector — SH9 swatches and
    /// confidence per probe, read back from the shared-memory DDGI buffers.
    /// </summary>
    private void OnProbeInspectorClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        var lifetime = Avalonia.Application.Current?.ApplicationLifetime
            as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
        PrimalEditor_Avalonia.Editors.Gi.ProbeInspectorWindow.ShowFor(lifetime?.MainWindow);
    }

    /// <summary>
    /// B2 增量: open the (singleton) fog shader editor — edit the Froxel MSL
    /// sources and hot-reload via the pass off→on rebuild.
    /// </summary>
    private void OnFogShaderClick(object? sender, Avalonia.Interactivity.RoutedEventArgs e)
    {
        var lifetime = Avalonia.Application.Current?.ApplicationLifetime
            as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime;
        PrimalEditor_Avalonia.Editors.Shaders.FogShaderEditorWindow.ShowFor(lifetime?.MainWindow);
    }
}
