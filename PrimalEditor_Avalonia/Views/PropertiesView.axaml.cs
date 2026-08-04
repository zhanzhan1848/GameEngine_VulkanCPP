using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
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
        if (host.DataContext is not MaterialOverrideViewModel slot) return;

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
                    Patterns = new[] { "*.material", "*.mat" },
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
        slot.MaterialGuid = info.Guid.ToString("D");
        // Flag the project so SaveAll actually writes the override.
        ProjectContextLocator.Current?.MarkDirty();
        ProjectContextLocator.Current?.ActiveScene?.MarkDirty();
    }
}
